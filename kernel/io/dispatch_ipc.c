// kernel/io/dispatch_ipc.c — Phase 18.
//
// OP_SENDMSG dispatcher: non-blocking send into a channel referenced by
// the SQE's fd_or_handle cap_handle slot. Inline payload is copied from
// the job's dest_vmo at dest_vmo_offset (up to 256 bytes). Useful for the
// high-rate small-message telemetry patterns (spec gate G7).

#include "stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../cap/token.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../ipc/channel.h"
#include "../mm/vmo.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

extern uint64_t g_hhdm_offset;

// Resolve sqe.fd_or_handle (a cap_object idx, not a handle-table slot)
// to a CAP_KIND_CHANNEL write endpoint. Audience-checks the submitter.
static channel_t *resolve_channel(int32_t submitter_pid, uint32_t obj_idx) {
    (void)submitter_pid;  // audience check below
    cap_object_t *obj = cap_object_get(obj_idx);
    if (!obj || obj->kind != CAP_KIND_CHANNEL) return NULL;
    bool in_audience = false;
    for (int i = 0; i < CAP_AUDIENCE_MAX; i++) {
        int32_t a = obj->audience_set[i];
        if (a == PID_NONE) break;
        if (a == (int32_t)PID_PUBLIC || a == submitter_pid) {
            in_audience = true;
            break;
        }
    }
    if (!in_audience) return NULL;
    chan_endpoint_t *ep = (chan_endpoint_t *)obj->kind_data;
    if (!ep || ep->direction != CHAN_ENDPOINT_WRITE) return NULL;
    return ep->channel;
}

// Copy up to `len` bytes out of a VMO's HHDM-mapped page into a linear
// destination. Used to pull the SQE's payload into a channel message.
static uint32_t copy_from_vmo(uint8_t *dst, const struct vmo *v,
                              uint64_t src_offset, uint32_t max) {
    if (!v || max == 0) return 0;
    if (src_offset >= v->size_bytes) return 0;
    if (src_offset + max > v->size_bytes) {
        max = (uint32_t)(v->size_bytes - src_offset);
    }
    uint32_t copied = 0;
    uint64_t off = src_offset;
    uint32_t remaining = max;
    while (remaining > 0) {
        uint32_t page_idx = (uint32_t)(off / 4096);
        uint32_t page_off = (uint32_t)(off % 4096);
        uint32_t chunk = 4096 - page_off;
        if (chunk > remaining) chunk = remaining;
        if (page_idx >= v->npages) break;
        uint64_t phys = v->pages[page_idx];
        if (phys == 0) break;
        memcpy(dst + copied, (void *)(phys + g_hhdm_offset + page_off), chunk);
        copied += chunk;
        off += chunk;
        remaining -= chunk;
    }
    return copied;
}

int dispatch_sendmsg(stream_job_t *job) {
    if (!job) return 0;
    if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == JOB_STATE_CANCELING) {
        stream_complete_job(job, CAP_V2_ECANCELED);
        return 0;
    }

    channel_t *ch = resolve_channel(job->submitter_pid,
                                    job->sqe_copy.fd_or_handle);
    if (!ch) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }

    task_t *sender = sched_get_task(job->submitter_pid);
    if (!sender) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }

    // Build a staged channel message. Type hash must match the channel's
    // (chan_send itself rechecks and audits if it doesn't).
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type_hash  = ch->type_hash;
    msg.header.sender_pid = sender->id;
    msg.header.inline_len = 0;
    msg.header.nhandles   = 0;

    if (job->dest_vmo && job->sqe_copy.len > 0) {
        uint32_t to_copy = (uint32_t)job->sqe_copy.len;
        if (to_copy > CHAN_MSG_INLINE_MAX) to_copy = CHAN_MSG_INLINE_MAX;
        uint32_t copied = copy_from_vmo(msg.inline_payload, job->dest_vmo,
                                        job->sqe_copy.dest_vmo_offset, to_copy);
        msg.header.inline_len = (uint16_t)copied;
    }

    // Non-blocking send (timeout_ns=0 → return -EAGAIN on full ring).
    int r = chan_send(ch, sender, &msg, 0);
    int64_t cqe_result = (r == 0) ? (int64_t)msg.header.inline_len : (int64_t)r;
    stream_complete_job(job, cqe_result);
    return 0;
}
