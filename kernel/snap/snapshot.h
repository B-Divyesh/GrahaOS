// kernel/snap/snapshot.h
// Phase 24: COW Snapshot Subsystem — public header.
//
// A snapshot is a coherent capture, taken at a single scheduler-barrier
// instant, of every in-scope task's page tables, every VMO those tasks
// hold, every GrahaFS v2 version-chain head referenced by their open
// inodes, and every channel endpoint inside the snapshot scope. Creation
// is O(1) in the unchanged-memory case: PTEs flip to read-only on both
// parent and snapshot image, and subsequent writes trigger the COW
// page-fault handler.
//
// W13 (skeleton): only types + extern prototypes are defined here; the
// real implementations live in W14 (snap_create), W15 (cow_fault), W16
// (snap_restore), W17 (snap_delete + fs pins), W18 (chan_freeze), W19
// (syscall entry points).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../cap/token.h"
#include "../sync/spinlock.h"

// ---------------------------------------------------------------------------
// Scope flags (bitfield for SYS_SNAP_CREATE.flags / snapshot_t.scope_flags).
// ---------------------------------------------------------------------------
#define SNAP_SCOPE_SELF             0x00000001u   // Caller's process tree only
#define SNAP_SCOPE_GLOBAL           0x00000002u   // Every task (requires CAP_KIND_SYSTEM)
#define SNAP_SCOPE_FREEZE_ALL_CHANS 0x00000004u   // Freeze even out-of-scope-peer chans
#define SNAP_SCOPE_VALID_MASK       (SNAP_SCOPE_SELF | \
                                     SNAP_SCOPE_GLOBAL | \
                                     SNAP_SCOPE_FREEZE_ALL_CHANS)

// ---------------------------------------------------------------------------
// Lifecycle states (snapshot_t.state).
// ---------------------------------------------------------------------------
#define SNAP_STATE_ACTIVE     1u
#define SNAP_STATE_RESTORING  2u
#define SNAP_STATE_DELETED    3u

// ---------------------------------------------------------------------------
// Channel-capture mode (snapshot_chan_entry_t.mode).
// ---------------------------------------------------------------------------
#define SNAP_CHAN_FROZEN   1u   // Both endpoints in scope; entire channel paused
#define SNAP_CHAN_DRAINED  2u   // External peer; messages serialised into drain_vmo

// ---------------------------------------------------------------------------
// Limits (compile-time guards used by snap_create's allocation paths).
// ---------------------------------------------------------------------------
#define SNAP_MAX_LIVE         128u   // System-wide concurrent live snapshots
#define SNAP_NAME_MAX_LEN     31u    // + 1 for null terminator
#define SNAP_BARRIER_TIMEOUT_NS  100000000ULL  // 100 ms watchdog (AW-24.4)

// ---------------------------------------------------------------------------
// snap_captured_page_t — one record per user-half PTE captured at snap time.
// W14.4 walks each in-scope task's PML4 user-half; for every present PTE
// it bumps cow_page_tracker, bumps pmm_page_ref, marks the PTE read-only,
// and records the (virt, phys, pre-RO flags) here. snap_restore (W16) walks
// these to re-install the page mappings. snap_delete (W17.1) walks them
// to drop the cow_page_tracker + pmm_page_ref.
// ---------------------------------------------------------------------------
typedef struct snap_captured_page {
    uint64_t virt;     // Virtual address (4 KiB aligned).
    uint64_t phys;     // Physical address (4 KiB aligned).
    uint64_t flags;    // Original PTE flags at capture time (writable bit
                       // preserved here so snap_restore can put it back).
} snap_captured_page_t;

// ---------------------------------------------------------------------------
// snapshot_task_entry_t — per-task capture (regs, CR3, FD-table copy).
// ---------------------------------------------------------------------------
// The real arch_regs_t is `struct interrupt_frame`; we forward-declare so
// callers do not need the arch header.
struct interrupt_frame;
struct fd_table;       // Per-task FD-table copy; concrete shape is W14 work.

