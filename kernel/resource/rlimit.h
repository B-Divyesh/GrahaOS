// kernel/resource/rlimit.h
//
// Phase 20 — Per-task resource-limit accounting + enforcement.
//
// Three resources are tracked: memory pages, CPU time per 1-second epoch, and
// I/O bytes per second through the stream submit path. Each is per-task with
// a simple accounting field on task_t (see sched.h). Enforcement hooks are
// called from:
//   - rlimit_check_mem(task, npages):  before every vmm_map_page / pmm_alloc
//                                       on a user-attributable path (SYS_BRK
//                                       grow, vmo_map on-demand, COW fault).
//                                       Returns 0 if below limit (and
//                                       reserves the npages), -ENOMEM if over.
//   - rlimit_check_cpu(task, ns):      called from schedule() tick to
//                                       decrement cpu_budget_remaining_ns; if
//                                       ≤ 0, caller moves task to its runq's
//                                       starved_head.
//   - rlimit_check_io(task, bytes):    from stream_submit_batch; returns 0
//                                       (allowed, tokens consumed) or
//                                       -RLIMIT_EAGAIN (caller chains SQE on
//                                       task.io_pending_head).
//
// The per-epoch refresh ("every task gets its budget back + starved →
// ready") runs from sched_epoch_task pinned to CPU 0 at 1 Hz (see
// rlimit_epoch_tick).
//
// Pledge gating: SYS_SETRLIMIT is gated by PLEDGE_SYS_CONTROL regardless of
// direction (no self-lowering). SYS_GETRLIMIT is gated by PLEDGE_SYS_QUERY
// (default-granted). See AW-20.6.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct task_struct;   // forward — full definition in sched.h

// ---------------------------------------------------------------------------
// Resource identifiers (ABI: match syscall.h / user/syscalls.h).
// ---------------------------------------------------------------------------
#define RLIMIT_RES_MEM   1   // pages of 4 KiB each
#define RLIMIT_RES_CPU   2   // ns per 1 s epoch
#define RLIMIT_RES_IO    3   // bytes per second

// Default limits for newly-spawned user tasks. Kernel-internal threads
// (idle, epoch, FS workers, recluster) are created with all limits = 0
// (unlimited).
#define RLIMIT_DEFAULT_MEM_PAGES        131072ULL    // 512 MiB
#define RLIMIT_DEFAULT_CPU_BUDGET_NS    1000000000ULL // 100% of a 1 s epoch
#define RLIMIT_DEFAULT_IO_RATE_BPS      0ULL         // 0 = unlimited by default

// Epoch length. `sched_epoch_task` refills cpu_budget_remaining_ns every
// RLIMIT_EPOCH_NS nanoseconds. At 100 Hz tick frequency, this is exactly
// 100 ticks.
#define RLIMIT_EPOCH_NS                 1000000000ULL
#define RLIMIT_EPOCH_TICKS              100u

// Token-bucket refill granularity. At 100 Hz, each tick adds
// io_rate_bytes_per_sec / 100 to io_tokens, capped at io_rate_bytes_per_sec
// (== 1 s burst).
#define RLIMIT_IO_REFILL_DIVISOR        100u

// Soft cap on total live tasks. Spawn beyond returns -EAGAIN. 10 240 × ~2580 B
// ≈ 25 MiB max task_t footprint — fits in a 2 GiB QEMU RAM default.
#define RLIMIT_MAX_TASKS                10240u

// Internal errno used by rlimit_check_io to signal "chain on pending list,
// don't reject". Distinct from user-visible -EAGAIN; never reaches userspace.
#define RLIMIT_EAGAIN_INTERNAL          -900

