// user/tests/txn_basic_commit.c — Phase 25 Stage F TAP gate test.
//
// Verifies the empty-buffer commit fast path is bullet-proof under repeated
// begin / commit cycles. This is the substrate test for txn_commit's
// state-machine: every begin must return a fresh handle, every commit on
// an unmutated SCOPE_SELF txn must return 0, and the cap_handles + slab
// caches must never leak (asserted indirectly via 64-iter cycle).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

void _start(void) {
    tap_plan(5);

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "stage_f_commit_1");
    TAP_ASSERT(h >= 0, "1. txn_begin returns valid handle");

    long c = syscall_txn_commit((uint32_t)h);
    TAP_ASSERT(c == 0, "2. txn_commit (empty buffer) returns 0");

    // Re-commit the same handle: should fail because it's torn down now.
    long c2 = syscall_txn_commit((uint32_t)h);
    TAP_ASSERT(c2 < 0, "3. re-commit of stale handle returns negative");

    // 64 begin/commit cycles to exercise slab + handle-table reuse.
    int cycle_ok = 1;
    for (int i = 0; i < 64; i++) {
        long hi = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "stage_f_cycle");
        if (hi < 0) { cycle_ok = 0; break; }
        long ci = syscall_txn_commit((uint32_t)hi);
        if (ci != 0) { cycle_ok = 0; break; }
    }
    TAP_ASSERT(cycle_ok, "4. 64 begin/commit cycles all succeed");

    // After cycle: ensure a fresh begin still works.
    long h_final = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "stage_f_final");
    long c_final = syscall_txn_commit((uint32_t)h_final);
    TAP_ASSERT(h_final >= 0 && c_final == 0,
               "5. fresh begin/commit succeeds post 64-cycle stress");

    tap_done();
    syscall_exit(0);
}