// Cap on per-task captured pages. A 4 MiB user image is 1024 pages; 4096
// covers ~16 MiB which is well above any user-mode test program. Going
// over the cap aborts the capture with -ENOMEM and the barrier is
// released cleanly.
#define SNAP_PAGES_PER_TASK_MAX  4096u

typedef struct snapshot_task_entry {
    int32_t  pid;                       // Original PID at capture time.
    uint64_t cr3_original;              // Parent's PML4 phys at capture.
    uint64_t cr3_snapshot;              // Snapshot-side CR3 (== cr3_original
                                        // in v1; reserved for divergent
                                        // page-table cloning in a future
                                        // revision).
    struct interrupt_frame *regs;       // Captured register file (kheap-owned).
    struct fd_table        *fd_table_copy;  // Deep copy of FD table at capture.
    uint64_t pledge_snapshot;           // Pledge bitmap at capture.
    // W14.4 page captures.
    snap_captured_page_t *pages;        // kheap array; NULL if empty.
    uint32_t              page_count;   // Used entries in pages[].
    uint32_t              page_cap;     // Allocated entries in pages[].
} snapshot_task_entry_t;

// ---------------------------------------------------------------------------
// snapshot_vmo_entry_t — captured VMO ref + version stamp.
// ---------------------------------------------------------------------------
typedef struct snapshot_vmo_entry {
    uint64_t vmo_id;                    // Phase-17 VMO handle.
    uint32_t captured_version;          // VMO's monotonic version at capture.
    uint32_t page_count;                // Pages in VMO at capture.
} snapshot_vmo_entry_t;

// ---------------------------------------------------------------------------
// snapshot_chan_entry_t — captured channel endpoint, frozen or drained.
// ---------------------------------------------------------------------------
typedef struct snapshot_chan_entry {
    uint64_t chan_id;                   // Phase-17 channel ID.
    uint32_t mode;                      // SNAP_CHAN_FROZEN | SNAP_CHAN_DRAINED
    uint64_t drain_vmo;                 // VMO holding serialised in-flight msgs.
    uint32_t queue_head_at_snap;        // Ring-buffer head index captured.
    uint32_t queue_tail_at_snap;        // Ring-buffer tail index captured.
} snapshot_chan_entry_t;

// ---------------------------------------------------------------------------
// snapshot_fs_pin_t — per-inode FS version-chain pin.
// ---------------------------------------------------------------------------
typedef struct snapshot_fs_pin {
    uint64_t inode_id;                  // GrahaFS v2 inode number.
    uint64_t pinned_version_id;         // Specific version that was HEAD at capture.
} snapshot_fs_pin_t;

// ---------------------------------------------------------------------------
// cow_page_tracker_t — refcount record per shared physical page (W15).
// Linked into a hash bucket keyed on (phys_page >> 12) & (COW_HASH_BUCKETS-1).
// ---------------------------------------------------------------------------
struct cow_page_tracker;
typedef struct cow_page_tracker {
    uint64_t phys_page;                 // Physical page address (4 KiB aligned).
    uint32_t refcount;                  // Count of references; 0 → freed.
    uint64_t owner_snap_id;             // Smallest snapshot ID holding it (or 0).
    struct cow_page_tracker *next;      // Next entry in hash bucket chain.
} cow_page_tracker_t;

// ---------------------------------------------------------------------------
// snap_barrier_state_t — global scheduler-barrier coordination (W14).
// barrier_flag is the only volatile-atomic field; the rest are protected
// by snap_barrier_state_t.lock.
// ---------------------------------------------------------------------------
struct task_struct;
typedef struct snap_barrier_state {
    volatile uint32_t barrier_flag;     // 0 = normal, 1 = snapshot in progress.
    volatile uint32_t acks;             // Per-CPU ack count (info only).
    uint64_t          barrier_entered_tsc;  // rdtsc at begin; for 100 ms watchdog.
    spinlock_t        lock;             // Protects parked_head + owner + seq.
    struct task_struct *parked_head;    // Singly-linked via task->barrier_next.
    struct task_struct *owner_task;     // The snap_create caller (skip-park).
    uint32_t           barrier_seq;     // Bumped each begin; per-CPU dedup.
} snap_barrier_state_t;

