// kernel/txn/transaction.c — Phase 25 Stage D lifecycle.
//
// Stage D ships:
//   - txn_init  (slab + globals)
//   - txn_begin (snap_create_internal + cap_object_create + stack push)
//   - txn_abort (no replay; just snap_restore_internal + buffer drop +
//                cap teardown; Stage F adds the state-CAS race resolution)
//   - txn_commit (STUB: returns 0 on empty-buffer success; Stage F adds
//                 the actual replay loop)
//   - txn_force_drop (dedicated task_exit path — NOT a normal abort;
//                     Plan-agent Q7)
//   - txn_task_exit_cleanup (called from sched.c::task_exit)
//   - Stage E hook STUBS for txn_is_external_peer + txn_buffer_append +
//     txn_buffer_free_drop. Stage E will replace these with real impls
//     in kernel/txn/buffer.c.

#include "transaction.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../log.h"
#include "../mm/slab.h"
#include "../mm/kheap.h"
#include "../audit.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/token.h"
#include "../snap/snapshot.h"
#include "../sync/spinlock.h"

#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/interrupts.h"

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------
static kmem_cache_t      *g_txn_cache = NULL;
static volatile uint64_t  g_txn_next_id = 1;
static transaction_t     *g_txn_live_head = NULL;
static spinlock_t         g_txn_live_lock = SPINLOCK_INITIALIZER("txn_live");

// ---------------------------------------------------------------------------
// Live-list helpers.
// ---------------------------------------------------------------------------
static void txn_link_locked(transaction_t *t) {
    t->prev = NULL;
    t->next = g_txn_live_head;
    if (g_txn_live_head) g_txn_live_head->prev = t;
    g_txn_live_head = t;
}

static void txn_unlink_locked(transaction_t *t) {
    if (t->prev) t->prev->next = t->next;
    else         g_txn_live_head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->prev = t->next = NULL;
}

// ---------------------------------------------------------------------------
// Scope PID set construction. Plan-agent Q2: sorted ascending so
// txn_is_external_peer can binary-search in O(log n).
// ---------------------------------------------------------------------------
// In-place insertion sort — N <= 256 so quicksort overhead isn't worth it.
static void sort_pids_inplace(int32_t *arr, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        int32_t key = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static int populate_scope_pids(transaction_t *t, snapshot_t *s) {
    if (!t || !s) return TXN_EINVAL;
    if (s->task_count > TXN_MAX_SCOPE_TASKS) return TXN_E2BIG;
    for (uint32_t i = 0; i < s->task_count; i++) {
        t->scope_pids[i] = s->tasks[i].pid;
    }
    t->scope_pid_count = s->task_count;
    sort_pids_inplace(t->scope_pids, t->scope_pid_count);
    return 0;
}

// ---------------------------------------------------------------------------
// Buffer-size flag → bytes mapping.
// ---------------------------------------------------------------------------
static uint32_t txn_buffer_bytes_for_flags(uint32_t flags) {
    if (flags & TXN_FLAG_BUFFER_2MB) return 2u * 1024u * 1024u;
    if (flags & TXN_FLAG_BUFFER_8MB) return 8u * 1024u * 1024u;
    return TXN_DEFAULT_BUFFER_BYTES;  // default 4 MiB
}

// ---------------------------------------------------------------------------
// txn_init — boot-time setup. Called from main.c after snap_init().
// ---------------------------------------------------------------------------
void txn_init(void) {
    g_txn_cache = kmem_cache_create("transaction_t", sizeof(transaction_t),
                                    /*align=*/8, /*ctor=*/NULL,
                                    SUBSYS_CORE);
    if (!g_txn_cache) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "txn_init: kmem_cache_create('transaction_t') failed");
        return;
    }
    g_txn_next_id   = 1;
    g_txn_live_head = NULL;
    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_init: ready (TXN_MAX_NESTING=%u, default buffer=%u bytes)",
         TXN_MAX_NESTING, TXN_DEFAULT_BUFFER_BYTES);
}

