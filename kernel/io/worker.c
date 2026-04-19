// kernel/io/worker.c — Phase 18.
//
// Single global kernel worker thread for stream dispatch. Phase 20 promotes
// to per-CPU queues + work stealing. For now the thread drains a FIFO job
// queue; dispatchers run inline (they themselves are sync but the caller's
// submission was asynchronous — the batching win is still substantial).
//
// Lost-wakeup discipline: the worker blocks with a short (10 ms / one tick)
// timeout rather than indefinite. If an enqueuer slips in between the
// worker's "queue empty" check and its transition to CHAN_WAIT, the wake
// is a no-op — but the timeout-driven re-check picks up the job within one
// timer tick. For the hot-busy case the enqueuer's wake fires normally and
// the worker resumes immediately.

#include "stream.h"

#include <stddef.h>
#include <stdint.h>

#include "../sync/spinlock.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// Queue globals. Not exported — only stream_worker_enqueue() and the main
// loop touch them, both gated by g_worker_queue.lock.
typedef struct stream_worker_queue {
    stream_job_t       *head;
    stream_job_t       *tail;
    uint32_t            count;
    struct task_struct *waiters;   // single-worker today; list-head for symmetry
    spinlock_t          lock;
} stream_worker_queue_t;

static stream_worker_queue_t g_worker_queue;

// Block timeout when the queue is empty. One timer tick (10 ms) is short
// enough that any lost wake is invisible to the user but long enough to
// avoid busy-spinning the CPU on idle.
#define STREAM_WORKER_IDLE_TICK_NS 10000000ULL

// Exported: the submit path calls this to hand a job off to the worker.
void stream_worker_enqueue(stream_job_t *job) {
    if (!job) return;
    spinlock_acquire(&g_worker_queue.lock);
    job->worker_next = NULL;
    if (g_worker_queue.tail) {
        g_worker_queue.tail->worker_next = job;
    } else {
        g_worker_queue.head = job;
    }
    g_worker_queue.tail = job;
    g_worker_queue.count++;
    spinlock_release(&g_worker_queue.lock);

    // Wake the worker. Safe to call without holding queue lock — the wake
    // only acts if the worker is in CHAN_WAIT; otherwise it is a no-op.
    sched_wake_one_on_channel(&g_worker_queue.waiters, 0);
}

// Pop the queue head atomically. Returns NULL if empty.
static stream_job_t *worker_pop(void) {
    spinlock_acquire(&g_worker_queue.lock);
    stream_job_t *job = g_worker_queue.head;
    if (job) {
        g_worker_queue.head = job->worker_next;
        if (!g_worker_queue.head) g_worker_queue.tail = NULL;
        g_worker_queue.count--;
    }
    spinlock_release(&g_worker_queue.lock);
    if (job) job->worker_next = NULL;
    return job;
}

// Find the op_schema row for a job. Returns NULL if the op was not in the
// stream's manifest (should not happen — submit_validate enforces this).
static const op_schema_t *worker_find_schema(stream_t *s, uint16_t op) {
    if (!s || !s->manifest) return NULL;
    for (uint16_t i = 0; i < s->manifest->op_count; i++) {
        if (s->manifest->ops[i].op == op) return &s->manifest->ops[i];
    }
    return NULL;
}

// Main loop.
static void stream_worker_main(void) {
    klog(KLOG_INFO, SUBSYS_CAP, "stream_worker_main: online");
    for (;;) {
        // Drain as many jobs as we can before sleeping.
        stream_job_t *job;
        while ((job = worker_pop()) != NULL) {
            if (!job->stream || job->magic != STREAM_JOB_MAGIC) {
                // Defensive: corrupt job. Log and skip.
                klog(KLOG_WARN, SUBSYS_CAP,
                     "stream_worker_main: bad job magic=0x%x", job->magic);
                continue;
            }
            __atomic_store_n(&job->state, JOB_STATE_RUNNING, __ATOMIC_RELEASE);
            const op_schema_t *schema = worker_find_schema(job->stream,
                                                           job->sqe_copy.op);
            if (schema && schema->dispatcher) {
                (void)schema->dispatcher(job);
                // Dispatcher contract: must call stream_complete_job once,
                // which frees the slab and drops refcounts.
            } else {
                stream_complete_job(job, -38 /* -ENOSYS */);
            }
        }

        // Queue empty — block with a short timeout so a lost wake can only
        // delay us by one tick.
        (void)sched_block_on_channel(/*channel=*/&g_worker_queue,
                                     WAIT_STREAM_WORKER,
                                     STREAM_WORKER_IDLE_TICK_NS,
                                     &g_worker_queue.waiters);
    }
}

// Initialise queue state and create the worker kernel thread. Called from
// stream_subsystem_init().
void stream_worker_init(void) {
    g_worker_queue.head    = NULL;
    g_worker_queue.tail    = NULL;
    g_worker_queue.count   = 0;
    g_worker_queue.waiters = NULL;
    spinlock_init(&g_worker_queue.lock, "stream_worker_q");

    int pid = sched_create_task(stream_worker_main);
    if (pid < 0) {
        klog(KLOG_FATAL, SUBSYS_CAP,
             "stream_worker_init: sched_create_task failed");
        return;
    }
    klog(KLOG_INFO, SUBSYS_CAP,
         "stream_worker_init: worker task pid=%d", pid);
}
