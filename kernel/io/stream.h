// kernel/io/stream.h
//
// Phase 18: Submission Streams (Async I/O).
//
// A stream is a submission-queue (SQ) / completion-queue (CQ) ring pair
// backed by two VMOs shared between userspace (producer of SQ, consumer of
// CQ) and kernel (consumer of SQ, producer of CQ). A stream is bound at
// creation time to a manifest type hash (e.g. "grahaos.io.v1") whose
// op_schema table enumerates the allowed opcodes and their dispatchers.
//
// -------------------------------------------------------------------------
// Ring memory layout (identical on both sides — kernel HHDM and userspace
// mapped view observe the same physical pages):
//
//     Page 0 of each VMO holds the metadata header:
//
//         offset   0 .. 3   : uint32_t head   (producer-advance counter)
//         offset   4 .. 127 : pad (cache line separation)
//         offset 128 ..131  : uint32_t tail   (consumer-advance counter)
//         offset 132 ..4095 : pad (unused)
//
//     Pages 1 .. N hold the entries, densely packed. SQE is 64 bytes, CQE
//     is 32 bytes. Index i lives at byte offset PAGE_SIZE + i * sizeof(entry).
//
// The head/tail placement on separate cache lines avoids false sharing
// between the producer's head writes and the consumer's tail writes — a
// silent 10x throughput trap otherwise.
//
// -------------------------------------------------------------------------
// Memory-ordering discipline (io_uring-style, mandatory):
//
//   Producer side (kernel posting a CQE):
//     1. write CQE payload at cq[cq_head_kernel]               (plain stores)
//     2. __atomic_store_n(&cq_head_shared, new,
//                         __ATOMIC_RELEASE);                  (publishes payload)
//
//   Consumer side (userspace reaping CQEs):
//     1. head = __atomic_load_n(&cq_head_shared, __ATOMIC_ACQUIRE);
//     2. plain reads of cq[tail..head)
//     3. __atomic_store_n(&cq_tail_shared, new, __ATOMIC_RELEASE);
//
//   Kernel reads cq_tail_shared with ACQUIRE before posting a CQE to
//   check for CQ overflow (cq_head_kernel - cq_tail_shared >= cq_entries).
//   Symmetric discipline governs the SQ: userspace RELEASE-stores sq_head;
//   kernel ACQUIRE-loads sq_head, plain-reads SQEs, RELEASE-stores sq_tail.
//
// Deviations from this protocol cause data races that pass 99.99% of tests
// under -smp 1 and corrupt silently under -smp >= 2.
//
// -------------------------------------------------------------------------
// Lifecycle & refcounting:
//
//   stream_t owns:
//     - refcount separate from active_jobs. stream_endpoint_deactivate
//       (called when the last cap_object holding CAP_KIND_STREAM is closed)
//       drops one ref; each in-flight stream_job_t holds one ref.
//     - sq_vmo / cq_vmo (vmo_t *) — the stream holds one vmo_ref each,
//       dropped in stream_free via vmo_unref.
//     - notify_endpoint (cap_object_t *) — optional write-end of a channel.
//       ref held via cap_object_ref, dropped in stream_destroy.
//
//   stream_destroy transitions state ACTIVE -> DESTROYING (one-way), walks
//   all jobs: queued jobs get an immediate CQE with -ECANCELED, running
//   jobs have job->state atomic-RELEASE-stored to JOB_CANCELING so the
//   dispatcher on its way out posts a cancel-flagged CQE. Worker's per-job
//   stream_ref is always dropped whether the job cancels or completes.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../sync/spinlock.h"

// Forward declarations (avoid pulling heavy headers).
struct vmo;
struct cap_object;
struct task_struct;
struct stream_job;

// ------------------------------------------------------------------------
// Magic canaries. Verified at every syscall entry; mismatch => kpanic.
// ------------------------------------------------------------------------
#define STREAM_MAGIC      0xCAFE5712u
#define STREAM_JOB_MAGIC  0xC0FFEE18u

