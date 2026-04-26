// kernel/net/rawnet.c — Phase 22.
//
// Named-channel registry. See rawnet.h for protocol-level docs.

#include "rawnet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../ipc/channel.h"
#include "../ipc/manifest.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/can.h"
#include "../audit.h"
#include "../log.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// ---------------------------------------------------------------------------
// Registry state.
// ---------------------------------------------------------------------------

static rawnet_publish_entry_t g_rawnet_entries[RAWNET_SLOTS_MAX];
static spinlock_t g_rawnet_lock = SPINLOCK_INITIALIZER("rawnet");
static bool g_rawnet_ready = false;

// ---------------------------------------------------------------------------
// rawnet_name_validate: public so test code can probe the parser.
// ---------------------------------------------------------------------------
bool rawnet_name_validate(const char *name, uint32_t name_len) {
    if (!name || name_len == 0 || name_len > RAWNET_NAME_MAX) return false;
    if (name[0] != '/') return false;
    for (uint32_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0 || c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Linear scan for a name. Caller holds g_rawnet_lock.
// ---------------------------------------------------------------------------
static rawnet_publish_entry_t *find_entry_locked(const char *name,
                                                 uint32_t name_len) {
    for (uint32_t i = 0; i < RAWNET_SLOTS_MAX; i++) {
        rawnet_publish_entry_t *e = &g_rawnet_entries[i];
        if (e->name_len == 0) continue;
        if (e->name_len != (uint8_t)name_len) continue;
        if (memcmp(e->name, name, name_len) == 0) return e;
    }
    return NULL;
}

static rawnet_publish_entry_t *find_free_slot_locked(void) {
    for (uint32_t i = 0; i < RAWNET_SLOTS_MAX; i++) {
        rawnet_publish_entry_t *e = &g_rawnet_entries[i];
        if (e->name_len == 0) return e;
    }
    return NULL;
}

const rawnet_publish_entry_t *rawnet_lookup_locked(const char *name,
                                                   uint32_t name_len) {
    return find_entry_locked(name, name_len);
}

// ---------------------------------------------------------------------------
// clear_entry_locked: zero an entry's runtime fields without touching
// `name_len` (caller sets that to 0 to mark the slot free).
// ---------------------------------------------------------------------------
static void clear_entry_runtime(rawnet_publish_entry_t *e) {
    e->publisher_pid      = 0;
    e->connection_seq     = 0;
    e->payload_type_hash  = 0;
    e->accept_channel     = NULL;
    e->accept_chan_obj_idx = 0;
}

// ---------------------------------------------------------------------------
// rawnet_init: zero the table, mark ready.
// ---------------------------------------------------------------------------
void rawnet_init(void) {
    spinlock_acquire(&g_rawnet_lock);
    for (uint32_t i = 0; i < RAWNET_SLOTS_MAX; i++) {
        g_rawnet_entries[i].name[0] = 0;
        g_rawnet_entries[i].name_len = 0;
        clear_entry_runtime(&g_rawnet_entries[i]);
    }
    g_rawnet_ready = true;
    spinlock_release(&g_rawnet_lock);
    klog(KLOG_INFO, SUBSYS_NET,
         "[rawnet] init ok (slots=%u, name_max=%u)",
         (unsigned)RAWNET_SLOTS_MAX, (unsigned)RAWNET_NAME_MAX);
}

// ---------------------------------------------------------------------------
// Helpers for mapping cap_token -> (channel_t*, cap_object_t*, obj_idx).
// Phase 17's cap_token layout: bits [ 7:0] = flags, [31:8] = obj idx,
// [63:32] = generation.
// ---------------------------------------------------------------------------
static inline uint32_t tok_obj_idx(cap_token_t t) {
    return (uint32_t)((t.raw >> 8) & 0x00FFFFFFu);
}

// ---------------------------------------------------------------------------
// rawnet_publish.
// ---------------------------------------------------------------------------
int rawnet_publish(int32_t publisher_pid,
                   const char *name, uint32_t name_len,
                   uint64_t payload_type_hash,
                   cap_token_t accept_write_tok) {
    if (!g_rawnet_ready) return CAP_V2_EINVAL;
    if (!rawnet_name_validate(name, name_len)) return CAP_V2_EINVAL;
    if (!manifest_type_known(payload_type_hash)) return CAP_V2_EPROTOTYPE;
    if (publisher_pid <= 0) return CAP_V2_EPERM;

    // Resolve the accept-channel write endpoint. Must be owned by the
    // publisher, must be a CHANNEL WRITE endpoint.
    channel_t *accept_chan = NULL;
    uint32_t   accept_obj_idx = 0;
    int rc = chan_resolve_endpoint(publisher_pid, accept_write_tok,
                                   CHAN_ENDPOINT_WRITE, 0,
                                   &accept_chan, &accept_obj_idx);
    if (rc != 0) return rc;
    if (!accept_chan) return CAP_V2_EINVAL;

    spinlock_acquire(&g_rawnet_lock);

    rawnet_publish_entry_t *existing = find_entry_locked(name, name_len);
    rawnet_publish_entry_t *slot = NULL;

    if (existing) {
        if (existing->publisher_pid != publisher_pid) {
            spinlock_release(&g_rawnet_lock);
            return CAP_V2_EPERM;
        }
        // Same PID republishing (daemon respawn after crash). Drop old refcount
        // on the previous accept channel, replace.
        if (existing->accept_channel) {
            __atomic_fetch_sub(&existing->accept_channel->refcount, 1,
                               __ATOMIC_RELEASE);
        }
        slot = existing;
    } else {
        slot = find_free_slot_locked();
        if (!slot) {
            spinlock_release(&g_rawnet_lock);
            return CAP_V2_ENOMEM;
        }
        memcpy(slot->name, name, name_len);
        slot->name[name_len] = '\0';
        slot->name_len = (uint8_t)name_len;
    }

    __atomic_fetch_add(&accept_chan->refcount, 1, __ATOMIC_RELEASE);

    slot->publisher_pid      = publisher_pid;
    slot->connection_seq     = slot->connection_seq;  // preserve on replace
    slot->payload_type_hash  = payload_type_hash;
    slot->accept_channel     = accept_chan;
    slot->accept_chan_obj_idx = accept_obj_idx;

    spinlock_release(&g_rawnet_lock);

    audit_write_chan_name_publish(publisher_pid, slot->name,
                                  payload_type_hash);
    klog(KLOG_INFO, SUBSYS_NET,
         "[rawnet] publish name=%s pid=%d type=0x%lx (slot %lu)",
         slot->name, (int)publisher_pid,
         (unsigned long)payload_type_hash,
         (unsigned long)(slot - g_rawnet_entries));
    return 0;
}

// ---------------------------------------------------------------------------
// rawnet_connect.
//
// Allocates a pair of per-connection channels typed with the publisher's
// declared payload hash. All four endpoints are initially owned by the
// connector; the kernel then moves the server-side pair (read-end of A,
// write-end of B) out of the connector's table and into a channel message
// on the accept channel, which the publisher recv's to obtain them.
// The client-side pair (write-end of A, read-end of B) is returned via
// `out_wr_req_tok` + `out_rd_resp_tok`.
// ---------------------------------------------------------------------------
int rawnet_connect(int32_t connector_pid,
                   const char *name, uint32_t name_len,
                   cap_token_t *out_wr_req_tok,
                   cap_token_t *out_rd_resp_tok) {
    if (!g_rawnet_ready) return CAP_V2_EINVAL;
    if (!out_wr_req_tok || !out_rd_resp_tok) return CAP_V2_EFAULT;
    if (!rawnet_name_validate(name, name_len)) return CAP_V2_EINVAL;
    if (connector_pid <= 0) return CAP_V2_EPERM;

    // Snapshot the entry under the lock; channel pointer is stable because
    // we bump its refcount before releasing.
    spinlock_acquire(&g_rawnet_lock);
    rawnet_publish_entry_t *e = find_entry_locked(name, name_len);
    if (!e) {
        spinlock_release(&g_rawnet_lock);
        return CAP_V2_EBADF;
    }
    channel_t *accept_chan = e->accept_channel;
    uint64_t   payload_hash = e->payload_type_hash;
    int32_t    publisher_pid = e->publisher_pid;
    uint32_t   conn_id = ++e->connection_seq;
    // Bump accept-channel refcount so it cannot evaporate between here and
    // the chan_send below. We drop the ref at the end of this function.
    if (accept_chan) {
        __atomic_fetch_add(&accept_chan->refcount, 1, __ATOMIC_RELEASE);
    }
    spinlock_release(&g_rawnet_lock);

    if (!accept_chan) return CAP_V2_EPIPE;

    task_t *connector = sched_get_task(connector_pid);
    if (!connector) {
        __atomic_fetch_sub(&accept_chan->refcount, 1, __ATOMIC_RELEASE);
        return CAP_V2_EPERM;
    }

    // Allocate the two per-connection channels. chan_create inserts all
    // endpoint tokens into the connector's handle table.
    cap_token_t rd_a = {0}, wr_a = {0};
    cap_token_t rd_b = {0}, wr_b = {0};
    int rc = chan_create(payload_hash, CHAN_MODE_BLOCKING,
                         RAWNET_CONN_CAPACITY, connector_pid, &rd_a, &wr_a);
    if (rc != 0) {
        __atomic_fetch_sub(&accept_chan->refcount, 1, __ATOMIC_RELEASE);
        return rc;
    }
    rc = chan_create(payload_hash, CHAN_MODE_BLOCKING,
                     RAWNET_CONN_CAPACITY, connector_pid, &rd_b, &wr_b);
    if (rc != 0) {
        // Destroy channel A.
        cap_object_destroy(tok_obj_idx(rd_a));
        cap_object_destroy(tok_obj_idx(wr_a));
        __atomic_fetch_sub(&accept_chan->refcount, 1, __ATOMIC_RELEASE);
        return rc;
    }

    // Build the staged channel message that we'll drop on the accept
    // channel. in_flight_idx carries the two cap_object indices the
    // publisher will receive; chan_marshal_recv on the publisher's side
    // inserts them into the publisher's handle table atomically.
    channel_msg_t staged;
    memset(&staged, 0, sizeof(staged));
    staged.header.type_hash   = accept_chan->type_hash;
    staged.header.inline_len  = (uint16_t)sizeof(rawnet_accept_info_t);
    staged.header.nhandles    = 2;
    staged.header.sender_pid  = (uint32_t)connector_pid;
    staged.in_flight_idx[0]   = tok_obj_idx(rd_a);   // server read-end of A
    staged.in_flight_idx[1]   = tok_obj_idx(wr_b);   // server write-end of B

    rawnet_accept_info_t *info = (rawnet_accept_info_t *)staged.inline_payload;
    info->connector_pid = connector_pid;
    info->connection_id = conn_id;
    info->_reserved     = 0;

    // Move rd_a and wr_b out of the connector's handle table BEFORE dropping
    // the message on the ring — chan_marshal_recv on the publisher side will
    // insert them into the publisher's handle table, and we must not have
    // them aliased in two tables.
    bool rd_a_removed = false;
    bool wr_b_removed = false;
    {
        uint32_t rd_a_idx = tok_obj_idx(rd_a);
        uint32_t wr_b_idx = tok_obj_idx(wr_b);
        for (uint32_t s = 0; s < connector->cap_handles.capacity; s++) {
            cap_handle_entry_t *h = cap_handle_lookup(&connector->cap_handles, s);
            if (!h) continue;
            if (!rd_a_removed && h->object_idx == rd_a_idx) {
                cap_handle_remove(&connector->cap_handles, s);
                rd_a_removed = true;
            } else if (!wr_b_removed && h->object_idx == wr_b_idx) {
                cap_handle_remove(&connector->cap_handles, s);
                wr_b_removed = true;
            }
            if (rd_a_removed && wr_b_removed) break;
        }
    }

    rc = chan_send(accept_chan, connector, &staged, 0);
    if (rc != 0) {
        // Rollback: destroy all four channels (which frees the underlying
        // channel_t once endpoint refcount hits zero), and restore any
        // handles we moved.  Since rd_a/wr_b are already out of the
        // connector's table we can just call cap_object_destroy on them;
        // the remaining wr_a / rd_b are still in the connector's table and
        // will be cleaned up by the same call.
        cap_object_destroy(tok_obj_idx(rd_a));
        cap_object_destroy(tok_obj_idx(wr_a));
        cap_object_destroy(tok_obj_idx(rd_b));
        cap_object_destroy(tok_obj_idx(wr_b));
        __atomic_fetch_sub(&accept_chan->refcount, 1, __ATOMIC_RELEASE);
        return rc;
    }

    *out_wr_req_tok  = wr_a;
    *out_rd_resp_tok = rd_b;

    __atomic_fetch_sub(&accept_chan->refcount, 1, __ATOMIC_RELEASE);

    audit_write_chan_name_connect(connector_pid, publisher_pid, name);
    klog(KLOG_INFO, SUBSYS_NET,
         "[rawnet] connect name=%s connector_pid=%d publisher_pid=%d conn_id=%u",
         name, (int)connector_pid, (int)publisher_pid, (unsigned)conn_id);
    return 0;
}

// ---------------------------------------------------------------------------
// rawnet_on_peer_death.
// ---------------------------------------------------------------------------
void rawnet_on_peer_death(int32_t pid) {
    if (!g_rawnet_ready || pid <= 0) return;

    spinlock_acquire(&g_rawnet_lock);
    uint32_t cleared = 0;
    for (uint32_t i = 0; i < RAWNET_SLOTS_MAX; i++) {
        rawnet_publish_entry_t *e = &g_rawnet_entries[i];
        if (e->name_len == 0) continue;
        if (e->publisher_pid != pid) continue;

        if (e->accept_channel) {
            __atomic_fetch_sub(&e->accept_channel->refcount, 1,
                               __ATOMIC_RELEASE);
        }
        klog(KLOG_INFO, SUBSYS_NET,
             "[rawnet] publisher pid=%d died; clearing name=%s",
             (int)pid, e->name);
        e->name[0] = 0;
        e->name_len = 0;
        clear_entry_runtime(e);
        cleared++;
    }
    spinlock_release(&g_rawnet_lock);

    if (cleared > 0) {
        klog(KLOG_INFO, SUBSYS_NET,
             "[rawnet] reaped %u published name(s) owned by pid=%d",
             cleared, (int)pid);
    }
}
