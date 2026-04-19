// kernel/io/stream.c — Phase 18.
//
// Stream lifecycle: create, destroy, ref/unref, CQE posting, HHDM page
// walker. Worker thread lives in worker.c (U5). Dispatchers live in
// dispatch_fs.c / dispatch_ipc.c (U8/U9). Reap blocking lives in U10.

#include "stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mm/slab.h"
#include "../mm/kheap.h"
#include "../mm/vmo.h"
#include "../cap/token.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../ipc/channel.h"
#include "../audit.h"
#include "../log.h"
#include "../panic.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// HHDM offset from arch/x86_64/mm/vmm.c.
extern uint64_t g_hhdm_offset;

// --- Subsystem globals ---------------------------------------------------
static kmem_cache_t *g_stream_cache = NULL;
static kmem_cache_t *g_streamjob_cache = NULL;
static kmem_cache_t *g_stream_ep_cache = NULL;
static uint64_t      g_next_stream_id = 1;
static spinlock_t    g_stream_id_lock = SPINLOCK_INITIALIZER("stream_id");

static uint64_t next_stream_id(void) {
    spinlock_acquire(&g_stream_id_lock);
    uint64_t id = g_next_stream_id++;
    spinlock_release(&g_stream_id_lock);
    return id;
}

// --- Forward declarations (worker lands in U5) ---------------------------
// stream_worker_init starts the kernel worker thread + global job queue.
// Declared weak here so a partial build (U4 without U5) still links.
void stream_worker_init(void);
__attribute__((weak)) void stream_worker_init(void) { }

void stream_subsystem_init(void) {
    g_stream_cache = kmem_cache_create("stream_t", sizeof(stream_t),
                                       _Alignof(stream_t), NULL, SUBSYS_CAP);
    g_streamjob_cache = kmem_cache_create("stream_job_t", sizeof(stream_job_t),
                                          _Alignof(stream_job_t), NULL, SUBSYS_CAP);
    g_stream_ep_cache = kmem_cache_create("stream_endpoint_t",
                                          sizeof(stream_endpoint_t),
                                          _Alignof(stream_endpoint_t),
                                          NULL, SUBSYS_CAP);
    if (!g_stream_cache || !g_streamjob_cache || !g_stream_ep_cache) {
        klog(KLOG_FATAL, SUBSYS_CAP,
             "stream_subsystem_init: slab alloc failed");
        return;
    }
    stream_worker_init();
    klog(KLOG_INFO, SUBSYS_CAP, "stream subsystem initialized");
}

// --- HHDM helpers --------------------------------------------------------
static inline void *phys_to_kv(uint64_t phys) {
    return (void *)(phys + g_hhdm_offset);
}

// Byte-granular access into a VMO via HHDM. The VMO's pages[] array holds
// physical frame addresses; (pages[i] + HHDM_OFFSET) is a valid kernel VA.
// Returns NULL if byte_offset is past the end.
static void *vmo_kva_at(struct vmo *v, uint64_t byte_offset) {
    if (!v) return NULL;
    uint32_t page_idx = (uint32_t)(byte_offset / STREAM_PAGE_SIZE);
    uint32_t page_off = (uint32_t)(byte_offset % STREAM_PAGE_SIZE);
    if (page_idx >= v->npages) return NULL;
    uint64_t phys = v->pages[page_idx];
    if (phys == 0) return NULL;
    return (void *)((uint64_t)phys_to_kv(phys) + page_off);
}

// Ring-entry accessors. Entries live starting at page 1 of each ring VMO
// (page 0 is the {head, tail, pad} metadata page).
sqe_t *stream_sqe_at(stream_t *s, uint32_t idx) {
    if (!s || idx >= s->sq_entries) return NULL;
    uint64_t byte_off = STREAM_ENTRIES_OFFSET + (uint64_t)idx * STREAM_SQE_SIZE;
    return (sqe_t *)vmo_kva_at(s->sq_vmo, byte_off);
}

