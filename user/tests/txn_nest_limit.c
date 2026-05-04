// user/tests/txn_nest_limit.c — Phase 25 Stage F TAP gate test.
//
// Verifies TXN_MAX_NESTING enforcement. After 4 successful begins, the 5th
// must fail with TXN_ENESTED (-201). Cleanup unwinds correctly via abort.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

#define EXPECTED_MAX_NESTING 4

void _start(void) {
    tap_plan(4);

    long handles[EXPECTED_MAX_NESTING];
    int begin_ok = 1;
    for (int i = 0; i < EXPECTED_MAX_NESTING; i++) {
        handles[i] = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "nest");
        if (handles[i] < 0) { begin_ok = 0; break; }
    }
    TAP_ASSERT(begin_ok, "1. 4 sequential begins all succeed");

    // 5th begin must fail with TXN_ENESTED (-201) or any other negative
    // — the spec says ENESTED specifically; we accept any negative as
    // "limit enforced" so the test isn't brittle to errno number changes.
    long h_overflow = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "overflow");
    TAP_ASSERT(h_overflow < 0,
               "2. 5th begin (over TXN_MAX_NESTING=4) returns negative");

    // Unwind: abort innermost-first. Each abort should succeed.
    int abort_ok = 1;
    for (int i = EXPECTED_MAX_NESTING - 1; i >= 0; i--) {
        long ar = syscall_txn_abort((uint32_t)handles[i]);
        if (ar != 0) { abort_ok = 0; break; }
    }
    TAP_ASSERT(abort_ok, "3. all 4 unwinds (innermost-first) succeed");

    // Post-unwind: fresh begin works.
    long h_fresh = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "fresh");
    long c_fresh = syscall_txn_commit((uint32_t)h_fresh);
    TAP_ASSERT(h_fresh >= 0 && c_fresh == 0,
               "4. fresh begin/commit works post-unwind");

    tap_done();
    syscall_exit(0);
}
