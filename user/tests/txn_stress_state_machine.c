// user/tests/txn_stress_state_machine.c — Phase 25 Stage G stress.
//
// Plan calls for txn_stress_replay_10k: 1 txn × 1000 buffered + commit. In
// Phase 25 v1 (single-process unit harness), buffered-replay testing
// requires a multi-process external peer — that lands at Stage I via the
// gash + grahai integration tests.
//
// This Stage G slot covers the state-machine + slab churn instead: 1000
// rapid begin/commit cycles with NO chan ops in between, which exercises
// the kmem_cache + cap_object/handle lifecycle + g_txn_live_head linkage
// at higher rate than txn_stress_basic. Combined with txn_stress_basic
// (which adds in-scope channel sends per cycle), we cover both axes of
// the txn substrate's per-iteration cost.
//
// Gate target: 1000 iters in ≤ 1 s wall-clock (Phase 25 v1 reference). If
// the gate timeout is breached, it's a real regression.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

#define ITERATIONS_GATE   1000

void _start(void) {
    tap_plan(3);

    int run_ok = 1;
    int failed_at = -1;
    for (int i = 0; i < ITERATIONS_GATE; i++) {
        long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "smach");
        if (h < 0) { run_ok = 0; failed_at = i; break; }
        long c = syscall_txn_commit((uint32_t)h);
        if (c != 0) { run_ok = 0; failed_at = -i; break; }
    }
    TAP_ASSERT(run_ok, "1. 1000 begin/commit state-machine cycles clean");
    (void)failed_at;

    // Mix abort: 100 begin + abort cycles.
    int abort_ok = 1;
    for (int i = 0; i < 100; i++) {
        long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "smach_abort");
        if (h < 0) { abort_ok = 0; break; }
        long ar = syscall_txn_abort((uint32_t)h);
        if (ar != 0) { abort_ok = 0; break; }
    }
    TAP_ASSERT(abort_ok, "2. 100 begin/abort cycles clean");

    // Mixed: alternating commit/abort.
    int mix_ok = 1;
    for (int i = 0; i < 100; i++) {
        long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "mix");
        if (h < 0) { mix_ok = 0; break; }
        long rc = (i & 1) ? syscall_txn_abort((uint32_t)h)
                          : syscall_txn_commit((uint32_t)h);
        if (rc != 0) { mix_ok = 0; break; }
    }
    TAP_ASSERT(mix_ok, "3. 100 alternating commit/abort cycles clean");

    tap_done();
    syscall_exit(0);
}