// ---------------------------------------------------------------------------
// txn_begin — SYS_TXN_BEGIN entry.
// ---------------------------------------------------------------------------
int txn_begin(uint32_t flags, const char *name, task_t *caller) {
    if (!g_txn_cache) return TXN_EINVAL;
    if (!caller) return TXN_EPERM;
    if (flags & ~TXN_FLAG_VALID_MASK) return TXN_EINVAL;

    // Default to SELF scope if neither bit set.
    if (!(flags & (TXN_FLAG_SELF_SCOPE | TXN_FLAG_GLOBAL_SCOPE))) {
        flags |= TXN_FLAG_SELF_SCOPE;
    }
    // Spec: GLOBAL requires CAP_KIND_SYSTEM. Phase 25 v1 doesn't implement
    // a separate "system" cap; document the limitation and reject GLOBAL.
    // Once Phase 26 lands a system-cap check, this becomes a real gate.
    if (flags & TXN_FLAG_GLOBAL_SCOPE) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "txn_begin: GLOBAL_SCOPE requested but CAP_KIND_SYSTEM gating "
             "isn't yet wired — refusing for safety");
        return TXN_EPERM;
    }

    if (caller->active_txn.stack_depth >= TXN_MAX_NESTING) {
        return TXN_ENESTED;
    }

    transaction_t *t = (transaction_t *)kmem_cache_alloc(g_txn_cache);
    if (!t) return TXN_ENOMEM;
    memset(t, 0, sizeof(*t));
    t->state = TXN_STATE_ACTIVE;
    t->flags = flags;
    t->creator_pid = caller->id;
    t->buffer_vmo_capacity = txn_buffer_bytes_for_flags(flags);
    spinlock_init(&t->state_waitq_lock, "txn_state_waitq");
    t->id = __atomic_fetch_add(&g_txn_next_id, 1, __ATOMIC_RELAXED);

    if (name) {
        size_t n = 0;
        while (n < TXN_NAME_MAX_LEN && name[n] != '\0') {
            t->name[n] = name[n];
            n++;
        }
        t->name[n] = '\0';
    }

    // Map TXN_FLAG_* → SNAP_SCOPE_*. SELF is always set (defaulted above).
    uint32_t snap_scope = SNAP_SCOPE_SELF;
    if (flags & TXN_FLAG_GLOBAL_SCOPE) snap_scope |= SNAP_SCOPE_GLOBAL;
    // Note: we don't set SNAP_SCOPE_FREEZE_ALL_CHANS here — txn buffering
    // happens at chan_send time (Stage E), so freezing channels would be
    // double-counting and would also break in-scope sends.

    snapshot_t *snap = NULL;
    int rc = snap_create_internal(snap_scope, t->name, &snap);
    if (rc < 0 || !snap) {
        kmem_cache_free(g_txn_cache, t);
        return rc < 0 ? rc : TXN_ENOMEM;
    }
    t->backing_snapshot = snap;

    // Build the scope_pids[] sorted array.
    rc = populate_scope_pids(t, snap);
    if (rc < 0) {
        snap_destroy_internal(snap);
        kmem_cache_free(g_txn_cache, t);
        return rc;
    }

    // Allocate the cap_object. RIGHT_INSPECT (for txn_list) +
    // RIGHT_REVOKE + RIGHT_DERIVE (for diminishing-derive of
    // abort-only sub-tokens) + RIGHT_COMMIT + RIGHT_ABORT.
    int32_t audience[2] = { caller->id, PID_NONE };
    int obj_idx = cap_object_create(CAP_KIND_TRANSACTION,
                                    /*rights=*/RIGHT_INSPECT | RIGHT_REVOKE
                                              | RIGHT_DERIVE
                                              | RIGHT_COMMIT | RIGHT_ABORT,
                                    audience,
                                    /*flags=*/0,
                                    /*kind_data=*/(uintptr_t)t,
                                    caller->id,
                                    CAP_OBJECT_IDX_NONE);
    if (obj_idx < 0) {
        snap_destroy_internal(snap);
        kmem_cache_free(g_txn_cache, t);
        return TXN_ENOMEM;
    }
    t->cap_object_idx = (uint32_t)obj_idx;

    uint32_t slot = CAP_HANDLE_SLOT_NONE;
    int ins = cap_handle_insert(&caller->cap_handles, (uint32_t)obj_idx,
                                /*token_flags=*/0, &slot);
    if (ins < 0) {
        cap_object_revoke((uint32_t)obj_idx);
        cap_object_destroy((uint32_t)obj_idx);
        snap_destroy_internal(snap);
        kmem_cache_free(g_txn_cache, t);
        return ins == CAP_V2_ENOMEM ? TXN_ENOMEM : TXN_EINVAL;
    }

    // Push onto caller's transaction stack.
    t->parent_txn = caller->active_txn.current;
    t->nesting_depth = caller->active_txn.stack_depth;
    caller->active_txn.current = t;
    caller->active_txn.stack_depth++;

    spinlock_acquire(&g_txn_live_lock);
    txn_link_locked(t);
    spinlock_release(&g_txn_live_lock);

    audit_write_txn_begin(caller->id, (uint32_t)obj_idx,
                          t->id, caller->active_txn.stack_depth, t->name);

    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_begin: id=%lu pid=%d nesting=%u slot=%u name='%s'",
         (unsigned long)t->id, caller->id,
         (unsigned)caller->active_txn.stack_depth,
         (unsigned)slot, t->name);

    return (int)slot;
}

