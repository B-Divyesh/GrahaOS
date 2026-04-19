// kernel/io/submit.c — Phase 18.
//
// SQE-to-job pipeline. Reads SQEs from the stream's SQ VMO (via HHDM),
// validates each against the stream's manifest op_schema, resolves any
// capability handles (dest_vmo for OP_READ_VMO / OP_WRITE_VMO) with
// vmo_ref captured now so the resource survives caller death, allocates
// a stream_job_t, and hands the job off to the worker thread.
//
// Rejections (unknown op, pledge denied, invalid handle) are reported as
// immediate CQEs with a negative errno so the caller never sees a silent
// "op lost" — spec AW-18.3.

#include "stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../cap/token.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/pledge.h"
#include "../mm/vmo.h"
#include "../audit.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// Access to the ring metadata page. Duplicated in stream.c; tiny helper —
// keeping it here avoids a public API on stream.h. Page 0 offset 0 holds
// head, offset 128 holds tail.
static inline volatile uint32_t *sq_head_ptr(void *meta_kva) {
    return (volatile uint32_t *)((uint8_t *)meta_kva + STREAM_META_HEAD_OFFSET);
}
static inline volatile uint32_t *sq_tail_ptr(void *meta_kva) {
    return (volatile uint32_t *)((uint8_t *)meta_kva + STREAM_META_TAIL_OFFSET);
}

// Resolve a dest_vmo_handle (cap_object idx) to a vmo_t *. The SQE field
// carries the IDX bits of a cap_token_t (bits 8..31 of the token raw),
// which is the global cap_object index — not a per-process handle-table
// slot. Kernel looks up the cap_object directly, checks that the
// submitter is in the audience, and bumps the VMO refcount so the
// pointer is valid until stream_complete_job.
static struct vmo *resolve_dest_vmo(task_t *submitter, uint32_t obj_idx) {
    if (!submitter) return NULL;
    cap_object_t *obj = cap_object_get(obj_idx);
    if (!obj || obj->kind != CAP_KIND_VMO) return NULL;
    // Audience check: submitter must be in the VMO's audience list or
    // the audience must be PUBLIC.
    bool in_audience = false;
    for (int i = 0; i < CAP_AUDIENCE_MAX; i++) {
        int32_t a = obj->audience_set[i];
        if (a == PID_NONE) break;
        if (a == (int32_t)PID_PUBLIC || a == submitter->id) {
            in_audience = true;
            break;
        }
    }
    if (!in_audience) return NULL;
    struct vmo *v = (struct vmo *)obj->kind_data;
    if (!v) return NULL;
    vmo_ref(v);
    return v;
}

// Emit an immediate rejection CQE for a bad SQE. Always posts; always
// increments rejected_submissions counter. No job is allocated.
static void reject_sqe(stream_t *s, int32_t caller_pid, const sqe_t *sqe,
                       int32_t errcode, const char *reason) {
    spinlock_acquire(&s->lock);
    s->rejected_submissions++;
    spinlock_release(&s->lock);
    audit_write_stream_op_rejected(caller_pid, (uint32_t)s->id,
                                   sqe->op, errcode, reason);
    stream_post_cqe(s, sqe->cookie, (int64_t)errcode, 0);
}

// Main submission loop.
int stream_submit_batch(stream_t *s, uint32_t n_to_submit, int32_t caller_pid) {
    if (!s || s->magic != STREAM_MAGIC) return CAP_V2_EBADF;
    if (s->state != STREAM_STATE_ACTIVE) return CAP_V2_EBADF;
    if (n_to_submit > s->sq_entries) return CAP_V2_EINVAL;

    task_t *submitter = sched_get_task(caller_pid);
    if (!submitter) return CAP_V2_EBADF;

    // ACQUIRE the userspace-visible SQ head; this publishes any SQE
    // payloads written by userspace prior to its own RELEASE store.
    volatile uint32_t *sq_head = sq_head_ptr(s->sq_meta_kva);
    volatile uint32_t *sq_tail = sq_tail_ptr(s->sq_meta_kva);
    uint32_t sq_head_shared = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);

    int processed = 0;
    for (uint32_t i = 0; i < n_to_submit; i++) {
        uint32_t slot_k = s->sq_tail_kernel;
        if (slot_k == sq_head_shared) break;  // no more visible SQEs

        sqe_t *ring_slot = stream_sqe_at(s, slot_k & (s->sq_entries - 1));
        if (!ring_slot) break;
        sqe_t snap = *ring_slot;   // snapshot to avoid TOCTOU

        // Always consume this SQE, even on rejection. sq_tail_kernel
        // advances; the RELEASE publish at end gives userspace slot-free.
        s->sq_tail_kernel = slot_k + 1;
        processed++;

        // --- Validate op is in the stream's manifest -------------------
        const op_schema_t *schema = stream_find_op(s, snap.op);
        if (!schema) {
            reject_sqe(s, caller_pid, &snap, CAP_V2_EPROTOTYPE, "unknown op");
            continue;
        }

        // --- Per-op pledge check ---------------------------------------
        // pledge_allows() takes a class bit position (0..11). The generated
        // op_schema stores a full bitmask (e.g., PLEDGE_FS_READ == 1u<<0).
        // Bitmask intersect suffices: nonzero means at least one required
        // class is satisfied. For MVP all ops require exactly one class,
        // so this is equivalent to the classic pledge check.
        if (schema->required_pledge_mask) {
            if ((submitter->pledge_mask.raw & schema->required_pledge_mask) == 0) {
                reject_sqe(s, caller_pid, &snap, CAP_V2_EPLEDGE, schema->name);
                continue;
            }
        }

        // --- Resolve dest_vmo for read/write ops -----------------------
        struct vmo *dest = NULL;
        if (snap.op == OP_READ_VMO || snap.op == OP_WRITE_VMO) {
            dest = resolve_dest_vmo(submitter, snap.dest_vmo_handle);
            if (!dest) {
                reject_sqe(s, caller_pid, &snap, CAP_V2_EBADF, "dest_vmo");
                continue;
            }
        }

        // --- Allocate and populate the job ----------------------------
        stream_job_t *job = stream_job_alloc();
        if (!job) {
            if (dest) vmo_unref(dest);
            // Partial submit: stop consuming. sq_tail_kernel was already
            // advanced, so rewind by one so the SQE is retried next call.
            s->sq_tail_kernel = slot_k;
            processed--;
            break;
        }
        job->magic         = STREAM_JOB_MAGIC;
        job->state         = JOB_STATE_QUEUED;
        job->stream        = s;
        job->sqe_copy      = snap;
        job->submitter_pid = caller_pid;
        job->dest_vmo      = dest;

        // --- Link into stream->jobs_head + bump stream ref -----------
        stream_ref(s);
        spinlock_acquire(&s->lock);
        job->job_next = s->jobs_head;
        s->jobs_head  = job;
        s->active_jobs++;
        s->total_submitted++;
        spinlock_release(&s->lock);

        // --- Hand off to worker --------------------------------------
        stream_worker_enqueue(job);
    }

    // RELEASE-publish the consumer cursor so userspace observes freed SQ
    // slots. (Spec 18 does not require submit to be blocking on SQ-full,
    // but publishing tail promptly is how backpressure is communicated.)
    __atomic_store_n(sq_tail, s->sq_tail_kernel, __ATOMIC_RELEASE);
    return processed;
}
