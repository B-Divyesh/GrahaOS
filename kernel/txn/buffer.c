// kernel/txn/buffer.c — Phase 25 Stage E: per-txn external-message buffer.
//
// While a transaction is ACTIVE, every chan_send to a peer outside the
// transaction's scope is intercepted (kernel/ipc/channel.c::chan_send
// prologue) and routed here. The buffered messages are replayed by
// txn_commit (Stage F) or dropped by txn_abort / txn_force_drop.
//
// The buffer is a kernel-only VMO allocated lazily on the first external
// send (Plan-agent Q5: most txns have no external sends; deferring the
// VMO creation until needed avoids paying the 4-MiB allocation cost
// universally). Layout per record:
//
//   [HEAD MAGIC u32 0xBEADF00D] [target_chan_id u64] [payload_len u32]
//   [flags u32] [original_send_seq u64] [payload bytes ROUND_UP_8]
//   [TAIL MAGIC u32 0xDEADC0DE]
//
// The head + tail magic catch off-by-one length bugs at iter time. They
// are NOT a security feature — the buffer is in kernel-owned memory and
// userspace cannot touch it.
//
// Stage E provides:
//   - txn_buffer_lazy_alloc            (first-touch VMO creation)
//   - txn_buffer_append                (intercept hook from chan_send)
//   - txn_buffer_iter_init/_iter_next  (replay-ready cursor; Stage F uses)
//   - txn_buffer_free_drop             (commit/abort teardown)
//   - txn_is_external_peer             (Q2 scope oracle for chan_send)
//
// Stage F replaces the txn_buffer_iter_next return-on-end sentinel with a
// real replay engine in kernel/txn/replay.c.

#include "transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../log.h"
#include "../audit.h"
#include "../cap/object.h"
#include "../cap/token.h"
#include "../ipc/channel.h"
#include "../mm/vmo.h"
#include "../sync/spinlock.h"

#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/mm/vmm.h"   // g_hhdm_offset

// ---------------------------------------------------------------------------
// Page-walk helpers. The VMO holds 1024 separately-allocated 4 KiB pages
// (2 MiB → 512 pages, 4 MiB → 1024 pages, 8 MiB → 2048 pages). Each page
// is reachable via the higher-half direct map at phys + g_hhdm_offset.
// We never need more than two adjacent pages for any single record (max
// record size = 32 + 320 + 4 = 356 bytes ≪ 4096), so the per-page split
// only matters when an entry crosses a page boundary.
// ---------------------------------------------------------------------------

static inline uint8_t *txn_buffer_page_kv(vmo_t *v, uint32_t page_idx) {
    if (!v || page_idx >= v->npages || v->pages[page_idx] == 0) {
        return NULL;
    }
    return (uint8_t *)(v->pages[page_idx] + g_hhdm_offset);
}

// Write `n` bytes from `src` into `v` starting at byte offset `off`. Spans
// page boundaries internally. Caller is expected to have already validated
// that off + n ≤ v->size_bytes (callers do via the txn_buffer_head capacity
// check). Silent no-op on NULL inputs (defensive).
static void txn_buffer_write_bytes(vmo_t *v, uint32_t off,
                                   const void *src, uint32_t n) {
    if (!v || !src) return;
    const uint8_t *p = (const uint8_t *)src;
    while (n > 0) {
        uint32_t page_idx = off / 4096u;
        uint32_t page_off = off % 4096u;
        uint32_t avail    = 4096u - page_off;
        uint32_t take     = (n < avail) ? n : avail;
        uint8_t *dst      = txn_buffer_page_kv(v, page_idx);
        if (!dst) return;  // page not backed; defensive
        memcpy(dst + page_off, p, take);
        p   += take;
        off += take;
        n   -= take;
    }
}

static void txn_buffer_read_bytes(vmo_t *v, uint32_t off,
                                  void *dst, uint32_t n) {
    if (!v || !dst) return;
    uint8_t *p = (uint8_t *)dst;
    while (n > 0) {
        uint32_t page_idx = off / 4096u;
        uint32_t page_off = off % 4096u;
        uint32_t avail    = 4096u - page_off;
        uint32_t take     = (n < avail) ? n : avail;
        const uint8_t *src = txn_buffer_page_kv(v, page_idx);
        if (!src) return;
        memcpy(p, src + page_off, take);
        p   += take;
        off += take;
        n   -= take;
    }
}