cqe_t *stream_cqe_at(stream_t *s, uint32_t idx) {
    if (!s || idx >= s->cq_entries) return NULL;
    uint64_t byte_off = STREAM_ENTRIES_OFFSET + (uint64_t)idx * STREAM_CQE_SIZE;
    return (cqe_t *)vmo_kva_at(s->cq_vmo, byte_off);
}

// Accessors for head/tail ring pointers. Layout at page 0:
//   offset 0   : uint32_t head (producer)
//   offset 128 : uint32_t tail (consumer)
static inline volatile uint32_t *ring_head_ptr(void *meta_kva) {
    return (volatile uint32_t *)((uint8_t *)meta_kva + STREAM_META_HEAD_OFFSET);
}
static inline volatile uint32_t *ring_tail_ptr(void *meta_kva) {
    return (volatile uint32_t *)((uint8_t *)meta_kva + STREAM_META_TAIL_OFFSET);
}

// --- Ring geometry helpers -----------------------------------------------
static bool is_power_of_two_u32(uint32_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

static uint64_t ring_vmo_size(uint32_t entries, uint32_t entry_size) {
    uint64_t payload = (uint64_t)entries * entry_size;
    // Round payload up to a page, then add one metadata page at the front.
    uint64_t payload_rounded = (payload + STREAM_PAGE_SIZE - 1) &
                               ~((uint64_t)STREAM_PAGE_SIZE - 1);
    return STREAM_PAGE_SIZE + payload_rounded;
}

// --- Refcount helpers ----------------------------------------------------
void stream_ref(stream_t *s) {
    if (!s || s->magic != STREAM_MAGIC) return;
    spinlock_acquire(&s->lock);
    s->refcount++;
    spinlock_release(&s->lock);
}

static void stream_free(stream_t *s) {
    if (!s) return;
    // notify_endpoint was unref'd in stream_destroy; just make sure.
    if (s->sq_vmo) { vmo_unref(s->sq_vmo); s->sq_vmo = NULL; }
    if (s->cq_vmo) { vmo_unref(s->cq_vmo); s->cq_vmo = NULL; }
    s->magic = 0;
    kmem_cache_free(g_stream_cache, s);
}

void stream_unref(stream_t *s) {
    if (!s || s->magic != STREAM_MAGIC) return;
    spinlock_acquire(&s->lock);
    bool last = (--s->refcount == 0);
    spinlock_release(&s->lock);
    if (last) stream_free(s);
}

// --- Reap wake (used by stream_post_cqe; real block path in U10) ---------
void stream_wake_reapers(stream_t *s) {
    if (!s) return;
    // reap_waiters head is guarded by stream->lock, same as channel's read
    // waiters. Wake one reaper — if their min_complete hasn't been met yet
    // they will re-sleep.
    spinlock_acquire(&s->lock);
    struct task_struct **head = &s->reap_waiters;
    spinlock_release(&s->lock);
    // sched_wake_one_on_channel touches head under its own scheduler-side
    // discipline; safe to call without holding stream->lock.
    (void)sched_wake_one_on_channel(head, 0);
}

// Best-effort notify send. Re-resolves the caller-provided write endpoint
// via cap_token_resolve so handles closed after stream_create are handled
// cleanly. Sends a 32-byte message with type_hash = grahaos.io.completion.v1.
// Non-blocking (timeout_ns=0). On -EPIPE / stale token, clears
// notify_tok_raw so we stop retrying.
//
// Returns 0 on successful enqueue or when notify is disabled; negative
// errno is returned for diagnostic purposes but callers ignore it.
int stream_notify_channel(stream_t *s) {
    if (!s || s->magic != STREAM_MAGIC) return CAP_V2_EINVAL;

    // Snapshot under lock. Release before any chan_send that may spin.
    spinlock_acquire(&s->lock);
    uint64_t tok_raw = s->notify_tok_raw;
    int32_t  pid     = s->notify_caller_pid;
    spinlock_release(&s->lock);

    if (tok_raw == 0) return 0;

    cap_token_t tok = { .raw = tok_raw };
    cap_object_t *ep_obj = cap_token_resolve(pid, tok, RIGHT_SEND);
    if (!ep_obj || ep_obj->kind != CAP_KIND_CHANNEL) {
        // Handle closed or stale — disable notify silently.
        spinlock_acquire(&s->lock);
        if (s->notify_tok_raw == tok_raw) s->notify_tok_raw = 0;
        spinlock_release(&s->lock);
        return CAP_V2_EPIPE;
    }
    chan_endpoint_t *ep = (chan_endpoint_t *)ep_obj->kind_data;
    if (!ep || ep->direction != CHAN_ENDPOINT_WRITE) {
        spinlock_acquire(&s->lock);
        if (s->notify_tok_raw == tok_raw) s->notify_tok_raw = 0;
        spinlock_release(&s->lock);
        return CAP_V2_EPIPE;
    }
    channel_t *ch = ep->channel;
    if (!ch) return CAP_V2_EPIPE;

    // Build a one-off notification message. The kernel is the sender.
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    // Inline payload: 8-byte LE stream id + 8-byte ready count (for debug
    // diagnostics, not load-bearing for semantic correctness).
    msg.header.type_hash  = ch->type_hash;
    msg.header.sender_pid = (uint32_t)-1;  // kernel
    msg.header.inline_len = 16;
    msg.header.nhandles   = 0;
    uint64_t ready_total = s->total_completed;
    memcpy(&msg.inline_payload[0], &s->id, 8);
    memcpy(&msg.inline_payload[8], &ready_total, 8);

    // Non-blocking send. We can't use kernel task context for chan_send
    // directly since it expects a task_t*; use the notify_caller task as
    // the logical sender (it authorised the notify registration).
    task_t *sender_task = sched_get_task(pid);
    if (!sender_task) return CAP_V2_EPIPE;
    int r = chan_send(ch, sender_task, &msg, 0);
    if (r == CAP_V2_EPIPE) {
        spinlock_acquire(&s->lock);
        if (s->notify_tok_raw == tok_raw) s->notify_tok_raw = 0;
        spinlock_release(&s->lock);
    }
    // EAGAIN / full ring is expected under load; not an error for us.
    return r;
}

// --- CQE posting ---------------------------------------------------------
void stream_post_cqe(stream_t *s, uint64_t cookie, int64_t result,
                     uint32_t cqe_flags) {
    if (!s || s->magic != STREAM_MAGIC || !s->cq_meta_kva) return;

    volatile uint32_t *cq_head = ring_head_ptr(s->cq_meta_kva);
    volatile uint32_t *cq_tail = ring_tail_ptr(s->cq_meta_kva);

    // Check CQ space. Under spec invariants (cq_entries >= sq_entries and
    // active_jobs <= sq_entries) this never fires; kept as defensive.
    uint32_t cur_head = s->cq_head_kernel;
    uint32_t cur_tail = __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
    uint32_t in_flight = cur_head - cur_tail;
    if (in_flight >= s->cq_entries) {
        klog(KLOG_WARN, SUBSYS_CAP,
             "stream_post_cqe: CQ full (stream id=%lu head=%u tail=%u cap=%u)",
             (unsigned long)s->id, cur_head, cur_tail, s->cq_entries);
        return;
    }

    uint32_t slot = cur_head & (s->cq_entries - 1);
    cqe_t *entry = stream_cqe_at(s, slot);
    if (!entry) return;

    entry->cookie  = cookie;
    entry->result  = result;
    entry->flags   = cqe_flags;
    memset(entry->reserved, 0, sizeof(entry->reserved));

    // Publish the entry: RELEASE store to cq_head makes the payload visible
    // to any userspace reader who subsequently ACQUIRE-loads cq_head.
    uint32_t new_head = cur_head + 1;
    __atomic_store_n(cq_head, new_head, __ATOMIC_RELEASE);
    s->cq_head_kernel = new_head;
    s->total_completed++;

    stream_wake_reapers(s);
    (void)stream_notify_channel(s);
}

// --- Job completion ------------------------------------------------------
void stream_complete_job(stream_job_t *job, int64_t result) {
    if (!job || job->magic != STREAM_JOB_MAGIC) return;
    stream_t *s = job->stream;
    if (!s || s->magic != STREAM_MAGIC) {
        kmem_cache_free(g_streamjob_cache, job);
        return;
    }

    // If destroy flagged us canceling, flip the result regardless of what
    // the dispatcher computed.
    uint32_t jstate = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    uint32_t cqe_flags = 0;
    int64_t  final_res = result;
    if (jstate == JOB_STATE_CANCELING) {
        final_res = -125; /* -ECANCELED */
        cqe_flags = CQE_FLAG_CANCELED;
    }

    // Unlink job from stream->jobs_head.
    spinlock_acquire(&s->lock);
    stream_job_t **cursor = &s->jobs_head;
    while (*cursor) {
        if (*cursor == job) {
            *cursor = job->job_next;
            break;
        }
        cursor = &(*cursor)->job_next;
    }
    if (s->active_jobs > 0) s->active_jobs--;
    spinlock_release(&s->lock);

    stream_post_cqe(s, job->sqe_copy.cookie, final_res, cqe_flags);

    // Drop VMO ref if the op held one.
    if (job->dest_vmo) {
        vmo_unref(job->dest_vmo);
        job->dest_vmo = NULL;
    }
    // Free the job slot.
    job->magic = 0;
    kmem_cache_free(g_streamjob_cache, job);

    // Drop the per-job stream ref (may free the stream).
    stream_unref(s);
}

// --- Create --------------------------------------------------------------
int stream_create(uint64_t type_hash, uint32_t sq_entries, uint32_t cq_entries,
                  int32_t owner_pid, uint64_t notify_wr_handle_raw,
                  stream_handles_t *out) {
    if (!out) return CAP_V2_EFAULT;
    if (!is_power_of_two_u32(sq_entries) || !is_power_of_two_u32(cq_entries))
        return CAP_V2_EINVAL;
    if (sq_entries < STREAM_MIN_ENTRIES || sq_entries > STREAM_MAX_SQ_ENTRIES)
        return CAP_V2_EINVAL;
    if (cq_entries < sq_entries || cq_entries > STREAM_MAX_CQ_ENTRIES)
        return CAP_V2_EINVAL;

    const manifest_entry_t *mf = stream_lookup_manifest(type_hash);
    if (!mf || mf->op_count == 0) return CAP_V2_EPROTOTYPE;

    // Allocate the ring VMOs (zeroed; userspace and kernel both observe
    // zero-initialized head/tail pointers at metadata page 0).
    struct vmo *sq_vmo = vmo_create(ring_vmo_size(sq_entries, STREAM_SQE_SIZE),
                                    VMO_ZEROED, owner_pid, owner_pid);
    if (!sq_vmo) return CAP_V2_ENOMEM;
    struct vmo *cq_vmo = vmo_create(ring_vmo_size(cq_entries, STREAM_CQE_SIZE),
                                    VMO_ZEROED, owner_pid, owner_pid);
    if (!cq_vmo) { vmo_unref(sq_vmo); return CAP_V2_ENOMEM; }

    // Allocate the stream control block.
    stream_t *s = (stream_t *)kmem_cache_alloc(g_stream_cache);
    if (!s) {
        vmo_unref(sq_vmo);
        vmo_unref(cq_vmo);
        return CAP_V2_ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->magic        = STREAM_MAGIC;
    s->state        = STREAM_STATE_ACTIVE;
    s->id           = next_stream_id();
    s->type_hash    = type_hash;
    s->manifest     = mf;
    s->sq_vmo       = sq_vmo;
    s->cq_vmo       = cq_vmo;
    s->sq_meta_kva  = vmo_kva_at(sq_vmo, 0);
    s->cq_meta_kva  = vmo_kva_at(cq_vmo, 0);
    s->sq_entries   = sq_entries;
    s->cq_entries   = cq_entries;
    s->sq_tail_kernel = 0;
    s->cq_head_kernel = 0;
    s->notify_tok_raw    = 0;
    s->notify_caller_pid = owner_pid;
    s->notify_pad0       = 0;
    s->owner_pid    = owner_pid;
    s->refcount     = 1;   // held by the CAP_KIND_STREAM cap_object below
    s->active_jobs  = 0;
    s->total_submitted = 0;
    s->total_completed = 0;
    s->rejected_submissions = 0;
    s->reap_waiters = NULL;
    s->jobs_head    = NULL;
    spinlock_init(&s->lock, "stream");

    if (!s->sq_meta_kva || !s->cq_meta_kva) {
        vmo_unref(sq_vmo);
        vmo_unref(cq_vmo);
        kmem_cache_free(g_stream_cache, s);
        return CAP_V2_ENOMEM;
    }

    // Optional notify channel: validate the caller-provided write endpoint
    // now so we return -EBADF up-front if it's bogus. At send time we
    // re-resolve the token via cap_token_resolve so a handle closed in the
    // meantime is handled cleanly without UAF.
    if (notify_wr_handle_raw != 0) {
        cap_token_t notify_tok = { .raw = notify_wr_handle_raw };
        cap_object_t *ep_obj = cap_token_resolve(owner_pid, notify_tok,
                                                 RIGHT_SEND);
        if (!ep_obj || ep_obj->kind != CAP_KIND_CHANNEL) {
            vmo_unref(sq_vmo);
            vmo_unref(cq_vmo);
            kmem_cache_free(g_stream_cache, s);
            return CAP_V2_EBADF;
        }
        s->notify_tok_raw    = notify_wr_handle_raw;
        s->notify_caller_pid = owner_pid;
    }

    // Wrap each VMO in a CAP_KIND_VMO cap_object for the caller.
    int32_t audience[CAP_AUDIENCE_MAX + 1];
    audience[0] = owner_pid;
    audience[1] = PID_NONE;

    int sq_idx = cap_object_create(CAP_KIND_VMO,
                                   RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                       RIGHT_DERIVE | RIGHT_REVOKE,
                                   audience, 0,
                                   (uintptr_t)sq_vmo, owner_pid,
                                   CAP_OBJECT_IDX_NONE);
    if (sq_idx < 0) {
        vmo_unref(sq_vmo);
        vmo_unref(cq_vmo);
        kmem_cache_free(g_stream_cache, s);
        return sq_idx;
    }
    // Register the cap_object_idx into the VMO for introspection parity
    // with Phase 17.
    sq_vmo->cap_object_idx = (uint32_t)sq_idx;

    int cq_idx = cap_object_create(CAP_KIND_VMO,
                                   RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                       RIGHT_DERIVE | RIGHT_REVOKE,
                                   audience, 0,
                                   (uintptr_t)cq_vmo, owner_pid,
                                   CAP_OBJECT_IDX_NONE);
    if (cq_idx < 0) {
        cap_object_destroy((uint32_t)sq_idx);
        vmo_unref(cq_vmo);  /* sq_vmo destroyed via cap_object_destroy path */
        kmem_cache_free(g_stream_cache, s);
        return cq_idx;
    }
    cq_vmo->cap_object_idx = (uint32_t)cq_idx;

    // Wrap the stream itself.
    stream_endpoint_t *ep = (stream_endpoint_t *)kmem_cache_alloc(g_stream_ep_cache);
    if (!ep) {
        cap_object_destroy((uint32_t)sq_idx);
        cap_object_destroy((uint32_t)cq_idx);
        kmem_cache_free(g_stream_cache, s);
        return CAP_V2_ENOMEM;
    }
    ep->stream = s;
    memset(ep->reserved, 0, sizeof(ep->reserved));

    int st_idx = cap_object_create(CAP_KIND_STREAM,
                                   RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                       RIGHT_DERIVE | RIGHT_REVOKE,
                                   audience, 0,
                                   (uintptr_t)ep, owner_pid,
                                   CAP_OBJECT_IDX_NONE);
    if (st_idx < 0) {
        kmem_cache_free(g_stream_ep_cache, ep);
        cap_object_destroy((uint32_t)sq_idx);
        cap_object_destroy((uint32_t)cq_idx);
        kmem_cache_free(g_stream_cache, s);
        return st_idx;
    }

    // Insert each handle into caller's handle table for ownership tracking.
    task_t *t = sched_get_task(owner_pid);
    if (!t) {
        cap_object_destroy((uint32_t)st_idx);
        cap_object_destroy((uint32_t)sq_idx);
        cap_object_destroy((uint32_t)cq_idx);
        kmem_cache_free(g_stream_cache, s);
        return CAP_V2_EINVAL;
    }

    uint32_t st_slot = 0, sq_slot = 0, cq_slot = 0;
    int ins = cap_handle_insert(&t->cap_handles, (uint32_t)st_idx, 0, &st_slot);
    if (ins < 0) goto insert_fail_st;
    ins = cap_handle_insert(&t->cap_handles, (uint32_t)sq_idx, 0, &sq_slot);
    if (ins < 0) { cap_handle_remove(&t->cap_handles, st_slot); goto insert_fail_st; }
    ins = cap_handle_insert(&t->cap_handles, (uint32_t)cq_idx, 0, &cq_slot);
    if (ins < 0) {
        cap_handle_remove(&t->cap_handles, sq_slot);
        cap_handle_remove(&t->cap_handles, st_slot);
        goto insert_fail_st;
    }

    // Pack tokens referencing the *global* cap_object idx plus its current
    // generation (same pattern as Phase 17 chan_create).
    cap_object_t *st_obj = g_cap_object_ptrs[st_idx];
    cap_object_t *sq_obj = g_cap_object_ptrs[sq_idx];
    cap_object_t *cq_obj = g_cap_object_ptrs[cq_idx];
    uint32_t st_gen = st_obj ? __atomic_load_n(&st_obj->generation, __ATOMIC_ACQUIRE) : 0;
    uint32_t sq_gen = sq_obj ? __atomic_load_n(&sq_obj->generation, __ATOMIC_ACQUIRE) : 0;
    uint32_t cq_gen = cq_obj ? __atomic_load_n(&cq_obj->generation, __ATOMIC_ACQUIRE) : 0;

    out->stream_handle  = cap_token_pack(st_gen, (uint32_t)st_idx, 0).raw;
    out->sq_vmo_handle  = cap_token_pack(sq_gen, (uint32_t)sq_idx, 0).raw;
    out->cq_vmo_handle  = cap_token_pack(cq_gen, (uint32_t)cq_idx, 0).raw;
    out->reserved       = 0;
    return 0;

insert_fail_st:
    cap_object_destroy((uint32_t)st_idx);
    cap_object_destroy((uint32_t)sq_idx);
    cap_object_destroy((uint32_t)cq_idx);
    kmem_cache_free(g_stream_cache, s);
    return CAP_V2_ENOMEM;
}

// --- Helper: find op_schema row for an opcode ---------------------------
const op_schema_t *stream_find_op(const stream_t *s, uint16_t op) {
    if (!s || !s->manifest) return NULL;
    for (uint16_t i = 0; i < s->manifest->op_count; i++) {
        if (s->manifest->ops[i].op == op) return &s->manifest->ops[i];
    }
    return NULL;
}

// --- Helpers: job slab alloc/free ---------------------------------------
stream_job_t *stream_job_alloc(void) {
    stream_job_t *j = (stream_job_t *)kmem_cache_alloc(g_streamjob_cache);
    if (!j) return NULL;
    memset(j, 0, sizeof(*j));
    return j;
}

void stream_job_free_raw(stream_job_t *job) {
    if (!job) return;
    job->magic = 0;
    kmem_cache_free(g_streamjob_cache, job);
}

// --- Destroy -------------------------------------------------------------
void stream_destroy(stream_t *s) {
    if (!s || s->magic != STREAM_MAGIC) return;

    uint32_t cancelled = 0;

    spinlock_acquire(&s->lock);
    if (s->state != STREAM_STATE_ACTIVE) {
        spinlock_release(&s->lock);
        return;
    }
    s->state = STREAM_STATE_DESTROYING;
    // Clear notify registration — post-destroy CQEs (the cancellation ones)
    // should not pointlessly try to notify.
    s->notify_tok_raw = 0;

    // Flag every outstanding job for cancellation. We do NOT free queued
    // jobs here — they're still linked in the global worker FIFO, and the
    // worker would see freed slabs. Instead the worker picks them up as
    // usual, the dispatcher sees state=JOB_STATE_CANCELING, and routes the
    // -ECANCELED CQE via stream_complete_job.
    stream_job_t *cur = s->jobs_head;
    while (cur) {
        __atomic_store_n(&cur->state, JOB_STATE_CANCELING, __ATOMIC_RELEASE);
        cancelled++;
        cur = cur->job_next;
    }
    spinlock_release(&s->lock);

    if (cancelled > 0) {
        audit_write_stream_destroy_canceled(s->owner_pid,
                                            (uint32_t)s->id, cancelled);
    }
}

// --- Reap ---------------------------------------------------------------
// Return count of CQEs currently ready. Blocks up to timeout_ns for at
// least min_complete to be ready. timeout_ns == 0 probes non-blocking.
// timeout_ns == UINT64_MAX means wait forever.
int stream_reap(stream_t *s, uint32_t min_complete, uint64_t timeout_ns) {
    if (!s || s->magic != STREAM_MAGIC) return CAP_V2_EBADF;
    if (s->state == STREAM_STATE_DESTROYED) return CAP_V2_EBADF;
    if (min_complete > s->cq_entries) return CAP_V2_EINVAL;

    volatile uint32_t *cq_tail = ring_tail_ptr(s->cq_meta_kva);

    uint32_t ready = s->cq_head_kernel -
                     __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
    if (ready >= min_complete || timeout_ns == 0) {
        return (int)ready;
    }

    // Block once; re-evaluate on wake. The waker (stream_post_cqe) calls
    // stream_wake_reapers which pops one task off reap_waiters per CQE.
    // Multiple CQEs may fire between enqueue-wake and context-switch;
    // after wake, we always return the current ready count — if less than
    // min_complete, the caller can loop at userspace level.
    int r = sched_block_on_channel(/*channel=*/s, WAIT_STREAM_REAP,
                                   timeout_ns, &s->reap_waiters);
    ready = s->cq_head_kernel -
            __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
    if (r == CAP_V2_ETIMEDOUT && ready < min_complete) return CAP_V2_ETIMEDOUT;
    if (r == CAP_V2_EPIPE) return (int)ready;  // stream destroyed mid-wait
    return (int)ready;
}

// --- cap_object deactivate ----------------------------------------------
// Defined here (non-weak) — overrides the weak stub in kernel/cap/object.c.
void stream_endpoint_deactivate(struct cap_object *obj) {
    if (!obj) return;
    stream_endpoint_t *ep = (stream_endpoint_t *)obj->kind_data;
    if (!ep) return;
    stream_t *s = ep->stream;
    if (s && s->magic == STREAM_MAGIC) {
        // First destroy: state transitions to DESTROYING, outstanding
        // jobs get cancellation markers. Then drop the cap-held refcount.
        stream_destroy(s);
        stream_unref(s);
    }
    kmem_cache_free(g_stream_ep_cache, ep);
    obj->kind_data = 0;
}