// ------------------------------------------------------------------------
// Ring geometry constants.
// ------------------------------------------------------------------------
#define STREAM_PAGE_SIZE          4096u
#define STREAM_SQE_SIZE           64u
#define STREAM_CQE_SIZE           32u
#define STREAM_MIN_ENTRIES        16u
#define STREAM_MAX_SQ_ENTRIES     4096u
#define STREAM_MAX_CQ_ENTRIES     8192u
// Metadata layout inside page 0.
#define STREAM_META_HEAD_OFFSET   0u
#define STREAM_META_TAIL_OFFSET   128u
#define STREAM_ENTRIES_OFFSET     STREAM_PAGE_SIZE

// ------------------------------------------------------------------------
// Stream state enum (stream_t.state). Monotonic: ACTIVE -> DESTROYING.
// ------------------------------------------------------------------------
#define STREAM_STATE_ACTIVE       1u
#define STREAM_STATE_DESTROYING   2u
#define STREAM_STATE_DESTROYED    3u

// ------------------------------------------------------------------------
// Job state enum (stream_job_t.state). Atomic RELEASE/ACQUIRE accessed.
// ------------------------------------------------------------------------
#define JOB_STATE_QUEUED      1u
#define JOB_STATE_RUNNING     2u
#define JOB_STATE_CANCELING   3u
#define JOB_STATE_DONE        4u

// ------------------------------------------------------------------------
// Opcodes. Until U3's gen_manifest.py runs, these are hand-written; the
// generator will replace the numeric assignments with ones from gcp.json.
// ------------------------------------------------------------------------
#define OP_NONE          0u
#define OP_READ_VMO      1u
#define OP_WRITE_VMO     2u
#define OP_SENDMSG       3u
#define OP_OPEN          4u
#define OP_STAT          5u
#define OP_FSYNC         6u
#define OP_CLOSE         7u
#define OP_MAX           7u

// ------------------------------------------------------------------------
// SQE / CQE flag bits.
// ------------------------------------------------------------------------
#define SQE_FLAG_IO_DRAIN   0x0001u  // reserved; wait for prior ops to complete
#define SQE_FLAG_IO_LINK    0x0002u  // reserved; chain next SQE after this

#define CQE_FLAG_CANCELED   0x0001u  // job was cancelled by stream_destroy
#define CQE_FLAG_MORE       0x0002u  // reserved: multi-shot completion

// ------------------------------------------------------------------------
// sqe_t: user-written, kernel-read. 64 bytes fixed.
// ------------------------------------------------------------------------
typedef struct sqe {
    uint16_t op;                  //   0..1   OP_* opcode
    uint16_t flags;               //   2..3   SQE_FLAG_*
    uint32_t fd_or_handle;        //   4..7   op-dependent: VFS fd (READ/WRITE/OPEN/STAT/FSYNC/CLOSE) or cap_handle_t slot (SENDMSG)
    uint64_t offset;              //   8..15  byte offset into source (read) or dest (write)
    uint64_t len;                 //  16..23  bytes to transfer, or 0 for "native op"
    uint32_t dest_vmo_handle;     //  24..27  destination VMO cap_handle_t slot (READ/WRITE)
    uint32_t _pad0;               //  28..31  reserved zero
    uint64_t dest_vmo_offset;     //  32..39  offset into dest_vmo
    uint64_t cookie;              //  40..47  user-chosen tag echoed in CQE
    uint8_t  reserved[16];        //  48..63  reserved zero
} sqe_t;

_Static_assert(sizeof(sqe_t) == 64, "sqe_t must be 64 bytes");

// ------------------------------------------------------------------------
// cqe_t: kernel-written, user-read. 32 bytes fixed.
// ------------------------------------------------------------------------
typedef struct cqe {
    uint64_t cookie;              //   0..7   echo of SQE.cookie
    int64_t  result;              //   8..15  >=0 success (typically bytes), negative errno on failure
    uint32_t flags;               //  16..19  CQE_FLAG_*
    uint8_t  reserved[12];        //  20..31  reserved zero
} cqe_t;

_Static_assert(sizeof(cqe_t) == 32, "cqe_t must be 32 bytes");

// ------------------------------------------------------------------------
// op_schema_t: per-op dispatcher + pledge mask. Built once at startup from
// the generated op_schema_io_v1[] table. See kernel/io/manifest_ops.c.
// ------------------------------------------------------------------------
typedef struct op_schema {
    uint16_t      op;
    uint16_t      _pad;
    uint64_t      required_pledge_mask;
    int         (*dispatcher)(struct stream_job *job);
    const char   *name;
} op_schema_t;