// ---------------------------------------------------------------------------
// Resolve a handle to a transaction_t pointer (caller's table).
// ---------------------------------------------------------------------------
static transaction_t *txn_resolve_handle(uint32_t handle, task_t *caller) {
    if (!caller) return NULL;
    cap_handle_entry_t *entry = cap_handle_lookup(&caller->cap_handles, handle);
    if (!entry) return NULL;
    cap_object_t *obj = cap_object_get(entry->object_idx);
    if (!obj || obj->kind != CAP_KIND_TRANSACTION) return NULL;
    return (transaction_t *)obj->kind_data;
}

// ---------------------------------------------------------------------------
// txn_pop_stack — remove `t` from caller's active_txn stack. The txn
// being popped need not be innermost (outer abort cancels all inners).
// Returns the count of stack frames removed (always >= 1 on success, 0
// if t wasn't found).
// ---------------------------------------------------------------------------
static uint32_t txn_pop_stack(task_t *caller, transaction_t *target) {
    if (!caller || !target) return 0;
    transaction_t *innermost = caller->active_txn.current;
    if (!innermost) return 0;

    // Walk stack from innermost outward to find target.
    transaction_t *cur = innermost;
    uint32_t removed = 0;
    while (cur) {
        if (cur == target) {
            // Pop everything from innermost up to and including target.
            transaction_t *next_outer = target->parent_txn;
            // Stack depth drops by (1 + number of inners).
            uint32_t cur_depth = caller->active_txn.stack_depth;
            uint32_t target_depth = target->nesting_depth;
            removed = cur_depth - target_depth;
            caller->active_txn.current = next_outer;
            caller->active_txn.stack_depth = target_depth;
            return removed;
        }
        cur = cur->parent_txn;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// State CAS helpers (Plan-agent Q6). The state field is 4 bytes (enum
// promoted to int). We use __atomic_compare_exchange so concurrent
// commit / abort callers serialise without a heavyweight mutex.
// ---------------------------------------------------------------------------
static bool txn_state_cas(transaction_t *t, txn_state_t expected,
                          txn_state_t desired) {
    if (!t) return false;
    txn_state_t exp = expected;
    return __atomic_compare_exchange_n(&t->state, &exp, desired,
                                       /*weak=*/false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

// Wake every task blocked on `t->state_waitq_head`. Called from txn_commit
// (after CAS to COMMITTED / COMMITTING_FAILED) and txn_abort (after CAS to
// ABORTING) so a blocked partner sees the new state.
static void txn_wake_state_waiters(transaction_t *t) {
    if (!t) return;
    spinlock_acquire(&t->state_waitq_lock);
    while (t->state_waitq_head) {
        sched_wake_one_on_channel(&t->state_waitq_head, /*wait_result=*/0);
    }
    spinlock_release(&t->state_waitq_lock);
}

// Block the caller on `t->state_waitq_head` until any state-change wake
// fires. Returns 0 normally; non-zero on -ETIMEDOUT if a timeout hits.
// 100 ms watchdog so a buggy commit can never hang an abort indefinitely.
static int txn_wait_state_change(transaction_t *t) {
    if (!t) return TXN_EINVAL;
    return sched_block_on_channel((void *)&t->state_waitq_lock,
                                  /*dir=*/0,
                                  /*timeout_ns=*/100ull * 1000ull * 1000ull,
                                  &t->state_waitq_head);
}

// ---------------------------------------------------------------------------
// txn_abort — Stage F full state-machine version.
//
// Resolves the txn via handle, validates ownership, then uses CAS to move
// the state from ACTIVE → ABORTING (or COMMITTING_FAILED → ABORTING when
// the user has given up retrying). If the state is COMMITTING when we
// arrive, we sleep on t->state_waitq until commit finishes (success or
// stall); on COMMITTED we return -EALREADY (commit already happened),
// on COMMITTING_FAILED we proceed with abort + emit the partial-external
// warning.
// ---------------------------------------------------------------------------
int txn_abort(uint32_t handle, task_t *caller) {
    transaction_t *t = txn_resolve_handle(handle, caller);
    if (!t) return TXN_EINVAL;
    if (t->creator_pid != caller->id) return TXN_EPERM;

    // Refuse abort on inner txns when an outer is still active. We require
    // the user to abort innermost-first.
    if (caller->active_txn.current != t) return TXN_EBUSY;

    // CAS state. Try ACTIVE → ABORTING first (common path); fall back to
    // COMMITTING_FAILED → ABORTING (after a stall the user gave up on);
    // if state is COMMITTING, sleep on the waitq.
    while (1) {
        if (txn_state_cas(t, TXN_STATE_ACTIVE, TXN_STATE_ABORTING)) break;
        if (txn_state_cas(t, TXN_STATE_COMMITTING_FAILED, TXN_STATE_ABORTING)) break;
        txn_state_t st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
        if (st == TXN_STATE_COMMITTING) {
            // Sleep, retry.
            txn_wait_state_change(t);
            continue;
        }
        if (st == TXN_STATE_COMMITTED) {
            // Too late — commit already succeeded. Tell the caller.
            return TXN_EALREADY;
        }
        if (st == TXN_STATE_ABORTED || st == TXN_STATE_ABORTING) {
            return TXN_EBUSY;
        }
        // Defensive fall-through.
        return TXN_EBUSY;
    }

    // We are now the unique owner of the abort path.

    // If a prior commit attempt delivered some messages before stalling,
    // emit the partial-external rollback record so audit consumers know
    // some packets are already on the wire.
    if (t->replay_delivered > 0) {
        txn_replay_rollback_warning(t, caller->id, t->replay_delivered,
                                    t->buffered_count - t->replay_delivered,
                                    /*force_drop=*/0);
    }

    // Drop any buffered external sends (no replay).
    txn_buffer_free_drop(t);

    // Restore the snapshot state — pages, FDs, FS pins.
    int rc = snap_restore_internal(t->backing_snapshot, caller);
    if (rc < 0) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "txn_abort: snap_restore_internal rc=%d (continuing teardown)", rc);
    }
    snap_destroy_internal(t->backing_snapshot);
    t->backing_snapshot = NULL;

    // Pop the caller's stack frame and tear down handle / cap_object.
    (void)txn_pop_stack(caller, t);
    cap_handle_remove(&caller->cap_handles, handle);
    cap_object_revoke(t->cap_object_idx);
    cap_object_destroy(t->cap_object_idx);

    spinlock_acquire(&g_txn_live_lock);
    txn_unlink_locked(t);
    spinlock_release(&g_txn_live_lock);

    audit_write_txn_abort(caller->id, t->cap_object_idx, t->id,
                          /*delivered=*/t->replay_delivered,
                          /*remaining=*/t->buffered_count);

    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_abort: id=%lu pid=%d delivered=%u dropped=%u",
         (unsigned long)t->id, caller->id,
         (unsigned)t->replay_delivered, (unsigned)t->buffered_count);

    __atomic_store_n(&t->state, TXN_STATE_ABORTED, __ATOMIC_RELEASE);
    txn_wake_state_waiters(t);
    kmem_cache_free(g_txn_cache, t);
    return 0;
}

// ---------------------------------------------------------------------------
// txn_commit — Stage F full state-machine + replay loop.
//
// CAS state ACTIVE → COMMITTING; if state is COMMITTING the caller is
// re-entering after a previous stall, so we move COMMITTING_FAILED →
// COMMITTING and resume. Other states return -EBUSY. Replay is delegated
// to txn_replay_all (kernel/txn/replay.c). On full drain we discard the
// snapshot, tear down the cap_object, and emit AUDIT_TXN_COMMIT (Plan-
// agent Q9: ordering AFTER successful replay). On stall we set
// COMMITTING_FAILED + return TXN_ETXNREPLAY so the caller can retry.
// ---------------------------------------------------------------------------
int txn_commit(uint32_t handle, task_t *caller) {
    transaction_t *t = txn_resolve_handle(handle, caller);
    if (!t) return TXN_EINVAL;
    if (t->creator_pid != caller->id) return TXN_EPERM;

    // Outer-still-active: refuse commit on inner txns.
    if (caller->active_txn.current != t) return TXN_EBUSY;

    // CAS the state. ACTIVE → COMMITTING (first attempt) or
    // COMMITTING_FAILED → COMMITTING (retry).
    bool first = txn_state_cas(t, TXN_STATE_ACTIVE, TXN_STATE_COMMITTING);
    bool retry = !first && txn_state_cas(t, TXN_STATE_COMMITTING_FAILED,
                                         TXN_STATE_COMMITTING);
    if (!first && !retry) {
        return TXN_EBUSY;
    }

    // Replay path — bypass entirely if buffer is empty.
    int replay_rc = 0;
    if (t->buffered_count > 0) {
        txn_replay_context_t ctx;
        if (first) {
            txn_buffer_iter_init(t, &ctx);
            ctx.delivered_count = 0;
        } else {
            // Retry: persist the running counters from the previous attempt.
            ctx.txn             = t;
            ctx.current_offset  = t->replay_cursor;
            ctx.delivered_count = t->replay_delivered;
            ctx.failed_chan_id  = 0;
        }
        replay_rc = txn_replay_all(t, &ctx, caller);
        // Persist progress for retry / partial-rollback bookkeeping.
        t->replay_cursor      = ctx.current_offset;
        t->replay_delivered   = ctx.delivered_count;
        t->replay_failed_chan_id = ctx.failed_chan_id;
        if (replay_rc < 0) {
            // Stall: stash state in COMMITTING_FAILED, wake any waiter,
            // surface error to the caller.
            __atomic_store_n(&t->state, TXN_STATE_COMMITTING_FAILED,
                             __ATOMIC_RELEASE);
            txn_wake_state_waiters(t);
            klog(KLOG_WARN, SUBSYS_CORE,
                 "txn_commit: stall at offset=%u delivered=%u failed_chan=%lu",
                 (unsigned)t->replay_cursor,
                 (unsigned)t->replay_delivered,
                 (unsigned long)t->replay_failed_chan_id);
            return replay_rc;
        }
    }

    // Replay drained (or buffer was empty). Audit BEFORE teardown so the
    // commit record carries the canonical pre-teardown state.
    audit_write_txn_commit(caller->id, t->cap_object_idx, t->id,
                           t->replay_delivered);

    // Discard the snapshot (commit semantics: keep current FS / page state).
    snap_destroy_internal(t->backing_snapshot);
    t->backing_snapshot = NULL;

    // Pop the caller's stack and clean up.
    (void)txn_pop_stack(caller, t);
    cap_handle_remove(&caller->cap_handles, handle);
    cap_object_revoke(t->cap_object_idx);
    cap_object_destroy(t->cap_object_idx);

    spinlock_acquire(&g_txn_live_lock);
    txn_unlink_locked(t);
    spinlock_release(&g_txn_live_lock);

    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_commit: id=%lu pid=%d delivered=%u",
         (unsigned long)t->id, caller->id, (unsigned)t->replay_delivered);

    __atomic_store_n(&t->state, TXN_STATE_COMMITTED, __ATOMIC_RELEASE);
    txn_wake_state_waiters(t);
    kmem_cache_free(g_txn_cache, t);
    return 0;
}

