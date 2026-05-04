// kernel/txn/transaction.h — Phase 25 transactional speculation.
//
// A transaction wraps a Phase-24 snapshot with two extras:
//   1. An active_txn frame on the caller's task — every chan_send while
//      the txn is active checks this frame and may intercept the send
//      into the txn's per-txn buffer (Stage E adds the interception).
//   2. A buffered-message ring inside a kernel-only VMO (Stage E adds
//      the buffer; Stage D leaves it NULL until first external send).
//
// State machine (per spec):
//   ACTIVE   --> COMMITTING                          (txn_commit start)
//   COMMITTING --> {COMMITTED, COMMITTING_FAILED}    (replay outcome)
//   COMMITTING_FAILED --> {COMMITTING, ABORTING}     (retry / give up)
//   ACTIVE   --> ABORTING                            (direct abort)
//   ABORTING --> ABORTED                             (abort done)
//
// Plan-agent Q6: state transitions are atomic CAS on transaction_t.state.
// A per-txn waitqueue (state_waiters / state_waitq_head) lets abort sleep
// while a concurrent commit replay is in flight; the commit's terminal
// CAS wakes the queue.
//
// Stage D ships the types + lifecycle stubs (txn_commit returns -ENOSYS
// when the buffer is non-empty; Stage F lands the real replay).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../sync/spinlock.h"

// Forward declarations to avoid pulling in heavy headers.
struct snapshot;
struct task_struct;
struct vmo;

// ---------------------------------------------------------------------------
// Limits + constants.
// ---------------------------------------------------------------------------
#define TXN_MAX_NESTING            4u    // depth limit per task
#define TXN_MAX_SCOPE_TASKS        256u  // sorted PID array bound (Q2)
#define TXN_DEFAULT_BUFFER_BYTES   (4u * 1024u * 1024u)  // 4 MiB
#define TXN_NAME_MAX_LEN           31u   // matches snapshot_t.name

// Buffered-message frame magic (Plan agent Q5: head + tail magic catch
// off-by-one length bugs during replay parsing).
#define TXN_BUFFER_MAGIC_HEAD      0xBEADF00Du
#define TXN_BUFFER_MAGIC_TAIL      0xDEADC0DEu

// ---------------------------------------------------------------------------
// Flags (passed to SYS_TXN_BEGIN; preserved on transaction_t.flags).
// ---------------------------------------------------------------------------
#define TXN_FLAG_SELF_SCOPE        0x00000001u
#define TXN_FLAG_GLOBAL_SCOPE      0x00000002u  // requires CAP_KIND_SYSTEM
#define TXN_FLAG_BUFFER_2MB        0x00000010u
#define TXN_FLAG_BUFFER_4MB        0x00000020u  // default
#define TXN_FLAG_BUFFER_8MB        0x00000040u
#define TXN_FLAG_VALID_MASK        (TXN_FLAG_SELF_SCOPE   | \
                                    TXN_FLAG_GLOBAL_SCOPE | \
                                    TXN_FLAG_BUFFER_2MB   | \
                                    TXN_FLAG_BUFFER_4MB   | \
                                    TXN_FLAG_BUFFER_8MB)

// ---------------------------------------------------------------------------
// State machine (spec-mandated).
// ---------------------------------------------------------------------------
typedef enum txn_state {
    TXN_STATE_INVALID           = 0,
    TXN_STATE_ACTIVE            = 1,
    TXN_STATE_COMMITTING        = 2,
    TXN_STATE_COMMITTED         = 3,
    TXN_STATE_COMMITTING_FAILED = 4,
    TXN_STATE_ABORTING          = 5,
    TXN_STATE_ABORTED           = 6,
} txn_state_t;

// ---------------------------------------------------------------------------
// Buffered message header (Stage E ring-buffer entry prefix).
// On disk: [TXN_BUFFER_MAGIC_HEAD u32]
//          [target_chan_id u64] [payload_len u32] [flags u32]
//          [original_send_seq u64]
//          [payload bytes, ROUND_UP_8]
//          [TXN_BUFFER_MAGIC_TAIL u32]
// ---------------------------------------------------------------------------
typedef struct buffered_msg_header {
    uint32_t magic;              // 0xBEADF00D — head sentinel
    uint32_t _pad0;              // alignment
    uint64_t target_chan_id;     // c->id at send time (resolved at replay)
    uint32_t payload_len;        // bytes after header, before tail magic
    uint32_t flags;              // original send flags / kind tag
    uint64_t original_send_seq;  // monotonic per-txn
} buffered_msg_header_t;

