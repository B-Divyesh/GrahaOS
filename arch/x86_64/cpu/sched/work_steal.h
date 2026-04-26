// arch/x86_64/cpu/sched/work_steal.h
//
// Phase 20 — work-stealing algorithm for per-CPU runqueues.
//
// When a CPU's schedule() finds its own runq empty, it invokes
// sched_steal_from_busiest(&own_rq) to attempt to splice half of the
// busiest peer runq's tail onto its own head. The algorithm is:
//
//   1. Scan every other per-CPU runq and snapshot-read ready_count WITHOUT
//      taking the peer's lock. This is deliberately racy; the trylock in
//      step 3 will reconfirm state before any mutation.
//
//   2. Sort candidates by ready_count descending, filter out peers with
//      count ≤ 1 (stealing from a runq with only its current task would
//      just migrate it).
//
//   3. For each of the top 4 candidates: trylock victim. On success,
//      compute n = floor(ready_count / 2), splice n from the victim's
//      ready_tail onto the thief's ready_head (tail-steal preserves
//      victim's cache warmth). Release victim's lock. Return n.
//
//   4. On all-trylock-fail: increment steal_failures, return 0. Caller
//      falls through to its per-CPU idle task.
//
// Lock hierarchy: the caller holds NO lock on entry. We trylock victim
// atomically; the thief's own runq lock is held by the caller (schedule()
// currently holds it during the empty-queue check). That's safe because
// we never call runq_enqueue_ready on the thief here — we splice the
// stolen chain onto the thief's ready_head directly, avoiding a lock
// round-trip.
//
// Bounded to 4 trylock attempts per call to prevent livelock (spec risk
// #1). If every peer is busy, we accept idle time over spinning.
#pragma once

#include <stdint.h>
#include "runq.h"

#define WORK_STEAL_MAX_ATTEMPTS 4u

// Scratch state for a stealing operation. Stack-allocated by
// sched_steal_from_busiest; the prototype is public only so unit tests can
// exercise stealing without a real schedule() around it.
typedef struct work_steal_ctx {
    uint32_t thief_cpu;
    uint32_t victim_cpu;
    uint32_t steal_count;
    struct task_struct *stolen_head;
    struct task_struct *stolen_tail;
} work_steal_ctx_t;

// ---------------------------------------------------------------------------
// sched_steal_from_busiest — try to move work onto `thief` from the
// busiest peer. Returns the count of tasks stolen (0 means no work found
// or all trylock attempts failed). `thief->ready_head` is populated
// before return; caller can subsequently runq_dequeue_ready.
//
// PRECONDITION: the caller holds `thief->lock` on entry. This is consistent
// with how schedule() calls it right after observing an empty local runq.
// ---------------------------------------------------------------------------
uint32_t sched_steal_from_busiest(runq_t *thief);