// ---------------------------------------------------------------------------
// txn_force_drop — task_exit teardown path. Plan-agent Q7: NOT a normal
// abort because the dying task's address space is mid-teardown. We:
//   - drop the buffer (no replay)
//   - destroy the snapshot WITHOUT calling snap_restore
//   - revoke + destroy the cap_object
//   - emit AUDIT_TXN_PARTIAL_EXTERNAL with force_drop=1 + delivered_count=0
//   - free the txn record
// Caller must NOT use t after this returns. Does NOT touch the caller's
// active_txn stack — txn_task_exit_cleanup walks the stack and calls
// this per entry.
// ---------------------------------------------------------------------------
static void txn_force_drop(transaction_t *t, int32_t pid) {
    if (!t) return;

    txn_buffer_free_drop(t);

    if (t->backing_snapshot) {
        snap_destroy_internal(t->backing_snapshot);
        t->backing_snapshot = NULL;
    }

    if (t->cap_object_idx != 0) {
        cap_object_revoke(t->cap_object_idx);
        cap_object_destroy(t->cap_object_idx);
    }

    spinlock_acquire(&g_txn_live_lock);
    txn_unlink_locked(t);
    spinlock_release(&g_txn_live_lock);

    audit_write_txn_partial_external(pid, t->cap_object_idx, t->id,
                                     /*delivered=*/0,
                                     /*remaining=*/t->buffered_count,
                                     /*force_drop=*/1);

    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_force_drop: id=%lu pid=%d remaining=%u",
         (unsigned long)t->id, (int)pid, (unsigned)t->buffered_count);

    t->state = TXN_STATE_ABORTED;
    kmem_cache_free(g_txn_cache, t);
}