_Static_assert(sizeof(buffered_msg_header_t) == 32,
               "buffered_msg_header_t must be 32 bytes");

// ---------------------------------------------------------------------------
// transaction_t: the kernel-side per-txn record. Slab-allocated.
// ---------------------------------------------------------------------------
typedef struct transaction {
    uint64_t id;                       // monotonic; 0 reserved
    uint32_t flags;                    // TXN_FLAG_* mask
    uint32_t cap_object_idx;           // CAP_KIND_TRANSACTION token slot
    int32_t  creator_pid;              // owner; only this task may commit/abort
                                       // unless a sub-token is delegated
    uint32_t nesting_depth;            // 0=outermost; <TXN_MAX_NESTING

    struct snapshot   *backing_snapshot;     // implicit snap (kernel-internal)
    struct transaction *parent_txn;          // outer txn, or NULL

    // Buffered messages (Stage E populates).
    struct vmo *buffer_vmo;            // NULL until first external send
    uint32_t   buffer_vmo_capacity;    // configured at begin (per FLAG_BUFFER_*)
    uint32_t   buffer_vmo_head;        // write offset within buffer_vmo
    uint32_t   buffered_count;         // records written

    // Scope membership oracle (Plan-agent Q2). PIDs in scope_pids[] are
    // sorted ascending; binary search at chan_send-time decides whether
    // a peer is in-scope. Array bound at TXN_MAX_SCOPE_TASKS.
    int32_t  scope_pids[TXN_MAX_SCOPE_TASKS];
    uint32_t scope_pid_count;

    // State machine.
    volatile txn_state_t state;        // updated via __atomic_compare_exchange
    uint32_t replay_cursor;            // byte offset for retry
    uint32_t replay_delivered;         // count successfully replayed so far
    uint64_t replay_failed_chan_id;    // 0 if replay still pending

    // Per-txn waitqueue for abort-vs-commit serialisation (Plan-agent Q6).
    // Singly-linked list of task_t * blocked on this txn's state change;
    // head set/cleared under state_waitq_lock. Stage F populates.
    struct task_struct *state_waitq_head;
    spinlock_t          state_waitq_lock;

    // Audit / introspection.
    uint64_t begun_ns;                 // wall-clock ns at txn_begin
    char     name[TXN_NAME_MAX_LEN + 1];

    // Live-list linkage (g_txn_live_head, protected by g_txn_live_lock).
    struct transaction *next;
    struct transaction *prev;
} transaction_t;

// ---------------------------------------------------------------------------
// task_txn_frame_t: per-task transaction stack. Embedded in task_struct.
// ---------------------------------------------------------------------------
typedef struct task_txn_frame {
    transaction_t *current;            // innermost active txn; NULL if none
    uint32_t       stack_depth;        // 0..TXN_MAX_NESTING
    uint32_t       reserved;           // future flags / padding
} task_txn_frame_t;

// ---------------------------------------------------------------------------
// txn_replay_context_t: Stage F replay state. Stack-allocated by txn_commit.
// ---------------------------------------------------------------------------
typedef struct txn_replay_context {
    transaction_t *txn;
    uint32_t       current_offset;
    uint32_t       delivered_count;
    uint64_t       failed_chan_id;
} txn_replay_context_t;

// ---------------------------------------------------------------------------
// Lifecycle API.
// ---------------------------------------------------------------------------

// Boot init: slab cache + globals. Called from kmain after snap_init.
void txn_init(void);

// SYS_TXN_BEGIN handler. Returns cap_handle slot ≥ 0 on success;
// negative -errno on failure.
int txn_begin(uint32_t flags, const char *name, struct task_struct *caller);

// SYS_TXN_COMMIT handler. Returns 0 on full commit, -ETXNREPLAY mid-replay
// (caller may retry via SYS_TXN_COMMIT again or fall back to abort).
int txn_commit(uint32_t handle, struct task_struct *caller);