// ---------------------------------------------------------------------------
// Lazy buffer allocation. Returns 0 on success; sets t->buffer_vmo. Idempotent
// on repeated entry (returns 0 if buffer is already allocated).
// ---------------------------------------------------------------------------
static int txn_buffer_lazy_alloc(transaction_t *t) {
    if (!t) return TXN_EINVAL;
    if (t->buffer_vmo) return 0;

    uint32_t cap = t->buffer_vmo_capacity;
    if (cap == 0) cap = TXN_DEFAULT_BUFFER_BYTES;
    // Round up to PAGE_SIZE (vmo_create requires that).
    if (cap & 0xFFFu) cap = (cap + 0xFFFu) & ~0xFFFu;

    // Owner = -1 (PID_NONE), audience = -1 — kernel-only, no userspace map.
    // VMO_ZEROED so iterator never reads uninitialised bytes for a partial
    // record. VMO_PINNED because the buffer must stay resident across the
    // possibly-blocking chan_send replays in Stage F.
    vmo_t *v = vmo_create((uint64_t)cap, VMO_PINNED | VMO_ZEROED, -1, -1);
    if (!v) return TXN_ENOMEM;

    t->buffer_vmo          = v;
    t->buffer_vmo_capacity = cap;
    t->buffer_vmo_head     = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// txn_buffer_append — invoked by chan_send when peer is external for `t`.
//
// We always copy the FULL channel_msg_t (320 bytes — 32 hdr + 256 payload +
// 32 in_flight_idx[8]). Stage F's replay re-runs chan_send with the original
// inline_payload + in_flight_idx; this matches the spec note that handles
// transferred WHILE in a txn are buffered alongside the message and only
// transfer at commit.
//
// For Phase 25 v1, we DO NOT bump per-channel internal refcounts to keep
// the channel alive across cap_revoke (Plan-agent Q9). Reasoning: a buffered
// message references the channel by ID (target_chan_id), and Stage F's
// replay re-resolves via chan_lookup_by_id. If the channel was destroyed,
// replay reports failed_chan_id and emits AUDIT_TXN_PARTIAL_EXTERNAL — the
// caller learns that some replays were lost. Q9's refcount-bump is an
// optimisation; not needed for correctness.
// ---------------------------------------------------------------------------
int txn_buffer_append(transaction_t *t, uint64_t target_chan_id,
                      const struct channel_msg *msg, uint32_t flags) {
    if (!t || !msg) return TXN_EINVAL;

    int rc = txn_buffer_lazy_alloc(t);
    if (rc < 0) return rc;

    // Each entry: header (32 B) + payload (320 B) + tail magic (4 B) = 356 B.
    // Round payload to 8 for tail alignment.
    const uint32_t payload_len     = (uint32_t)sizeof(channel_msg_t);
    const uint32_t payload_aligned = (payload_len + 7u) & ~7u;
    const uint32_t entry_size      = (uint32_t)sizeof(buffered_msg_header_t)
                                   + payload_aligned
                                   + (uint32_t)sizeof(uint32_t);

    // Capacity check (Plan-agent Q5): strict-greater so we never overrun.
    if ((uint64_t)t->buffer_vmo_head + entry_size > t->buffer_vmo_capacity) {
        return -28;  // -ENOSPC
    }

    uint32_t off = t->buffer_vmo_head;

    // Header.
    buffered_msg_header_t hdr = {
        .magic             = TXN_BUFFER_MAGIC_HEAD,
        ._pad0             = 0,
        .target_chan_id    = target_chan_id,
        .payload_len       = payload_len,
        .flags             = flags,
        .original_send_seq = (uint64_t)t->buffered_count,
    };
    txn_buffer_write_bytes(t->buffer_vmo, off, &hdr, (uint32_t)sizeof(hdr));
    off += (uint32_t)sizeof(hdr);

    // Payload (full channel_msg_t copy).
    txn_buffer_write_bytes(t->buffer_vmo, off, msg, payload_len);
    off += payload_aligned;  // pad bytes already zero from VMO_ZEROED

    // Tail magic.
    uint32_t tail_magic = TXN_BUFFER_MAGIC_TAIL;
    txn_buffer_write_bytes(t->buffer_vmo, off, &tail_magic,
                           (uint32_t)sizeof(tail_magic));
    off += (uint32_t)sizeof(tail_magic);

    t->buffer_vmo_head = off;
    t->buffered_count++;

    return 0;
}

// ---------------------------------------------------------------------------
// Iterator (Stage F replay). Caller passes a stack-allocated context;
// _init resets state, _next walks the buffer one record per call. Returns
// 0 + fills out_hdr + out_payload + out_next_offset on success;
// -ENODATA at end-of-buffer; TXN_EINVAL on magic mismatch.
// ---------------------------------------------------------------------------
void txn_buffer_iter_init(transaction_t *t, txn_replay_context_t *ctx) {
    if (!ctx) return;
    ctx->txn             = t;
    ctx->current_offset  = 0;
    ctx->delivered_count = 0;
    ctx->failed_chan_id  = 0;
}

int txn_buffer_iter_next(txn_replay_context_t *ctx,
                         buffered_msg_header_t *out_hdr,
                         struct channel_msg *out_payload,
                         uint32_t *out_next_offset) {
    if (!ctx || !ctx->txn || !out_hdr || !out_payload || !out_next_offset) {
        return TXN_EINVAL;
    }
    transaction_t *t = ctx->txn;
    if (!t->buffer_vmo) return -61;             // -ENODATA
    if (ctx->current_offset >= t->buffer_vmo_head) return -61;

    uint32_t off = ctx->current_offset;

    txn_buffer_read_bytes(t->buffer_vmo, off, out_hdr, (uint32_t)sizeof(*out_hdr));
    if (out_hdr->magic != TXN_BUFFER_MAGIC_HEAD) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "txn_buffer_iter_next: head magic mismatch at off=%u txn=%lu",
             (unsigned)off, (unsigned long)t->id);
        return TXN_EINVAL;
    }
    off += (uint32_t)sizeof(*out_hdr);

    if (out_hdr->payload_len > sizeof(struct channel_msg)) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "txn_buffer_iter_next: payload_len=%u oversized at off=%u",
             (unsigned)out_hdr->payload_len, (unsigned)off);
        return TXN_EINVAL;
    }
    txn_buffer_read_bytes(t->buffer_vmo, off, out_payload, out_hdr->payload_len);
    uint32_t payload_aligned = (out_hdr->payload_len + 7u) & ~7u;
    off += payload_aligned;

    uint32_t tail_magic = 0;
    txn_buffer_read_bytes(t->buffer_vmo, off, &tail_magic,
                          (uint32_t)sizeof(tail_magic));
    if (tail_magic != TXN_BUFFER_MAGIC_TAIL) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "txn_buffer_iter_next: tail magic mismatch at off=%u txn=%lu",
             (unsigned)off, (unsigned long)t->id);
        return TXN_EINVAL;
    }
    off += (uint32_t)sizeof(tail_magic);

    *out_next_offset = off;
    return 0;
}