// ---------------------------------------------------------------------------
// snapshot_t — top-level record (slab-allocated from snap_cache).
// ---------------------------------------------------------------------------
typedef struct snapshot {
    uint64_t                  id;
    cap_token_t               cap_token;
    uint64_t                  created_utc_ns;
    int32_t                   creator_pid;
    uint32_t                  scope_flags;

    snapshot_task_entry_t    *tasks;
    uint32_t                  task_count;

    snapshot_vmo_entry_t     *vmos;
    uint32_t                  vmo_count;

    snapshot_fs_pin_t        *fs_pins;
    uint32_t                  fs_pin_count;

    snapshot_chan_entry_t    *chans;
    uint32_t                  chan_count;

    uint64_t                  pages_shared;
    uint64_t                  pages_diverged;

    uint32_t                  state;
    char                      name[SNAP_NAME_MAX_LEN + 1];

    // Linkage in the global live-snapshot list (g_snap_live_head). NULL if
    // this is the tail or the snapshot has been removed.
    struct snapshot          *next;
    struct snapshot          *prev;
} snapshot_t;

// ---------------------------------------------------------------------------
// snap_info_t — record returned by SYS_SNAP_LIST. Stable layout for ABI.
// ---------------------------------------------------------------------------
typedef struct snap_info {
    uint64_t id;                        //  0
    uint64_t created_utc_ns;            //  8
    int32_t  creator_pid;               // 16
    uint32_t scope_flags;               // 20
    uint32_t state;                     // 24
    uint32_t task_count;                // 28
    uint32_t vmo_count;                 // 32
    uint32_t chan_count;                // 36
    uint64_t pages_shared;              // 40
    uint64_t pages_diverged;            // 48
    char     name[SNAP_NAME_MAX_LEN + 1];  // 56..87
} snap_info_t;

_Static_assert(sizeof(snap_info_t) == 88, "snap_info_t must be 88 bytes (stable ABI)");

// ---------------------------------------------------------------------------
// Globals (defined in snapshot.c).
// ---------------------------------------------------------------------------
extern snap_barrier_state_t g_snap_barrier;
extern uint64_t             g_snap_next_id;     // Monotonic ID generator.
extern uint32_t             g_snap_live_count;  // Number of live snapshots.

// ---------------------------------------------------------------------------
// Lifecycle / API. W13 ships only snap_init + ENOSYS stubs; the real
// implementations land in W14-W19.
// ---------------------------------------------------------------------------

// One-time initialiser: registers slab caches for snapshot_t and
// cow_page_tracker_t, zeroes g_snap_barrier, seeds g_snap_next_id from
// the boot RTC. Called from kernel/main.c after kheap_init() and
// cap_object_init(), before scheduler_start().
void snap_init(void);

// Capture a coherent system snapshot under the caller's scope.
// On success returns the cap_handle slot installed in the caller's
// handle table; on error, a negative -errno (EINVAL, EPERM, ENOMEM,
// EBUSY, ETIME). W14 implementation; W13 stub returns -ENOSYS.
int snap_create(uint32_t scope_flags, const char *name);

// Phase 25 — kernel-internal entry. Same capture semantics as snap_create
// but does NOT create a cap_object + does NOT insert a handle into the
// caller's table. Returns 0 on success with *out_snap pointing at the
// fresh snapshot_t (already linked into g_snap_live_head); negative
// -errno on failure. Used by txn_begin which manages its own
// CAP_KIND_TRANSACTION token rather than handing the caller a separate
// CAP_KIND_SNAPSHOT.
int snap_create_internal(uint32_t scope_flags, const char *name,
                         struct snapshot **out_snap);

