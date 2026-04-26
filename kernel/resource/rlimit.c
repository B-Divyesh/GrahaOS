// kernel/resource/rlimit.c
//
// Phase 20 — resource-limit defaults + accounting counters.
//
// U6 landed the task_t fields + init helper. U10 wired mem quota into the
// brk / vmo hot paths. U11 (this file) adds the full CPU-budget + epoch
// machinery. U12 adds the IO token-bucket.
//
// CPU budget model:
//   - Every user task has cpu_time_slice_budget_ns (default 1 s). On every
//     dispatch schedule() stamps last_ran_tsc. On every preemption
//     schedule() computes elapsed_ns = tsc_to_ns(rdtsc() - last_ran_tsc)
//     and calls rlimit_consume_cpu(task, elapsed_ns). Return ≤ 0 means
//     the budget is exhausted for this epoch; schedule() then parks the
//     task on its runq's starved_head instead of ready_tail.
//   - sched_epoch_task fires at 1 Hz (RLIMIT_EPOCH_TICKS of the 100 Hz
//     scheduler tick). It calls rlimit_epoch_tick which enumerates every
//     live task (pid_hash), re-slams cpu_budget_remaining_ns = cpu_time_slice_budget_ns,
//     then sweeps every per-CPU runq's starved_head back to ready_tail.
//     One AUDIT_SCHED_EPOCH is emitted per call with tasks_refilled and
//     starved_drained totals.
//
// IO token-bucket:
//   - Every user task has io_rate_bytes_per_sec (default 0 = unlimited).
//     io_tokens holds the current bucket balance. Bucket capacity == rate
//     (one-second burst).
//   - rlimit_check_io(task, bytes): if rate==0 allow, else if tokens>=bytes
//     subtract and allow, else audit + RLIMIT_EAGAIN_INTERNAL → caller
//     (submit.c) chains the SQE on task.io_pending_head.
//   - rlimit_refill_io_tokens(task): per-tick refill of rate/100 (10 ms
//     tick → 1% of burst). After top-up, drain io_pending_head while the
//     next job's len is covered.
#include "rlimit.h"

#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../audit.h"
#include "../io/stream.h"
#include "../pid_hash.h"
#include "../log.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/sched/runq.h"

// g_timer_ticks advances 100x/s inside the LAPIC timer handler. Epoch
// cadence is RLIMIT_EPOCH_TICKS (== 100) ticks per fire.
extern volatile uint64_t g_timer_ticks;

// Live task count. Incremented by sched_create_task / sched_create_user_process
// after successful slab_alloc + pid_hash_insert; decremented by sched_reap_zombie
// after pid_hash_remove. Read by psinfo and by the spawn path's soft-cap check.
uint64_t g_task_count = 0;

// ---------------------------------------------------------------------------
// rlimit_init_defaults — populates the Phase 20 accounting fields on a
// freshly-allocated task_t. Two modes:
//   parent != NULL: inherit every limit from the parent. This is the common
//                   case (sys_spawn without attrs override).
//   parent == NULL: apply system defaults. Used by sched_init for the BSP
//                   idle task and by kernel-internal workers (flusher,
//                   epoch, recluster). Kernel workers intentionally get
//                   UNLIMITED (0) everywhere — they are trusted.
// ---------------------------------------------------------------------------
void rlimit_init_defaults(task_t *child, const task_t *parent) {
    if (!child) return;

    if (parent) {
        child->mem_limit_pages           = parent->mem_limit_pages;
        child->mem_pages_used            = 0;
        child->cpu_time_slice_budget_ns  = parent->cpu_time_slice_budget_ns;
        child->cpu_budget_remaining_ns   = (int64_t)parent->cpu_time_slice_budget_ns;
        child->io_rate_bytes_per_sec     = parent->io_rate_bytes_per_sec;
        child->io_tokens                 = (int64_t)parent->io_rate_bytes_per_sec;
        child->io_pending_head           = NULL;
    } else {
        // Kernel-internal tasks: unlimited.
        child->mem_limit_pages           = 0;
        child->mem_pages_used            = 0;
        child->cpu_time_slice_budget_ns  = 0;
        child->cpu_budget_remaining_ns   = 0;
        child->io_rate_bytes_per_sec     = 0;
        child->io_tokens                 = 0;
        child->io_pending_head           = NULL;
    }

    // CPU affinity: -1 means "any CPU". Kernel idle tasks override this
    // after rlimit_init_defaults to pin themselves to their CPU id.
    child->cpu_pinned = -1;
    child->last_ran_cpu = 0xFFFFFFFFu;
    child->last_ran_tsc = 0;
    child->last_starvation_epoch = 0;

    // Runq and hash linkage — NULL until the task is inserted.
    child->runq_next   = NULL;
    child->runq_prev   = NULL;
    child->hash_next   = NULL;
    child->global_next = NULL;
    child->global_prev = NULL;
}