// ------------------------------------------------------------------------
// Per-type manifest registry entry. One per well-known stream manifest type.
// The kernel resolves create-time type_hash to this row and uses ops to
// validate every submission.
// ------------------------------------------------------------------------
typedef struct manifest_entry {
    uint64_t            type_hash;
    const op_schema_t  *ops;        // array of op_count entries, sorted by op
    uint16_t            op_count;
    uint16_t            _pad[3];
    const char         *type_name;  // "grahaos.io.v1"
} manifest_entry_t;

// ------------------------------------------------------------------------
// stream_t: kernel-side control block.
// ------------------------------------------------------------------------
typedef struct stream {
    uint32_t        magic;                  // STREAM_MAGIC
    uint32_t        state;                  // STREAM_STATE_*
    uint64_t        id;                     // monotonically assigned at create
    uint64_t        type_hash;              // cached from manifest lookup
    const manifest_entry_t *manifest;       // resolved at create
    struct vmo     *sq_vmo;                 // ref held
    struct vmo     *cq_vmo;                 // ref held
    // Kernel HHDM pointers to page 0 of each ring's metadata header. Lives in
    // the producer side; head/tail accessed via pointer arithmetic within.
    void           *sq_meta_kva;            // points into sq_vmo page 0
    void           *cq_meta_kva;            // points into cq_vmo page 0
    uint32_t        sq_entries;             // power of two
    uint32_t        cq_entries;             // power of two
    uint32_t        sq_tail_kernel;         // kernel's consumer cursor into SQ (== tail producer)
    uint32_t        cq_head_kernel;         // kernel's producer cursor into CQ (== head producer)
    // Notify endpoint: caller-provided CAP_KIND_CHANNEL write endpoint. We
    // store the raw cap_token and the caller pid so each notify re-resolves
    // via cap_token_resolve. If the caller closed the handle in the meantime
    // the resolve fails and we clear notify_tok_raw. This trades one extra
    // resolve per CQE post for freedom from cap_object lifetime races.
    uint64_t notify_tok_raw;
    int32_t  notify_caller_pid;
    int32_t  notify_pad0;
    int32_t         owner_pid;              // submitter PID for audit
    uint32_t        refcount;               // cap-held + per-inflight-job refs
    uint32_t        active_jobs;            // in-flight stream_job_t's
    uint64_t        total_submitted;
    uint64_t        total_completed;
    uint64_t        rejected_submissions;
    // Waiter list: tasks blocked in SYS_STREAM_REAP waiting for >=min_complete
    // CQEs. Head set/unlinked under stream->lock; wake path uses
    // sched_wake_one_on_channel (generic over list heads).
    struct task_struct *reap_waiters;
    // All in-flight jobs linked via stream_job_t.job_next, so stream_destroy
    // can walk them and cancel.
    struct stream_job  *jobs_head;
    spinlock_t      lock;
} stream_t;

// ------------------------------------------------------------------------
// stream_job_t: per-submission kernel record.
// ------------------------------------------------------------------------
typedef struct stream_job {
    uint32_t        magic;               // STREAM_JOB_MAGIC
    uint32_t        state;               // JOB_STATE_* (atomic accessed)
    stream_t       *stream;              // owning stream (holds ref)
    sqe_t           sqe_copy;            // kernel snapshot of the SQE (TOCTOU-free)
    int32_t         submitter_pid;
    // Resolved dispatch-time references. Taken at submit time with
    // vmo_ref()/cap_object_ref() so process death doesn't free them
    // mid-dispatch. NULL if op doesn't use them.
    struct vmo     *dest_vmo;            // for OP_READ_VMO / OP_WRITE_VMO
    // Worker FIFO linkage (g_worker_queue).
    struct stream_job *worker_next;
    // Per-stream job list linkage (stream->jobs_head).
    struct stream_job *job_next;
    // Scratch slot for dispatchers.
    uint64_t        dispatcher_arg;
} stream_job_t;

// ------------------------------------------------------------------------
// stream_endpoint_t: payload at cap_object_t.kind_data for CAP_KIND_STREAM.
// 16 bytes, matches Phase 17's chan_endpoint_t layout idiom.
// ------------------------------------------------------------------------
typedef struct stream_endpoint {
    stream_t *stream;
    uint8_t   reserved[8];
} stream_endpoint_t;

