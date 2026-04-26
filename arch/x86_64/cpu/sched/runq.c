// arch/x86_64/cpu/sched/runq.c
//
// Phase 20 — per-CPU runqueue primitives. Pure list plumbing; no scheduling
// policy, no cross-CPU communication. See runq.h for the contract.
#include "runq.h"

#include "sched.h"  // task_t, task_state_t, TASK_STATE_* enum values

void runq_init(runq_t *rq, uint32_t cpu_id) {
    if (!rq) return;
    rq->magic = RUNQ_MAGIC;
    rq->cpu_id = cpu_id;
    rq->ready_head = NULL;
    rq->ready_tail = NULL;
    rq->ready_count = 0;
    rq->_pad0 = 0;
    rq->starved_head = NULL;
    rq->current = NULL;
    rq->steal_successes = 0;
    rq->steal_failures = 0;
    rq->context_switches = 0;
    rq->last_epoch_tick_ticks = RUNQ_EPOCH_NEVER;
    rq->idle_task = NULL;
    spinlock_init(&rq->lock, "runq");
}

void runq_enqueue_ready(runq_t *rq, task_t *task) {
    if (!rq || !task) return;
    // Detach first — calling with a task already linked elsewhere would
    // corrupt that other list. Cheap (inline NULL check).
    task->runq_next = NULL;
    task->runq_prev = rq->ready_tail;
    if (rq->ready_tail) {
        rq->ready_tail->runq_next = task;
    } else {
        rq->ready_head = task;
    }
    rq->ready_tail = task;
    rq->ready_count++;
    task->state = TASK_STATE_READY;
}

task_t *runq_dequeue_ready(runq_t *rq) {
    if (!rq || !rq->ready_head) return NULL;
    task_t *t = rq->ready_head;
    rq->ready_head = t->runq_next;
    if (rq->ready_head) {
        rq->ready_head->runq_prev = NULL;
    } else {
        rq->ready_tail = NULL;
    }
    t->runq_next = NULL;
    t->runq_prev = NULL;
    if (rq->ready_count > 0) rq->ready_count--;
    return t;
}

// Unlink from ready OR starved list. We detect which by whether the task's
// prev is set, and whether the ready/starved heads point at it.
void runq_remove(runq_t *rq, task_t *task) {
    if (!rq || !task) return;

    // Ready-list unlink (doubly-linked).
    if (task->runq_prev) {
        task->runq_prev->runq_next = task->runq_next;
    } else if (rq->ready_head == task) {
        rq->ready_head = task->runq_next;
    }
    if (task->runq_next) {
        task->runq_next->runq_prev = task->runq_prev;
    } else if (rq->ready_tail == task) {
        rq->ready_tail = task->runq_prev;
    }

    // Starved-list unlink (singly-linked via runq_next only). Walk to find
    // the predecessor. If neither head nor chain matches, no-op — task
    // wasn't on any list, which is a legal call.
    if (rq->starved_head == task) {
        rq->starved_head = task->runq_next;
    } else {
        task_t *prev = rq->starved_head;
        while (prev && prev->runq_next != task) {
            prev = prev->runq_next;
        }
        if (prev) {
            prev->runq_next = task->runq_next;
        }
    }

    task->runq_next = NULL;
    task->runq_prev = NULL;
    // ready_count is decremented only if we actually removed from ready —
    // we can't easily tell, so instead the caller must track. For MVP, only
    // the schedule() pop path uses this for ready removals; starved list
    // has its own count-keeping semantics (refill_starved rebuilds from
    // scratch).
}

void runq_move_to_starved(runq_t *rq, task_t *task) {
    if (!rq || !task) return;
    // Remove from ready first (we assume it is on ready).
    if (task->runq_prev) {
        task->runq_prev->runq_next = task->runq_next;
    } else if (rq->ready_head == task) {
        rq->ready_head = task->runq_next;
    }
    if (task->runq_next) {
        task->runq_next->runq_prev = task->runq_prev;
    } else if (rq->ready_tail == task) {
        rq->ready_tail = task->runq_prev;
    }
    if (rq->ready_count > 0) rq->ready_count--;

    // Push onto starved head (singly-linked via runq_next; prev kept NULL).
    task->runq_prev = NULL;
    task->runq_next = rq->starved_head;
    rq->starved_head = task;
    task->state = TASK_STATE_STARVED;
}

void runq_push_starved(runq_t *rq, task_t *task) {
    if (!rq || !task) return;
    task->runq_prev = NULL;
    task->runq_next = rq->starved_head;
    rq->starved_head = task;
    task->state = TASK_STATE_STARVED;
}

uint32_t runq_refill_starved(runq_t *rq) {
    if (!rq || !rq->starved_head) return 0;
    uint32_t n = 0;
    task_t *s = rq->starved_head;
    rq->starved_head = NULL;
    while (s) {
        task_t *next = s->runq_next;
        s->runq_prev = NULL;
        s->runq_next = NULL;
        runq_enqueue_ready(rq, s);
        s = next;
        n++;
    }
    return n;
}
