// kernel/txn/replay.c — Phase 25 Stage F: commit-time replay engine.
//
// txn_commit hands off to txn_replay_all once it has CAS'd the state to
// COMMITTING. Replay walks the buffer's records and re-runs chan_send for
// each one. Two correctness rules:
//
//   1. **caller->replay_in_progress** is set across the per-record send so
//      chan_send's prologue does NOT re-buffer the message into the same
//      txn (Plan-agent Q3). Cleared on every iter exit (success, stall,
//      kill). The flag is per-task, not per-txn, because the caller is
//      what chan_send sees.
//
//   2. **Audit ordering** (Plan-agent Q9): the AUDIT_TXN_COMMIT record is
//      emitted by txn_commit AFTER replay_all returns 0. If we emitted
//      before, a crash mid-replay would leave a false success record.
//      AUDIT_TXN_PARTIAL_EXTERNAL fires from txn_replay_rollback_warning
//      whenever the replay loop exits with delivered_count > 0 but
//      remaining > 0 — i.e., some packets were already on the wire.
//
// Stall path: chan_lookup_by_id returns NULL (channel destroyed) or
// chan_send returns < 0. We save ctx->failed_chan_id + ctx->current_offset
// so a subsequent SYS_TXN_COMMIT call can resume from where we stopped.
// If the user gives up and aborts, the rollback warning fires with the
// (delivered, remaining) split.

#include "transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../audit.h"
#include "../ipc/channel.h"
#include "../log.h"

#include "../../arch/x86_64/cpu/sched/sched.h"

// ---------------------------------------------------------------------------
// txn_replay_all — drain every buffered record into its target channel.
//
// Returns 0 on full drain; TXN_ETXNREPLAY (-200) on stall (caller may
// retry or abort); TXN_EINVAL on internal magic mismatch (corrupted
// buffer — shouldn't happen).
//
// The iter is initialised by txn_replay_all on first entry; on retry,
// callers pass the same ctx without re-init so current_offset / delivered
// persist across calls. txn_replay_resume is a thin alias used by Stage F
// retry tests for clarity.
// ---------------------------------------------------------------------------
int txn_replay_all(transaction_t *t, txn_replay_context_t *ctx, task_t *caller) {
    if (!t || !ctx || !caller) return TXN_EINVAL;
    if (!t->buffer_vmo || t->buffered_count == 0) {
        // Nothing to replay; trivial success.
        ctx->delivered_count = 0;
        return 0;
    }

    // Set the per-task bypass so chan_send's prologue does NOT re-buffer
    // these sends into the active_txn. The flag is RELEASE-stored so
    // observers (other CPUs scheduling chan_send concurrently) see it.
    __atomic_store_n(&caller->replay_in_progress, 1u, __ATOMIC_RELEASE);

    int last_rc = 0;
    while (ctx->current_offset < t->buffer_vmo_head) {
        buffered_msg_header_t hdr;
        channel_msg_t payload;
        uint32_t next_offset = ctx->current_offset;

        ctx->failed_chan_id = 0;
        int rc = txn_buffer_iter_next(ctx, &hdr, &payload, &next_offset);
        if (rc < 0) {
            // Buffer corruption (magic mismatch) or end-of-data sentinel.
            // Treat ENODATA (-61) as full drain; anything else as stall.
            if (rc == -61) { last_rc = 0; break; }
            last_rc = rc;
            break;
        }

        // Resolve the target channel. If it was destroyed mid-txn, mark
        // failure and exit (the caller can either retry once the channel
        // is recreated or abort).
        channel_t *c = chan_lookup_by_id(hdr.target_chan_id);
        if (!c) {
            ctx->failed_chan_id = hdr.target_chan_id;
            last_rc = TXN_ETXNREPLAY;
            klog(KLOG_WARN, SUBSYS_CORE,
                 "txn_replay_all: target chan_id=%lu not found at offset=%u",
                 (unsigned long)hdr.target_chan_id,
                 (unsigned)ctx->current_offset);
            break;
        }

        // Re-run chan_send. With replay_in_progress=1 the prologue falls
        // through to the live ring directly. timeout_ns=0 to avoid blocking
        // the caller indefinitely if the receiver is gone.
        int srv = chan_send(c, caller, &payload, /*timeout_ns=*/0);
        if (srv < 0) {
            // Receiver gone, full ring on a non-blocking peer, or any other
            // chan_send failure. Save failed chan + cursor for retry.
            ctx->failed_chan_id = hdr.target_chan_id;
            last_rc = TXN_ETXNREPLAY;
            klog(KLOG_WARN, SUBSYS_CORE,
                 "txn_replay_all: chan_send rc=%d on chan_id=%lu offset=%u",
                 srv, (unsigned long)hdr.target_chan_id,
                 (unsigned)ctx->current_offset);
            break;
        }

        ctx->current_offset = next_offset;
        ctx->delivered_count++;
    }

    __atomic_store_n(&caller->replay_in_progress, 0u, __ATOMIC_RELEASE);
    return last_rc;
}

int txn_replay_resume(transaction_t *t, txn_replay_context_t *ctx, task_t *caller) {
    // Same routine, just doesn't re-init ctx — caller persists offset.
    return txn_replay_all(t, ctx, caller);
}

// ---------------------------------------------------------------------------
// txn_replay_rollback_warning — emit AUDIT_TXN_PARTIAL_EXTERNAL.
// Fired by txn_abort when a commit attempt left messages already on the
// wire (delivered > 0) but the abort discarded the rest. force_drop = 0
// for user-initiated aborts, 1 for task_exit teardown.
// ---------------------------------------------------------------------------
void txn_replay_rollback_warning(transaction_t *t, int32_t caller_pid,
                                 uint32_t delivered, uint32_t remaining,
                                 uint8_t force_drop) {
    if (!t) return;
    audit_write_txn_partial_external(caller_pid, t->cap_object_idx, t->id,
                                     delivered, remaining, force_drop);
    klog(KLOG_WARN, SUBSYS_CORE,
         "txn_partial_external: id=%lu pid=%d delivered=%u remaining=%u force_drop=%u",
         (unsigned long)t->id, (int)caller_pid,
         (unsigned)delivered, (unsigned)remaining, (unsigned)force_drop);
}
