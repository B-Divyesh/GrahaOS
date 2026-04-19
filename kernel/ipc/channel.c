// kernel/ipc/channel.c — Phase 17.
#include "channel.h"
#include "manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mm/slab.h"
#include "../mm/kheap.h"
#include "../cap/token.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../audit.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// --- Subsystem globals ---------------------------------------------------
static kmem_cache_t *g_channel_cache = NULL;
static kmem_cache_t *g_endpoint_cache = NULL;
static uint64_t g_next_chan_id = 1;
static spinlock_t g_chan_id_lock = SPINLOCK_INITIALIZER("chan_id");

static uint64_t next_chan_id(void) {
    spinlock_acquire(&g_chan_id_lock);
    uint64_t id = g_next_chan_id++;
    spinlock_release(&g_chan_id_lock);
    return id;
}

void channel_subsystem_init(void) {
    g_channel_cache  = kmem_cache_create("channel_t", sizeof(channel_t),
                                         _Alignof(channel_t), NULL, SUBSYS_CAP);
    g_endpoint_cache = kmem_cache_create("chan_endpoint_t",
                                         sizeof(chan_endpoint_t),
                                         _Alignof(chan_endpoint_t), NULL,
                                         SUBSYS_CAP);
    if (!g_channel_cache || !g_endpoint_cache) {
        klog(KLOG_FATAL, SUBSYS_CAP, "channel_subsystem_init: slab alloc failed");
        return;
    }
    klog(KLOG_INFO, SUBSYS_CAP, "channel subsystem initialized");
}

static bool chan_check(channel_t *c) {
    return c && c->magic == CHANNEL_MAGIC && c->capacity > 0 && c->ring;
}

