// arch/x86_64/cpu/sched/runq.h
//
// Phase 20 — per-CPU runqueue.
//
// One `runq_t` lives inside every `percpu_t` at a fixed offset (see percpu.h).
// Each CPU runs schedule() against its local runq with a dedicated spinlock,
// eliminating the global `sched_lock` bottleneck that serialised every
// context switch pre-Phase-20. Work-stealing uses trylock on peer runqs
// (see work_steal.h).
//
// Two lists:
//   - ready_head / ready_tail / ready_count: FIFO. Enqueue at tail (O(1)
//     append), dequeue at head (O(1) pop). ready_count kept in sync so
//     work-stealing can snapshot-read it without the lock to pick a victim.
//   - starved_head: tasks whose cpu_budget_remaining_ns went ≤ 0 this epoch.
//     Refilled + drained by rlimit_epoch_tick at 1 Hz (sched_epoch_task on
//     CPU 0 sweeps every CPU's starved list).
//
// `current` is the task this CPU is running right now; NULL means the CPU
// is inside sched_idle (in schedule()'s hlt-loop). The IPI wakeup suppression
// check compares (rq->current == <that CPU's idle task>) to decide whether
// to send an IPI at all.
//
// Locking: a single per-runq `lock` protects all mutation. Lock ordering for
// work-stealing: thief holds NO lock, then trylocks victim. On success, the
// splice runs with only the victim's lock held; thief modifies its own runq
// directly after releasing victim. This keeps the hierarchy simple — at no
// point are two runq locks held simultaneously.
#pragma once

#include <stdint.h>

#include "../../../../kernel/sync/spinlock.h"

struct task_struct;

// Sentinel embedded in every runq for panic-time diagnosis. If the first
// word of a runq ever isn't RUNQ_MAGIC, percpu_t was scribbled on.
#define RUNQ_MAGIC 0xCAFE2010u

// Telemetry sentinel the scheduler installs when runq_init runs.
#define RUNQ_EPOCH_NEVER ((uint64_t)-1)

typedef struct runq {
    uint32_t magic;                       //  0..3    RUNQ_MAGIC canary
    uint32_t cpu_id;                      //  4..7    owning CPU (duplicated from percpu for fast access)

    struct task_struct *ready_head;       //  8..15   FIFO: dequeue here
    struct task_struct *ready_tail;       // 16..23   FIFO: enqueue here
    uint32_t ready_count;                 // 24..27   updated atomic-ish under lock; lockless snapshot ok
    uint32_t _pad0;                       // 28..31

    struct task_struct *starved_head;     // 32..39   rlimit_consume_cpu exhausted → here
    struct task_struct *current;          // 40..47   running task, or <per-CPU idle> when idle

    uint64_t steal_successes;             // 48..55   count of successful splices AS THIEF
    uint64_t steal_failures;              // 56..63   trylock-on-victim failures (back-off)
    uint64_t context_switches;            // 64..71   local context switch counter
    uint64_t last_epoch_tick_ticks;       // 72..79   g_timer_ticks at last epoch handled

    spinlock_t lock;                      // 80..127  ~48 bytes (SPINLOCK_INITIALIZER)

    // Phase 20 U14: this CPU's idle task. Set once at sched_init (BSP) /
    // sched_create_idle_for_cpu (APs). Used as the schedule()-fallback
    // when both the local runq is empty AND work-stealing failed. Must
    // never be NULL after the per-CPU idle is created — schedule() relies
    // on this to avoid two CPUs racing on `task_ptrs[0]` (BSP idle).
    struct task_struct *idle_task;        // 128..135
} runq_t;

_Static_assert(sizeof(runq_t) <= 192, "runq_t must fit in the 192-byte slot reserved in percpu_t");

// ---------------------------------------------------------------------------
// Lifecycle. Caller passes the runq (typically &percpu_get()->runq) and its
// CPU id. Zero-initialises fields, installs the magic, initialises the lock.
// ---------------------------------------------------------------------------
void runq_init(runq_t *rq, uint32_t cpu_id);

// ---------------------------------------------------------------------------
// Enqueue a READY task at the tail. Caller MUST hold rq->lock. Transitions
// `task->state` to TASK_STATE_READY as a side effect (so a STARVED task
// being drained back to ready doesn't need a separate state write).
// ---------------------------------------------------------------------------
void runq_enqueue_ready(runq_t *rq, struct task_struct *task);

// ---------------------------------------------------------------------------
// Pop the head of the ready list and return it. Caller MUST hold rq->lock.
// Returns NULL if the ready list is empty. Does NOT change task state —
// caller is responsible for transitioning to RUNNING.
// ---------------------------------------------------------------------------
struct task_struct *runq_dequeue_ready(runq_t *rq);

// ---------------------------------------------------------------------------
// Remove an arbitrary task from whichever list (ready or starved) it lives
// on. O(1) via task->runq_next/prev. Caller MUST hold rq->lock. Safe to
// call on a task that is NOT on any list (no-op in that case).
// ---------------------------------------------------------------------------
void runq_remove(runq_t *rq, struct task_struct *task);

// ---------------------------------------------------------------------------
// Move a task from the ready list to the starved head. Transitions state
// to TASK_STATE_STARVED. Caller MUST hold rq->lock.
// ---------------------------------------------------------------------------
void runq_move_to_starved(runq_t *rq, struct task_struct *task);

// ---------------------------------------------------------------------------
// Push a task onto the starved head directly. Intended for tasks that are
// not currently on any list (e.g. a RUNNING task whose CPU budget just
// exhausted — schedule() hands it to us). Transitions state to
// TASK_STATE_STARVED. Caller MUST hold rq->lock.
// ---------------------------------------------------------------------------
void runq_push_starved(runq_t *rq, struct task_struct *task);

// ---------------------------------------------------------------------------
// Sweep all starved tasks back to ready_tail in one pass. Transitions each
// task's state to TASK_STATE_READY. Caller MUST hold rq->lock. Returns the
// count drained.
// ---------------------------------------------------------------------------
uint32_t runq_refill_starved(runq_t *rq);