// ---------------------------------------------------------------------------
// txn_buffer_free_drop — release the buffer VMO and clear counters.
// Idempotent (safe to call when buffer was never allocated). Called from
// txn_commit (post-replay), txn_abort, txn_force_drop.
// ---------------------------------------------------------------------------
void txn_buffer_free_drop(transaction_t *t) {
    if (!t) return;
    if (t->buffer_vmo) {
        vmo_unref(t->buffer_vmo);
        t->buffer_vmo = NULL;
    }
    t->buffer_vmo_head = 0;
    t->buffered_count  = 0;
}

// ---------------------------------------------------------------------------
// txn_is_external_peer — Plan-agent Q2 scope oracle.
//
// A chan_send peer is "external" (and therefore should be buffered) iff the
// other endpoint of the channel is currently held by a task whose PID is
// NOT in t->scope_pids[]. We use the chan_endpoint_t.current_holder_pid
// field maintained by chan_create + chan_marshal_recv. If the peer endpoint
// has been destroyed (orphan), we conservatively treat the send as external
// — the message goes into the buffer and Stage F's replay will fail to
// resolve the channel, producing an audit record but no kernel mishap.
//
// Returns true when the peer is external; false when in-scope (deliver
// directly via the live channel).
// ---------------------------------------------------------------------------
bool txn_is_external_peer(struct channel *c, transaction_t *t,
                          struct task_struct *sender) {
    if (!c || !t || !sender) return false;

    // Look up both endpoints' current holders. The sender holds one (read or
    // write); the other one is the peer we care about.
    cap_object_t *rd_obj = cap_object_get(c->read_cap_idx);
    cap_object_t *wr_obj = cap_object_get(c->write_cap_idx);

    int32_t rd_holder = -1;
    int32_t wr_holder = -1;
    if (rd_obj && rd_obj->kind == CAP_KIND_CHANNEL && rd_obj->kind_data) {
        chan_endpoint_t *ep = (chan_endpoint_t *)rd_obj->kind_data;
        rd_holder = __atomic_load_n(&ep->current_holder_pid, __ATOMIC_ACQUIRE);
    }
    if (wr_obj && wr_obj->kind == CAP_KIND_CHANNEL && wr_obj->kind_data) {
        chan_endpoint_t *ep = (chan_endpoint_t *)wr_obj->kind_data;
        wr_holder = __atomic_load_n(&ep->current_holder_pid, __ATOMIC_ACQUIRE);
    }

    int32_t peer_pid;
    if (rd_holder == sender->id) {
        peer_pid = wr_holder;
    } else if (wr_holder == sender->id) {
        peer_pid = rd_holder;
    } else {
        // Sender holds neither endpoint (e.g., kernel-internal sender via
        // chan_create_kernel paths). Treat as external for safety; Stage F
        // can refine if a regression appears.
        return true;
    }

    if (peer_pid < 0) {
        // Orphaned peer endpoint — destroyed or never-assigned. Treat as
        // external; replay will surface failed_chan_id.
        return true;
    }

    // Self-channel (sender on both ends): in-scope by definition.
    if (peer_pid == sender->id) return false;

    // Binary search the sorted scope_pids[] (built at txn_begin from the
    // backing snapshot's task set). If peer_pid is in the array, in-scope.
    if (t->scope_pid_count == 0) return true;  // no scope: everything external

    int32_t lo = 0;
    int32_t hi = (int32_t)t->scope_pid_count - 1;
    while (lo <= hi) {
        int32_t mid = lo + ((hi - lo) >> 1);
        int32_t v   = t->scope_pids[mid];
        if (v == peer_pid) return false;        // in-scope
        if (v < peer_pid)  lo = mid + 1;
        else               hi = mid - 1;
    }
    return true;  // peer not in scope → external
}