// --- Lifecycle -----------------------------------------------------------
int chan_create(uint64_t type_hash, uint32_t mode, uint32_t capacity,
                int32_t caller_pid,
                cap_token_t *rd_tok_out, cap_token_t *wr_tok_out) {
    if (!rd_tok_out || !wr_tok_out) return CAP_V2_EFAULT;
    if (capacity == 0 || capacity > CHAN_CAPACITY_MAX) return CAP_V2_EINVAL;
    if (mode != CHAN_MODE_BLOCKING && mode != CHAN_MODE_NONBLOCKING)
        return CAP_V2_EINVAL;
    if (!manifest_type_known(type_hash)) return CAP_V2_EPROTOTYPE;

    channel_t *c = (channel_t *)kmem_cache_alloc(g_channel_cache);
    if (!c) return CAP_V2_ENOMEM;
    c->magic       = CHANNEL_MAGIC;
    c->mode        = mode;
    c->id          = next_chan_id();
    c->type_hash   = type_hash;
    c->capacity    = capacity;
    c->head        = 0;
    c->tail        = 0;
    c->msgcount    = 0;
    c->refcount    = 2;  // two endpoint caps hold refs
    c->seq_next    = 0;
    c->total_sends = 0;
    c->total_recvs = 0;
    c->rejected_messages = 0;
    c->owner_pid   = caller_pid;
    c->read_waiters  = NULL;
    c->write_waiters = NULL;
    spinlock_init(&c->lock, "channel");

    c->ring = (channel_msg_t *)kmalloc(sizeof(channel_msg_t) * capacity, SUBSYS_CAP);
    if (!c->ring) {
        kmem_cache_free(g_channel_cache, c);
        return CAP_V2_ENOMEM;
    }
    memset(c->ring, 0, sizeof(channel_msg_t) * capacity);

    // Allocate endpoint payloads.
    chan_endpoint_t *rd_ep = (chan_endpoint_t *)kmem_cache_alloc(g_endpoint_cache);
    chan_endpoint_t *wr_ep = (chan_endpoint_t *)kmem_cache_alloc(g_endpoint_cache);
    if (!rd_ep || !wr_ep) {
        if (rd_ep) kmem_cache_free(g_endpoint_cache, rd_ep);
        if (wr_ep) kmem_cache_free(g_endpoint_cache, wr_ep);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return CAP_V2_ENOMEM;
    }
    rd_ep->channel   = c;
    rd_ep->direction = CHAN_ENDPOINT_READ;
    wr_ep->channel   = c;
    wr_ep->direction = CHAN_ENDPOINT_WRITE;

    // Create cap_objects. Audience = [caller_pid] only.
    int32_t audience[CAP_AUDIENCE_MAX + 1];
    audience[0] = caller_pid;
    audience[1] = PID_NONE;

    int rd_idx = cap_object_create(CAP_KIND_CHANNEL,
                                   RIGHT_READ | RIGHT_RECV | RIGHT_INSPECT |
                                       RIGHT_DERIVE | RIGHT_REVOKE,
                                   audience,
                                   0,
                                   (uintptr_t)rd_ep,
                                   caller_pid,
                                   CAP_OBJECT_IDX_NONE);
    if (rd_idx < 0) {
        kmem_cache_free(g_endpoint_cache, rd_ep);
        kmem_cache_free(g_endpoint_cache, wr_ep);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return rd_idx;
    }

    int wr_idx = cap_object_create(CAP_KIND_CHANNEL,
                                   RIGHT_WRITE | RIGHT_SEND | RIGHT_INSPECT |
                                       RIGHT_DERIVE | RIGHT_REVOKE,
                                   audience,
                                   0,
                                   (uintptr_t)wr_ep,
                                   caller_pid,
                                   CAP_OBJECT_IDX_NONE);
    if (wr_idx < 0) {
        // Revoke rd_idx cleanly.
        cap_object_destroy((uint32_t)rd_idx);
        kmem_cache_free(g_endpoint_cache, wr_ep);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return wr_idx;
    }

    c->read_cap_idx  = (uint32_t)rd_idx;
    c->write_cap_idx = (uint32_t)wr_idx;

    // Insert handles into caller's handle table.
    task_t *t = sched_get_task(caller_pid);
    if (!t) {
        cap_object_destroy((uint32_t)rd_idx);
        cap_object_destroy((uint32_t)wr_idx);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return CAP_V2_EINVAL;
    }

    // Insert into the caller's handle table for ownership tracking. The
    // returned slot index is NOT what the token carries; the token packs
    // the cap_object's global idx (so cap_token_resolve can look it up
    // lock-free in g_cap_object_ptrs).
    uint32_t rd_slot = 0, wr_slot = 0;
    int rd_ins = cap_handle_insert(&t->cap_handles, (uint32_t)rd_idx, 0, &rd_slot);
    if (rd_ins < 0) {
        cap_object_destroy((uint32_t)rd_idx);
        cap_object_destroy((uint32_t)wr_idx);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return rd_ins;
    }
    int wr_ins = cap_handle_insert(&t->cap_handles, (uint32_t)wr_idx, 0, &wr_slot);
    if (wr_ins < 0) {
        cap_handle_remove(&t->cap_handles, rd_slot);
        cap_object_destroy((uint32_t)rd_idx);
        cap_object_destroy((uint32_t)wr_idx);
        kfree(c->ring);
        kmem_cache_free(g_channel_cache, c);
        return wr_ins;
    }

    // Token idx = cap_object idx; gen comes from the cap_object's atomic
    // generation. cap_token_resolve uses both to verify aliveness.
    cap_object_t *rd_obj = g_cap_object_ptrs[rd_idx];
    cap_object_t *wr_obj = g_cap_object_ptrs[wr_idx];
    uint32_t rd_gen = rd_obj ? __atomic_load_n(&rd_obj->generation, __ATOMIC_ACQUIRE) : 0;
    uint32_t wr_gen = wr_obj ? __atomic_load_n(&wr_obj->generation, __ATOMIC_ACQUIRE) : 0;

    *rd_tok_out = cap_token_pack(rd_gen, (uint32_t)rd_idx, 0);
    *wr_tok_out = cap_token_pack(wr_gen, (uint32_t)wr_idx, 0);
    return 0;
}