// ---------------------------------------------------------------------------
// U10: real memory quota enforcement. Returns 0 on accept (and increments
// mem_pages_used), -ENOMEM (-12) on limit exceeded (with audit entry).
// ---------------------------------------------------------------------------
int rlimit_check_mem(task_t *t, uint64_t npages) {
    if (!t) return 0;
    // Unlimited tasks (kernel threads, default user) bypass the check.
    if (t->mem_limit_pages == 0) return 0;

    uint64_t used = t->mem_pages_used;
    if (used + npages > t->mem_limit_pages) {
        audit_write_rlimit_mem((int32_t)t->id, t->mem_limit_pages,
                               used + npages);
        return -12;  // -ENOMEM
    }
    t->mem_pages_used = used + npages;
    return 0;
}

void rlimit_account_free_mem(task_t *t, uint64_t npages) {
    if (!t) return;
    if (t->mem_pages_used >= npages) {
        t->mem_pages_used -= npages;
    } else {
        // Clamp at zero rather than underflow. If we see negative accounting
        // it means some free path ran without a matching check (usually
        // benign — kernel-internal paths don't count). Audit would be noisy
        // here, so we silently clamp.
        t->mem_pages_used = 0;
    }
}

// ---------------------------------------------------------------------------
// U11: real CPU budget consumption. Subtracts `ns` from cpu_budget_remaining_ns.
// Returns the new balance. Unlimited tasks (budget == 0) return a large
// positive sentinel so callers don't special-case them.
// ---------------------------------------------------------------------------
int64_t rlimit_consume_cpu(task_t *t, uint64_t ns) {
    if (!t) return 1;
    if (t->cpu_time_slice_budget_ns == 0) return 1;   // unlimited
    // Guard against crazy deltas (e.g. TSC not ready on first dispatch) —
    // if elapsed exceeds a whole second, clamp to the full budget so we
    // debit at most one epoch's worth in a single consume call.
    uint64_t clamped = (ns > RLIMIT_EPOCH_NS) ? RLIMIT_EPOCH_NS : ns;
    t->cpu_budget_remaining_ns -= (int64_t)clamped;
    return t->cpu_budget_remaining_ns;
}

// ---------------------------------------------------------------------------
// U12: real I/O token-bucket enforcement. check-then-consume semantics so
// the bucket never goes negative — insufficient tokens is a clean defer,
// not a silent over-spend.
// ---------------------------------------------------------------------------
int rlimit_check_io(task_t *t, uint64_t bytes) {
    if (!t) return 0;
    if (t->io_rate_bytes_per_sec == 0) return 0;  // unlimited
    if (t->io_tokens >= (int64_t)bytes) {
        t->io_tokens -= (int64_t)bytes;
        return 0;
    }
    audit_write_rlimit_io((int32_t)t->id, t->io_rate_bytes_per_sec, bytes);
    return RLIMIT_EAGAIN_INTERNAL;
}