// ---------------------------------------------------------------------------
// rlimit_violation_t — compact record emitted to the audit log when a limit
// is enforced. Carried inline in the 192-byte audit detail field.
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t timestamp_ns;   // epoch ns at violation
    int32_t  pid;            // offending task PID (-1 = kernel-internal)
    uint8_t  resource;       // RLIMIT_RES_*
    uint8_t  _pad[3];
    uint64_t limit;          // the configured cap
    uint64_t attempted;      // the value that would have exceeded it
    uint64_t rip;            // instruction pointer at detection (0 if unknown)
} rlimit_violation_t;

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Initialize a freshly-allocated task's resource-limit fields from the parent
// (if non-NULL) or system defaults. Called from sched_create_task /
// sched_create_user_process / sys_spawn.
void rlimit_init_defaults(struct task_struct *child, const struct task_struct *parent);

// ---------------------------------------------------------------------------
// Enforcement hot paths.
// ---------------------------------------------------------------------------

// Check and reserve `npages` against task's mem_limit_pages. If task has no
// limit (mem_limit_pages == 0), always returns 0 and increments mem_pages_used.
// If the request would exceed the limit, emits AUDIT_RLIMIT_MEM and returns
// -ENOMEM; mem_pages_used is NOT incremented. On success, mem_pages_used is
// incremented atomically.
int rlimit_check_mem(struct task_struct *t, uint64_t npages);

// Release `npages` back to the task's mem-pages counter. Called from SYS_BRK
// shrink, vmo_unmap, COW old-page unref. Must mirror rlimit_check_mem's
// increment; never causes audit events.
void rlimit_account_free_mem(struct task_struct *t, uint64_t npages);

// Subtract `ns` from cpu_budget_remaining_ns. Caller must check returned value:
//   > 0  : budget remaining, task keeps running
//   ≤ 0  : budget exhausted this epoch, caller must move task to starved_head.
// If task has no CPU limit (cpu_time_slice_budget_ns == 0), returns a large
// positive value unchanged.
int64_t rlimit_consume_cpu(struct task_struct *t, uint64_t ns);

// Check and consume `bytes` from the I/O token bucket. Returns:
//    0                       : allowed, tokens deducted
//   -RLIMIT_EAGAIN_INTERNAL  : insufficient tokens, caller chains SQE on
//                              task.io_pending_head
// If task has no I/O limit (io_rate_bytes_per_sec == 0), always returns 0.
int rlimit_check_io(struct task_struct *t, uint64_t bytes);

// Top up the task's I/O tokens by io_rate_bytes_per_sec / RLIMIT_IO_REFILL_DIVISOR
// (capped at bucket capacity = io_rate_bytes_per_sec). Called per tick from
// schedule() for the currently-running task. Also drains io_pending_head while
// tokens are sufficient.
void rlimit_refill_io_tokens(struct task_struct *t);

// ---------------------------------------------------------------------------
// Epoch handler. Runs on CPU 0's sched_epoch_task at 1 Hz (g_timer_ticks %
// RLIMIT_EPOCH_TICKS == 0). Walks task_global_list:
//   - refills every task's cpu_budget_remaining_ns = cpu_time_slice_budget_ns
//   - for each per-CPU runq (in cpu_id order), locks, drains starved_head →
//     ready_tail, unlocks
// Emits AUDIT_SCHED_EPOCH once. Yields every 256 tasks via
// asm("pause") to avoid holding the tick too long.
// ---------------------------------------------------------------------------
void rlimit_epoch_tick(void);

// ---------------------------------------------------------------------------
// SYS_SETRLIMIT / SYS_GETRLIMIT backends.
// ---------------------------------------------------------------------------

// Set the named resource on `target` to `value`. Pledge check is done by the
// syscall dispatcher BEFORE this call. Returns 0 / -EINVAL.
int rlimit_set(struct task_struct *target, uint32_t resource, uint64_t value);

// Read the named resource's current value on `target`. Returns 0 and writes
// to *out, or -EINVAL if resource unknown.
int rlimit_get(const struct task_struct *target, uint32_t resource, uint64_t *out);

// ---------------------------------------------------------------------------
// Telemetry helpers.
// ---------------------------------------------------------------------------

// Incremented by spawn when a task is created, decremented by sched_reap_zombie.
// Readable via psinfo. Soft-capped at RLIMIT_MAX_TASKS; spawn returns -EAGAIN
// past the cap.
extern uint64_t g_task_count;
