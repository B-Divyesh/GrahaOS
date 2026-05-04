// user/tests/txn_stress_nested.c — Phase 25 Stage G stress.
//
// 100 outer iterations × 4 nested levels × random commit/abort decisions.
// Every iteration ends with the active_txn stack at depth 0. Asserts no
// panics and a final clean begin/commit afterwards.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

#define OUTERS         100
#define NEST_DEPTH     4

static uint32_t lcg(uint32_t *st) {
    *st = (*st) * 1103515245u + 12345u;
    return *st;
}

void _start(void) {
    tap_plan(3);

    uint32_t prng = 0xDEADBEEFu;

    int run_ok = 1;
    int failed_at = -1;
    for (int outer = 0; outer < OUTERS; outer++) {
        long handles[NEST_DEPTH];
        int  pushed = 0;
        for (int d = 0; d < NEST_DEPTH; d++) {
            long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "nstress");
            if (h < 0) { run_ok = 0; failed_at = outer * 100 + d; goto unwind; }
            handles[d] = h;
            pushed++;
        }

        // Unwind: from innermost to outermost, mix abort/commit randomly.
        for (int d = NEST_DEPTH - 1; d >= 0; d--) {
            long rc;
            if ((lcg(&prng) & 1) == 0) {
                rc = syscall_txn_commit((uint32_t)handles[d]);
            } else {
                rc = syscall_txn_abort((uint32_t)handles[d]);
            }
            if (rc != 0) { run_ok = 0; failed_at = -(outer * 100 + d); break; }
        }
        continue;

unwind:
        // Begin partial-fail unwind: abort everything we got.
        for (int d = pushed - 1; d >= 0; d--) {
            (void)syscall_txn_abort((uint32_t)handles[d]);
        }
        break;
    }

    TAP_ASSERT(run_ok, "1. 100 outer × 4 nested cycles all clean");
    (void)failed_at;

    // Active-stack must be empty after the run — verify by doing a final
    // fresh begin/commit that doesn't bump into nesting.
    long h_post = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "post_nested_stress");
    TAP_ASSERT(h_post >= 0, "2. post-stress begin returns a fresh handle");
    long c_post = syscall_txn_commit((uint32_t)h_post);
    TAP_ASSERT(c_post == 0, "3. post-stress commit returns 0");

    tap_done();
    syscall_exit(0);
}