// SYS_TXN_ABORT handler. Returns 0 on successful abort.
int txn_abort(uint32_t handle, struct task_struct *caller);

// SYS_TXN_LIST handler — Phase 25 introspection.
struct snap_info_user;  // forward
int txn_list(struct snap_info_user *out_buf, uint32_t max);

// task_exit hook: walk the task's active_txn stack outermost-last,
// txn_force_drop each entry. Called from sched.c::task_exit.
void txn_task_exit_cleanup(struct task_struct *dying);

// Diagnostic: dump current txn state to klog.
void txn_dump(transaction_t *t);

// ---------------------------------------------------------------------------
// Stage E hooks (defined here so chan_send can find them).
// ---------------------------------------------------------------------------

// True iff the channel's peer endpoint's holder PID is NOT in t's scope.
// Resolves the OTHER endpoint of c against cap_object[*].current_holder_pid.
// Returns true (treat as external, buffer) for orphaned endpoints.
struct channel;
bool txn_is_external_peer(struct channel *c, transaction_t *t,
                          struct task_struct *sender);

// Append a buffered message to t's ring (lazy-alloc the VMO on first call).
// Stage E lands the actual implementation; Stage D's stub returns -ENOSYS.
struct channel_msg;
int txn_buffer_append(transaction_t *t, uint64_t target_chan_id,
                      const struct channel_msg *msg, uint32_t flags);

// Drop the buffer VMO + decrement any per-channel internal refcounts.
// Called from txn_commit (post-replay), txn_abort, txn_force_drop.
void txn_buffer_free_drop(transaction_t *t);

// Iterator (Stage F replay walker). _init resets ctx; _next pulls one
// record per call. Returns 0 on success; -ENODATA(-61) at end of buffer;
// TXN_EINVAL on magic mismatch. The replay engine in kernel/txn/replay.c
// is the primary consumer.
struct channel_msg;
void txn_buffer_iter_init(transaction_t *t, txn_replay_context_t *ctx);
int  txn_buffer_iter_next(txn_replay_context_t *ctx,
                          buffered_msg_header_t *out_hdr,
                          struct channel_msg *out_payload,
                          uint32_t *out_next_offset);

// ---------------------------------------------------------------------------
// Stage F replay engine. Lands in kernel/txn/replay.c.
// ---------------------------------------------------------------------------

// Drain every buffered record into its target channel via chan_send. Sets
// caller->replay_in_progress=1 across the loop so the prologue doesn't
// re-buffer (Plan-agent Q3). Returns 0 on full drain; TXN_ETXNREPLAY on
// stall (caller may retry SYS_TXN_COMMIT or fall back to abort).
int  txn_replay_all(transaction_t *t, txn_replay_context_t *ctx,
                    struct task_struct *caller);

// Same loop starting from ctx->current_offset (caller preserves it for
// retry). Used by gate test txn_commit_retry.
int  txn_replay_resume(transaction_t *t, txn_replay_context_t *ctx,
                       struct task_struct *caller);

// Emit AUDIT_TXN_PARTIAL_EXTERNAL with the (delivered, remaining) split.
// Fired from txn_abort + txn_force_drop when a partial replay has already
// put messages on the wire that abort can't recall.
void txn_replay_rollback_warning(transaction_t *t, int32_t caller_pid,
                                 uint32_t delivered, uint32_t remaining,
                                 uint8_t force_drop);

// ---------------------------------------------------------------------------
// Errno-style returns. Negative; the syscall dispatcher propagates them.
// ---------------------------------------------------------------------------
#define TXN_OK                  0
#define TXN_EINVAL             -22
#define TXN_EPERM              -1
#define TXN_ENOMEM             -3
#define TXN_ENOSYS             -38
#define TXN_EBUSY              -16
#define TXN_E2BIG              -7
#define TXN_ESTALE             -10
#define TXN_EALREADY           -114
#define TXN_EINPROGRESS        -115
#define TXN_ETXNREPLAY         -200    // commit stalled; retry or abort
#define TXN_ENESTED            -201    // TXN_MAX_NESTING exceeded