// --- Helpers -------------------------------------------------------------
static void chan_free(channel_t *c) {
    if (!chan_check(c)) return;
    if (c->ring) {
        kfree(c->ring);
        c->ring = NULL;
    }
    c->magic = 0;
    kmem_cache_free(g_channel_cache, c);
}

channel_t *chan_create_kernel(uint64_t type_hash, uint32_t mode,
                              uint32_t capacity) {
    if (capacity == 0 || capacity > CHAN_CAPACITY_MAX) return NULL;
    if (mode != CHAN_MODE_BLOCKING && mode != CHAN_MODE_NONBLOCKING)
        return NULL;

    channel_t *c = (channel_t *)kmem_cache_alloc(g_channel_cache);
    if (!c) return NULL;
    c->magic       = CHANNEL_MAGIC;
    c->mode        = mode;
    c->id          = next_chan_id();
    c->type_hash   = type_hash;
    c->capacity    = capacity;
    c->head        = 0;
    c->tail        = 0;
    c->msgcount    = 0;
    c->refcount    = 2;  // read + write virtual endpoints
    c->seq_next    = 0;
    c->total_sends = 0;
    c->total_recvs = 0;
    c->rejected_messages = 0;
    c->owner_pid   = -1;
    c->read_cap_idx  = 0;
    c->write_cap_idx = 0;
    c->read_waiters  = NULL;
    c->write_waiters = NULL;
    spinlock_init(&c->lock, "channel");

    c->ring = (channel_msg_t *)kmalloc(sizeof(channel_msg_t) * capacity, SUBSYS_CAP);
    if (!c->ring) {
        kmem_cache_free(g_channel_cache, c);
        return NULL;
    }
    memset(c->ring, 0, sizeof(channel_msg_t) * capacity);
    return c;
}

void chan_destroy_kernel(channel_t *c) {
    if (!chan_check(c)) return;
    spinlock_acquire(&c->lock);
    sched_wake_all_on_channel(&c->read_waiters, CAP_V2_EPIPE);
    sched_wake_all_on_channel(&c->write_waiters, CAP_V2_EPIPE);
    c->refcount = 0;
    spinlock_release(&c->lock);
    chan_free(c);
}

void chan_endpoint_deactivate(cap_object_t *obj) {
    if (!obj) return;
    chan_endpoint_t *ep = (chan_endpoint_t *)obj->kind_data;
    if (!ep) return;
    channel_t *c = ep->channel;
    if (chan_check(c)) {
        spinlock_acquire(&c->lock);
        // Wake all waiters with -EPIPE as the channel is about to die on
        // this side. This ensures no task spins forever after closure.
        sched_wake_all_on_channel(&c->read_waiters, CAP_V2_EPIPE);
        sched_wake_all_on_channel(&c->write_waiters, CAP_V2_EPIPE);
        bool last = (--c->refcount == 0);
        spinlock_release(&c->lock);
        if (last) chan_free(c);
    }
    kmem_cache_free(g_endpoint_cache, ep);
    obj->kind_data = 0;
}

// --- Core send/recv ------------------------------------------------------
int chan_send(channel_t *c, task_t *sender, channel_msg_t *msg_kern,
              uint64_t timeout_ns) {
    if (!chan_check(c) || !sender || !msg_kern) return CAP_V2_EINVAL;

    // Type-hash gate.
    if (msg_kern->header.type_hash != c->type_hash) {
        c->rejected_messages++;
        audit_write_chan_type_mismatch(sender->id, c->write_cap_idx,
                                       c->type_hash, msg_kern->header.type_hash);
        return CAP_V2_EPROTOTYPE;
    }

    while (1) {
        spinlock_acquire(&c->lock);
        if (c->msgcount < c->capacity) {
            // Space available — install and advance.
            channel_msg_t *slot = &c->ring[c->tail];
            msg_kern->header.sender_pid = (uint32_t)sender->id;
            msg_kern->header.seq        = c->seq_next++;
            *slot = *msg_kern;
            c->tail = (c->tail + 1) % c->capacity;
            c->msgcount++;
            c->total_sends++;
            sched_wake_one_on_channel(&c->read_waiters, 0);
            spinlock_release(&c->lock);
            return 0;
        }
        // Full.
        spinlock_release(&c->lock);
        if (c->mode == CHAN_MODE_NONBLOCKING || timeout_ns == 0) {
            return CAP_V2_EAGAIN;
        }
        int rc = sched_block_on_channel(c, CHAN_WAIT_WRITE, timeout_ns,
                                        (struct task_struct **)&c->write_waiters);
        if (rc != 0) return rc;
        // Loop and retry insertion.
    }
}