// ------------------------------------------------------------------------
// stream_handles_t: syscall out-struct returned by SYS_STREAM_CREATE.
// Userspace has the same layout in user/syscalls.h with _Static_assert.
// ------------------------------------------------------------------------
typedef struct stream_handles {
    uint64_t stream_handle;      // cap_token_t raw for CAP_KIND_STREAM
    uint64_t sq_vmo_handle;      // cap_token_t raw for CAP_KIND_VMO (SQ)
    uint64_t cq_vmo_handle;      // cap_token_t raw for CAP_KIND_VMO (CQ)
    uint64_t reserved;           // reserved zero (padding / future)
} stream_handles_t;

_Static_assert(sizeof(stream_handles_t) == 32, "stream_handles_t must be 32 bytes");

// ------------------------------------------------------------------------
// Public API.
// ------------------------------------------------------------------------

// Initialise slab caches, register default op_schema table, start worker
// thread. Called once at boot from kernel/main.c.
void stream_subsystem_init(void);

// Look up the manifest_entry_t for a given type_hash. Returns NULL if the
// hash is not in the registered manifest table.
const manifest_entry_t *stream_lookup_manifest(uint64_t type_hash);

// Lifecycle.
int stream_create(uint64_t type_hash, uint32_t sq_entries, uint32_t cq_entries,
                  int32_t owner_pid, uint64_t notify_wr_handle_raw,
                  stream_handles_t *out);
void stream_destroy(stream_t *s);

void stream_ref(stream_t *s);
void stream_unref(stream_t *s);

// Called from cap_object_destroy switch (kernel/cap/object.c).
void stream_endpoint_deactivate(struct cap_object *obj);

// Post a CQE into the CQ ring, bump head with RELEASE, wake reapers, and
// best-effort notify the subscribed channel if any.
void stream_post_cqe(stream_t *s, uint64_t cookie, int64_t result,
                     uint32_t cqe_flags);

// Completion entry point used by dispatchers. Unlinks job from stream's
// jobs_head list, posts the CQE, drops the per-job stream_ref + VMO ref,
// and frees the slab slot.
void stream_complete_job(stream_job_t *job, int64_t result);

// Wake one reaper blocked on stream->reap_waiters.
void stream_wake_reapers(stream_t *s);

// Best-effort notify-channel send. Returns 0 on success, negative errno on
// unrecoverable failure (which also clears notify_endpoint).
int  stream_notify_channel(stream_t *s);

// Look up SQE at logical index (i % sq_entries) via HHDM mapping. Returns a
// pointer into sq_vmo page contents; no allocation.
sqe_t *stream_sqe_at(stream_t *s, uint32_t idx);

// Look up CQE slot at logical index (i % cq_entries). Same contract.
cqe_t *stream_cqe_at(stream_t *s, uint32_t idx);

// Reap helper. Returns number of CQEs ready; blocks with timeout if below
// min_complete; returns -ETIMEDOUT if timeout expires.
int stream_reap(stream_t *s, uint32_t min_complete, uint64_t timeout_ns);

// SQ submission path. Consumes up to n_to_submit SQEs from sq_head_shared;
// returns the number of SQEs processed (including immediate rejections,
// which get inline CQEs). Never throws -EAGAIN — partial progress is a
// first-class success.
int stream_submit_batch(stream_t *s, uint32_t n_to_submit, int32_t caller_pid);

// Lookup the op_schema row for a given opcode in the stream's manifest.
// Returns NULL if op is not registered.
const op_schema_t *stream_find_op(const stream_t *s, uint16_t op);

// Slab-allocate a zeroed stream_job_t. Returns NULL on exhaustion. Caller
// populates fields and links into stream->jobs_head + worker queue.
stream_job_t *stream_job_alloc(void);
// Slab-free a stream_job_t after the dispatch completed or was cancelled
// before linking. Does NOT touch stream refcount or dest_vmo refcount —
// caller is responsible.
void stream_job_free_raw(stream_job_t *job);

// Worker entry — exported so submit_batch can hand off jobs.
void stream_worker_enqueue(stream_job_t *job);
