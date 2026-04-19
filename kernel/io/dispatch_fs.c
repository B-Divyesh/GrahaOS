// kernel/io/dispatch_fs.c — Phase 18.
//
// File-system op dispatchers. Invoked by the stream worker thread after
// submit_batch hands a stream_job_t off. Each dispatcher must call
// stream_complete_job() exactly once — that routes the CQE to the CQ
// ring, drops the per-job stream refcount, and releases the slab slot.

#include "stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../fs/vfs.h"
#include "../mm/vmo.h"
#include "../cap/token.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// HHDM offset for VMO page-physical to kernel-virtual conversion.
extern uint64_t g_hhdm_offset;

// Resolve the submitter's FD into a vfs_node_t pointer. For Phase 18 MVP
// the node is not refcounted at submit time; instead we trust stream_destroy
// to flag jobs CANCELING before callers die. This helper returns NULL if
// the fd doesn't resolve cleanly.
static vfs_node_t *node_for_submitter_fd(int32_t submitter_pid, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    task_t *t = sched_get_task(submitter_pid);
    if (!t) return NULL;
    proc_fd_t *pf = &t->fd_table[fd];
    if (pf->type != FD_TYPE_FILE) return NULL;
    return vfs_node_for_file_slot(pf->ref);
}

// Copy up to `len` bytes between a contiguous kernel buffer and a VMO slice.
// Handles cross-page boundaries; VMO pages are HHDM-mapped so the kernel
// reads them via (pa + g_hhdm_offset). Returns bytes transferred (may be
// less than `len` on VMO bound or short I/O) or negative errno.
static int64_t read_into_vmo(vfs_node_t *node, uint64_t src_offset,
                             struct vmo *dst, uint64_t dst_offset,
                             uint64_t len) {
    if (!node || !node->read || !dst) return CAP_V2_EINVAL;
    if (dst_offset + len > dst->size_bytes) return CAP_V2_EINVAL;

    int64_t total = 0;
    uint64_t s_off = src_offset;
    uint64_t d_off = dst_offset;
    uint64_t remaining = len;

    while (remaining > 0) {
        uint32_t page_idx = (uint32_t)(d_off / 4096);
        uint32_t page_off = (uint32_t)(d_off % 4096);
        uint32_t chunk = 4096 - page_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        if (page_idx >= dst->npages) break;
        uint64_t phys = dst->pages[page_idx];
        if (phys == 0) break;
        void *dst_kva = (void *)(phys + g_hhdm_offset + page_off);
        ssize_t n = node->read(node, s_off, chunk, dst_kva);
        if (n < 0) return (int64_t)n;
        if (n == 0) break;
        total += n;
        if ((uint32_t)n < chunk) break;  // short read
        s_off += chunk;
        d_off += chunk;
        remaining -= chunk;
    }
    return total;
}

static int64_t write_from_vmo(vfs_node_t *node, uint64_t dst_offset,
                              struct vmo *src, uint64_t src_offset,
                              uint64_t len) {
    if (!node || !node->write || !src) return CAP_V2_EINVAL;
    if (src_offset + len > src->size_bytes) return CAP_V2_EINVAL;

    int64_t total = 0;
    uint64_t s_off = src_offset;
    uint64_t d_off = dst_offset;
    uint64_t remaining = len;

    while (remaining > 0) {
        uint32_t page_idx = (uint32_t)(s_off / 4096);
        uint32_t page_off = (uint32_t)(s_off % 4096);
        uint32_t chunk = 4096 - page_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        if (page_idx >= src->npages) break;
        uint64_t phys = src->pages[page_idx];
        if (phys == 0) break;
        void *src_kva = (void *)(phys + g_hhdm_offset + page_off);
        ssize_t n = node->write(node, d_off, chunk, src_kva);
        if (n < 0) return (int64_t)n;
        if (n == 0) break;
        total += n;
        if ((uint32_t)n < chunk) break;
        s_off += chunk;
        d_off += chunk;
        remaining -= chunk;
    }
    return total;
}

// ---------------------------------------------------------------------------
// OP_READ_VMO: read `len` bytes from fd at offset into dest_vmo.
// ---------------------------------------------------------------------------
int dispatch_read_vmo(stream_job_t *job) {
    if (!job) return 0;

    // Cancelled before we ran?
    if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == JOB_STATE_CANCELING) {
        stream_complete_job(job, CAP_V2_ECANCELED);
        return 0;
    }
    if (!job->dest_vmo) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }

    vfs_node_t *node = node_for_submitter_fd(job->submitter_pid,
                                             (int)job->sqe_copy.fd_or_handle);
    if (!node) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }

    int64_t r = read_into_vmo(node, job->sqe_copy.offset, job->dest_vmo,
                              job->sqe_copy.dest_vmo_offset, job->sqe_copy.len);
    stream_complete_job(job, r);
    return 0;
}

// ---------------------------------------------------------------------------
// OP_WRITE_VMO: write `len` bytes from dest_vmo to fd at offset.
// ---------------------------------------------------------------------------
int dispatch_write_vmo(stream_job_t *job) {
    if (!job) return 0;
    if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == JOB_STATE_CANCELING) {
        stream_complete_job(job, CAP_V2_ECANCELED);
        return 0;
    }
    if (!job->dest_vmo) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }
    vfs_node_t *node = node_for_submitter_fd(job->submitter_pid,
                                             (int)job->sqe_copy.fd_or_handle);
    if (!node) {
        stream_complete_job(job, CAP_V2_EBADF);
        return 0;
    }

    int64_t r = write_from_vmo(node, job->sqe_copy.offset, job->dest_vmo,
                               job->sqe_copy.dest_vmo_offset, job->sqe_copy.len);
    stream_complete_job(job, r);
    return 0;
}

// ---------------------------------------------------------------------------
// Stubs for OP_OPEN / OP_STAT / OP_FSYNC / OP_CLOSE. Phase 19 provides real
// dispatchers. The weak fallbacks in manifest_ops.c return -ENOSYS — these
// strong symbols override them with the same behaviour but route through
// stream_complete_job so the CQE lands cleanly.
// ---------------------------------------------------------------------------
int dispatch_open_stub(stream_job_t *job) {
    stream_complete_job(job, -38 /* -ENOSYS */);
    return 0;
}
int dispatch_stat_stub(stream_job_t *job) {
    stream_complete_job(job, -38);
    return 0;
}
int dispatch_fsync_stub(stream_job_t *job) {
    stream_complete_job(job, -38);
    return 0;
}
int dispatch_close_stub(stream_job_t *job) {
    stream_complete_job(job, -38);
    return 0;
}