int chan_recv(channel_t *c, task_t *receiver, channel_msg_t *msg_kern,
              uint64_t timeout_ns) {
    if (!chan_check(c) || !receiver || !msg_kern) return CAP_V2_EINVAL;

    while (1) {
        spinlock_acquire(&c->lock);
        if (c->msgcount > 0) {
            channel_msg_t *slot = &c->ring[c->head];
            *msg_kern = *slot;
            // Clear handles in the slot so a later double-read can't steal.
            memset(slot->in_flight_idx, 0, sizeof(slot->in_flight_idx));
            c->head = (c->head + 1) % c->capacity;
            c->msgcount--;
            c->total_recvs++;
            sched_wake_one_on_channel(&c->write_waiters, 0);
            spinlock_release(&c->lock);
            return (int)msg_kern->header.inline_len;
        }
        // Empty.
        // Peer closed? refcount==1 means only one side (ours) remains.
        if (c->refcount < 2) {
            spinlock_release(&c->lock);
            return CAP_V2_EPIPE;
        }
        spinlock_release(&c->lock);
        if (c->mode == CHAN_MODE_NONBLOCKING || timeout_ns == 0) {
            return CAP_V2_EAGAIN;
        }
        int rc = sched_block_on_channel(c, CHAN_WAIT_READ, timeout_ns,
                                        (struct task_struct **)&c->read_waiters);
        if (rc != 0) return rc;
    }
}

uint32_t chan_poll_probe(channel_t *c) {
    if (!chan_check(c)) return 0;
    uint32_t revents = 0;
    spinlock_acquire(&c->lock);
    if (c->msgcount > 0) revents |= 0x1;
    if (c->msgcount < c->capacity) revents |= 0x2;
    if (c->refcount < 2) revents |= 0x4;
    spinlock_release(&c->lock);
    return revents;
}

// --- Marshaling + atomic handle transfer --------------------------------
//
// SEND (msg_copyin): sender provides a chan_msg_user_t in kernel memory
// (already copied from user by the syscall handler). We validate header
// fields, copy inline bytes, then for each handle resolve it in sender's
// table, stage the object_idx, and (on full success) remove all from
// sender's table atomically. Rollback is trivial since removal is the
// last step.
int chan_marshal_send(task_t *sender,
                      const chan_msg_user_t *user_msg,
                      channel_msg_t *staged) {
    if (!sender || !user_msg || !staged) return CAP_V2_EFAULT;
    if (user_msg->header.inline_len > CHAN_MSG_INLINE_MAX) return CAP_V2_EINVAL;
    if (user_msg->header.nhandles > CHAN_MSG_HANDLES_MAX) return CAP_V2_EINVAL;

    // Copy header + inline payload.
    staged->header = user_msg->header;
    memset(staged->in_flight_idx, 0, sizeof(staged->in_flight_idx));
    if (user_msg->header.inline_len > 0) {
        memcpy(staged->inline_payload, user_msg->inline_payload,
               user_msg->header.inline_len);
    }

    // Phase 1: resolve each handle. Token IDX field is the cap_object_idx
    // (cap_token_resolve confirmed valid + audience + rights).
    uint32_t staged_obj_idx[CHAN_MSG_HANDLES_MAX];
    for (uint8_t i = 0; i < user_msg->header.nhandles; i++) {
        cap_token_t tok = user_msg->handles[i];
        cap_object_t *obj = cap_token_resolve(sender->id, tok, 0);
        if (!obj) return CAP_V2_EPERM;
        // Token's IDX field is the global cap_object index.
        uint32_t obj_idx = (uint32_t)((tok.raw >> 8) & 0xFFFFFFu);
        staged->in_flight_idx[i] = obj_idx;
        staged_obj_idx[i] = obj_idx;
    }

    // Phase 2: remove each from sender's handle table by object_idx scan.
    // (Sender's table holds {slot → object_idx}; we walk to find matching.)
    for (uint8_t i = 0; i < user_msg->header.nhandles; i++) {
        for (uint32_t s = 0; s < sender->cap_handles.capacity; s++) {
            cap_handle_entry_t *e = cap_handle_lookup(&sender->cap_handles, s);
            if (e && e->object_idx == staged_obj_idx[i]) {
                cap_handle_remove(&sender->cap_handles, s);
                break;
            }
        }
    }

    return 0;
}