// ---------------------------------------------------------------------------
// txn_task_exit_cleanup — called from sched.c::task_exit. Walks the dying
// task's active_txn stack and calls txn_force_drop on each. Walks
// innermost-first since each force_drop is independent. Clears the
// task's active_txn frame.
// ---------------------------------------------------------------------------
void txn_task_exit_cleanup(task_t *dying) {
    if (!dying) return;
    transaction_t *cur = dying->active_txn.current;
    while (cur) {
        transaction_t *next = cur->parent_txn;
        txn_force_drop(cur, dying->id);
        cur = next;
    }
    dying->active_txn.current = NULL;
    dying->active_txn.stack_depth = 0;
}

// ---------------------------------------------------------------------------
// txn_dump — diagnostic.
// ---------------------------------------------------------------------------
void txn_dump(transaction_t *t) {
    if (!t) return;
    klog(KLOG_INFO, SUBSYS_CORE,
         "txn_dump: id=%lu state=%d nesting=%u buffer_count=%u name='%s'",
         (unsigned long)t->id, (int)t->state,
         (unsigned)t->nesting_depth, (unsigned)t->buffered_count, t->name);
}

// ---------------------------------------------------------------------------
// txn_list — Phase 25 introspection. Currently returns 0 (no records);
// Stage E or later may surface live txns. The signature uses the same
// snap_info_user_t struct as snap_list to share the userspace ABI.
// ---------------------------------------------------------------------------
int txn_list(struct snap_info_user *out_buf, uint32_t max) {
    (void)out_buf;
    (void)max;
    // No introspection in Stage D. Stage F may extend this for txn status.
    return 0;
}

// ===========================================================================
// Stage E hooks (txn_is_external_peer / txn_buffer_append /
// txn_buffer_free_drop) live in kernel/txn/buffer.c. They were stubbed
// here in Stage D and have moved out as of Stage E.
// ===========================================================================