// Phase 25 — kernel-internal counterpart of snap_delete.
// Drops the snapshot's live-list entry, reclaims captured pages via
// snap_destroy_captures, and frees the body. Caller has already
// dropped any cap_handle / cap_object linkage (txn_commit /
// txn_abort do this themselves).
void snap_destroy_internal(struct snapshot *s);

// Phase 25 — kernel-internal counterpart of snap_restore.
// Identical to the syscall path but operates on a snapshot_t* rather
// than a cap_handle slot. Caller must hold the snapshot alive.
int snap_restore_internal(struct snapshot *s, struct task_struct *caller);

// Atomically restore system state to the snapshot referenced by the
// caller's handle. W16 implementation; W13 stub returns -ENOSYS.
int snap_restore(uint32_t handle);

// Release the snapshot and reclaim any uniquely-referenced pages.
// W17 implementation; W13 stub returns -ENOSYS.
int snap_delete(uint32_t handle);

// Enumerate live snapshots into the caller's buffer. Returns the
// number of records written, or -errno. W19 implementation; W13 stub
// returns -ENOSYS.
int snap_list(snap_info_t *user_buf, size_t count);

// ---------------------------------------------------------------------------
// Future surfaces (declarations only; W14-W18 will provide bodies).
// ---------------------------------------------------------------------------

// Barrier coordination (W14).
int  snap_begin_barrier(void);
void snap_end_barrier(void);

// W14.2 helper: schedule() calls this with the running task to flip its
// state to TASK_STATE_BARRIER_WAIT and link it into the parked list. The
// caller (sched.c) holds no scheduler locks at the call site (parks via
// the barrier lock). Defined in kernel/snap/barrier.c.
void snap_barrier_park_locked(struct task_struct *cur);

// W14.3-W14.7 capture orchestrator + rollback (defined in capture.c).
// snap_run_capture is invoked by snap_create after snap_begin_barrier;
// snap_destroy_captures rolls back every capture-side ref taken by
// snap_run_capture (drops cow trackers + pmm refs + FS pins). Both are
// safe to call multiple times — the second call is a no-op once the
// arrays have been freed.
int  snap_run_capture(struct snapshot *snap, struct task_struct *self);
void snap_destroy_captures(struct snapshot *snap);

// Pre-Phase-28 sweep B.3 (FU25.A.3) — append a single (inode, version)
// pin to a SNAP_STATE_ACTIVE snapshot's fs_pins[] mid-flight. Used by the
// SYS_TXN_PIN_PATH syscall so userspace can request a pin captured for
// files opened-then-written-then-closed inside a txn body (the original
// snap_capture_fs_pins_for_task only walks open FDs at txn_begin time).
// Lazy-allocates the fs_pins array on first call; calls grahafs_pin_
// version internally so the version_entry is held alive until snap_
// destroy_captures unwinds it. Returns 0 on success, -CAP_E2BIG if the
// snapshot is full, -CAP_EINVAL on bad inputs.
int snap_add_fs_pin(struct snapshot *snap, uint64_t inode_id,
                    uint64_t version_id);

// COW page-fault entry point (W15). Called from arch/x86_64/mm/vmm.c
// when error_code indicates "write to present, read-only page".
int  cow_fault_handle(uint64_t fault_addr, uint64_t error_code,
                      struct interrupt_frame *regs);

// COW-tracker refcount API (W15).
cow_page_tracker_t *cow_page_tracker_get(uint64_t phys);
void                cow_page_tracker_put(uint64_t phys);
void                cow_page_tracker_bump(uint64_t phys);

// Channel freeze/thaw/drain helpers (W18).
int  chan_freeze(uint64_t chan_id, uint64_t snap_id);
int  chan_thaw(uint64_t chan_id, uint32_t head, uint32_t tail);
int  chan_drain_to_vmo(uint64_t chan_id, uint64_t vmo_id);
int  chan_redrain_from_vmo(uint64_t chan_id, uint64_t vmo_id);