// RECV (msg_copyout): copy ring slot → user-shaped message, inserting each
// in-flight object_idx into the receiver's handle table to produce fresh
// tokens.
int chan_marshal_recv(task_t *receiver,
                      const channel_msg_t *slot,
                      chan_msg_user_t *user_msg) {
    if (!receiver || !slot || !user_msg) return CAP_V2_EFAULT;

    user_msg->header = slot->header;
    memset(user_msg->handles, 0, sizeof(user_msg->handles));
    if (slot->header.inline_len > 0) {
        memcpy(user_msg->inline_payload, slot->inline_payload,
               slot->header.inline_len);
    }

    uint32_t inserted[CHAN_MSG_HANDLES_MAX];
    uint8_t inserted_count = 0;
    for (uint8_t i = 0; i < slot->header.nhandles; i++) {
        uint32_t obj_idx = slot->in_flight_idx[i];
        if (obj_idx == 0) continue;
        uint32_t new_slot = 0;
        int rc_ins = cap_handle_insert(&receiver->cap_handles, obj_idx, 0,
                                        &new_slot);
        if (rc_ins < 0) {
            for (uint8_t j = 0; j < inserted_count; j++) {
                cap_handle_remove(&receiver->cap_handles, inserted[j]);
            }
            return rc_ins;
        }
        // Token = (object_generation, object_idx, 0). The token-idx field
        // is the GLOBAL cap_object index, not the receiver's handle slot.
        cap_object_t *o = g_cap_object_ptrs[obj_idx];
        uint32_t ogen = o ? __atomic_load_n(&o->generation, __ATOMIC_ACQUIRE) : 0;
        user_msg->handles[i] = cap_token_pack(ogen, obj_idx, 0);
        inserted[inserted_count++] = new_slot;
    }

    audit_write_handle_transfer((int32_t)slot->header.sender_pid,
                                receiver->id,
                                slot->in_flight_idx[0],
                                slot->header.nhandles);
    return 0;
}

// --- Endpoint resolve ----------------------------------------------------
int chan_resolve_endpoint(int32_t caller_pid, cap_token_t tok, uint8_t dir,
                          uint64_t required_rights,
                          channel_t **out_channel, uint32_t *out_obj_idx) {
    if (!out_channel || !out_obj_idx) return CAP_V2_EFAULT;
    cap_object_t *obj = cap_token_resolve(caller_pid, tok, required_rights);
    if (!obj) return CAP_V2_EPERM;
    if (obj->kind != CAP_KIND_CHANNEL) return CAP_V2_EINVAL;
    chan_endpoint_t *ep = (chan_endpoint_t *)obj->kind_data;
    if (!ep || !chan_check(ep->channel)) return CAP_V2_EINVAL;
    if (ep->direction != dir) return CAP_V2_EPERM;
    *out_channel = ep->channel;
    // Compute the cap_object idx by scanning the registry (small, ≤ 64).
    // Better: cap_object_t itself holds no idx; we need an accessor. Use
    // the endpoint's known channel-side idx as a proxy.
    *out_obj_idx = (dir == CHAN_ENDPOINT_READ) ? ep->channel->read_cap_idx
                                               : ep->channel->write_cap_idx;
    return 0;
}
