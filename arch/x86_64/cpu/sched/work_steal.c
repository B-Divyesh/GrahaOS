// arch/x86_64/cpu/sched/work_steal.c
//
// Phase 20 — work-stealing implementation.
//
// See work_steal.h for the contract. The algorithm is a bounded-attempt
// trylock scan of peer runqueues, splicing half of the busiest peer's
// ready_tail onto the thief's ready_head. Tail-steal approximates cache
// affinity: the victim's HEAD holds tasks it most recently dispatched
// (cache-hot on the victim); we leave those and steal from the TAIL
// (coldest — least recent dispatch).
#include "work_steal.h"

#include "sched.h"  // task_t + task_state_t
#include "../smp.h"
#include "../../../../kernel/percpu.h"

// Trylock. Non-blocking test_and_set. Returns true on acquire, false if
// the lock was already held. Does NOT disable interrupts — this is a
// cooperative work-steal check; the caller of sched_steal_from_busiest
// already has IRQs masked via spinlock_acquire on its own runq lock, and
// we release the victim promptly.
static inline bool runq_trylock(runq_t *rq) {
    return !__atomic_test_and_set(&rq->lock.locked, __ATOMIC_ACQUIRE);
}

static inline void runq_trylock_release(runq_t *rq) {
    __atomic_clear(&rq->lock.locked, __ATOMIC_RELEASE);
}

// Pick the busiest peer. Snapshot reads of ready_count without a lock — OK
// because we'll trylock before actually manipulating it. Returns the
// victim CPU id, or UINT32_MAX if no candidate has > 1 task.
static uint32_t pick_busiest(uint32_t thief_cpu) {
    uint32_t best_cpu = (uint32_t)-1;
    uint32_t best_count = 1;   // strictly greater than 1 required

    uint32_t n_cpus = g_cpu_count;
    for (uint32_t i = 0; i < n_cpus; i++) {
        if (i == thief_cpu) continue;
        uint32_t c = g_cpu_locals[i].runq.ready_count;
        if (c > best_count) {
            best_count = c;
            best_cpu = i;
        }
    }
    return best_cpu;
}

// Steal individual tasks from victim->ready_tail into thief, respecting
// task->cpu_pinned. Caller holds BOTH runq locks. Tasks pinned to the
// victim or to a third CPU are skipped (pinned to thief_cpu would also be
// stealable, but in practice never happens because such tasks would have
// been routed to thief_cpu's runq directly by sched_enqueue_ready). For
// each stealable task we unlink from victim and prepend onto thief.
//
// Returns the count actually stolen (≤ requested n).
static uint32_t splice_tail_to_thief(runq_t *victim, runq_t *thief, uint32_t n) {
    if (n == 0 || victim->ready_count == 0) return 0;

    int32_t thief_cpu = (int32_t)thief->cpu_id;
    uint32_t stolen = 0;
    task_t *cursor = victim->ready_tail;

    while (cursor && stolen < n) {
        task_t *prev = cursor->runq_prev;

        // Skip tasks pinned to a CPU other than the thief. Kernel threads
        // (audit flusher, mongoose, fs indexer, recluster, stream worker,
        // sched_epoch_task) are pinned to CPU 0; they MUST NOT migrate or
        // they'll read percpu state via the wrong GS base. User tasks
        // default to cpu_pinned == -1 and are always stealable.
        bool stealable = (cursor->cpu_pinned < 0 ||
                          cursor->cpu_pinned == thief_cpu);
        if (!stealable) {
            cursor = prev;
            continue;
        }

        // Unlink cursor from victim's ready list.
        task_t *next = cursor->runq_next;
        if (cursor->runq_prev) {
            cursor->runq_prev->runq_next = next;
        } else {
            victim->ready_head = next;
        }
        if (next) {
            next->runq_prev = cursor->runq_prev;
        } else {
            victim->ready_tail = cursor->runq_prev;
        }
        if (victim->ready_count > 0) victim->ready_count--;

        // Prepend onto thief's ready_head. Stolen tasks become "next up".
        cursor->runq_prev = NULL;
        cursor->runq_next = thief->ready_head;
        if (thief->ready_head) {
            thief->ready_head->runq_prev = cursor;
        } else {
            thief->ready_tail = cursor;
        }
        thief->ready_head = cursor;
        thief->ready_count++;
        stolen++;

        cursor = prev;
    }

    return stolen;
}

uint32_t sched_steal_from_busiest(runq_t *thief) {
    if (!thief) return 0;
    uint32_t thief_cpu = thief->cpu_id;

    for (uint32_t attempt = 0; attempt < WORK_STEAL_MAX_ATTEMPTS; attempt++) {
        uint32_t victim_cpu = pick_busiest(thief_cpu);
        if (victim_cpu == (uint32_t)-1) return 0;

        runq_t *victim = &g_cpu_locals[victim_cpu].runq;

        if (!runq_trylock(victim)) {
            thief->steal_failures++;
            // On next iteration pick_busiest may return a different victim
            // (snapshot reads are racy).
            continue;
        }

        // Recheck under victim's lock.
        if (victim->ready_count <= 1) {
            runq_trylock_release(victim);
            // Pick_busiest may still return this CPU due to stale snapshot;
            // bail out of this attempt and try a fresh scan.
            continue;
        }

        uint32_t n = victim->ready_count / 2;
        if (n == 0) n = 1;   // always steal at least one if possible

        uint32_t stolen = splice_tail_to_thief(victim, thief, n);
        runq_trylock_release(victim);

        if (stolen > 0) {
            thief->steal_successes++;
            return stolen;
        }
        thief->steal_failures++;
    }

    return 0;
}