// ---------------------------------------------------------------------------
// U12: per-tick token refill + pending-queue drain. Called from schedule()
// for the currently-running task. The drain is bounded by the caller's
// tick slice (cheap: one 64-bit compare per pending job).
// ---------------------------------------------------------------------------
void rlimit_refill_io_tokens(task_t *t) {
    if (!t) return;
    if (t->io_rate_bytes_per_sec == 0) return;  // unlimited — no bucket

    // Refill rate/100 per tick (10 ms), capped at one-second burst.
    int64_t refill = (int64_t)(t->io_rate_bytes_per_sec / RLIMIT_IO_REFILL_DIVISOR);
    if (refill == 0 && t->io_rate_bytes_per_sec > 0) {
        refill = 1;  // rate < 100 still gets >0 tokens per tick eventually.
    }
    t->io_tokens += refill;
    if (t->io_tokens > (int64_t)t->io_rate_bytes_per_sec) {
        t->io_tokens = (int64_t)t->io_rate_bytes_per_sec;
    }

    // Drain io_pending_head while tokens cover the head job's len. Each
    // job is threaded via stream_job_t.worker_next — we re-use that slot
    // because a pending job is by definition NOT on the worker queue.
    // When the job finally ships, stream_worker_enqueue resets worker_next.
    while (t->io_pending_head) {
        stream_job_t *job = (stream_job_t *)t->io_pending_head;
        uint64_t len = job->sqe_copy.len;
        if (t->io_tokens < (int64_t)len) break;   // next job still can't fit
        t->io_tokens -= (int64_t)len;
        t->io_pending_head = (void *)job->worker_next;
        job->worker_next = NULL;
        stream_worker_enqueue(job);
    }
}

// ---------------------------------------------------------------------------
// U11: Epoch tick. Runs on sched_epoch_task (pinned CPU 0) once per second.
// Two phases:
//   1. Walk the global task list via pid_hash_enumerate; for every task,
//      slam cpu_budget_remaining_ns = cpu_time_slice_budget_ns. Unlimited
//      tasks (budget==0) are untouched.
//   2. For each per-CPU runq in cpu_id order, lock it, drain starved_head
//      → ready_tail via runq_refill_starved, unlock.
// Emits a single AUDIT_SCHED_EPOCH with cumulative counts.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t refilled;
} epoch_refill_ctx_t;

static void epoch_refill_one(task_t *t, void *ctxp) {
    if (!t) return;
    epoch_refill_ctx_t *ctx = (epoch_refill_ctx_t *)ctxp;
    if (t->cpu_time_slice_budget_ns != 0) {
        t->cpu_budget_remaining_ns = (int64_t)t->cpu_time_slice_budget_ns;
        ctx->refilled++;
    }
}

void rlimit_epoch_tick(void) {
    epoch_refill_ctx_t ctx = { .refilled = 0 };
    pid_hash_enumerate(epoch_refill_one, &ctx);

    // Phase 2: drain starved lists per CPU.
    uint32_t drained_total = 0;
    for (uint32_t cpu = 0; cpu < g_cpu_count; cpu++) {
        runq_t *rq = &g_cpu_locals[cpu].runq;
        spinlock_acquire(&rq->lock);
        drained_total += runq_refill_starved(rq);
        rq->last_epoch_tick_ticks = g_timer_ticks;
        spinlock_release(&rq->lock);
    }

    audit_write_sched_epoch(ctx.refilled, drained_total);
}

int rlimit_set(task_t *target, uint32_t resource, uint64_t value) {
    if (!target) return -3;  // -ESRCH
    switch (resource) {
    case RLIMIT_RES_MEM:
        target->mem_limit_pages = value;
        return 0;
    case RLIMIT_RES_CPU:
        if (value > RLIMIT_EPOCH_NS) return -22;  // -EINVAL
        target->cpu_time_slice_budget_ns = value;
        target->cpu_budget_remaining_ns = (int64_t)value;
        return 0;
    case RLIMIT_RES_IO:
        target->io_rate_bytes_per_sec = value;
        target->io_tokens = (int64_t)value;  // prime the bucket at full
        return 0;
    default:
        return -22;  // -EINVAL
    }
}

int rlimit_get(const task_t *target, uint32_t resource, uint64_t *out) {
    if (!target || !out) return -3;  // -ESRCH / -EFAULT
    switch (resource) {
    case RLIMIT_RES_MEM: *out = target->mem_limit_pages; return 0;
    case RLIMIT_RES_CPU: *out = target->cpu_time_slice_budget_ns; return 0;
    case RLIMIT_RES_IO:  *out = target->io_rate_bytes_per_sec; return 0;
    default:             return -22;
    }
}
