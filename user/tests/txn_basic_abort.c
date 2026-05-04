// user/tests/txn_basic_abort.c — Phase 25 Stage F TAP gate test.
//
// Exercises the user-page revert path through txn_abort. The transaction
// is SCOPE_SELF, so snap_capture + snap_restore_internal cover the
// caller's BSS + heap pages. Mutations made between txn_begin and
// txn_abort MUST revert; the txn_abort path must clean up the cap_object
// and handle-table without leaks.
//
// This piggybacks on Phase 25 Stage C's snap_restore_self verification:
// txn_abort calls snap_restore_internal under the hood, so if Stage C is
// passing, abort revert should work identically. The added value here is
// proving the txn_begin+txn_abort syscall path doesn't introduce its own
// regressions.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

// BSS-resident marker: should revert from 0xBBBBBBBB → 0xAAAAAAAA on abort.
static volatile uint32_t g_marker = 0xAAAAAAAA;

void _start(void) {
    tap_plan(6);

    // Pre-condition: marker holds the captured value.
    g_marker = 0xAAAAAAAA;
    TAP_ASSERT(g_marker == 0xAAAAAAAA,
               "1. pre-txn marker is captured value");

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "stage_f_abort_1");
    TAP_ASSERT(h >= 0, "2. txn_begin returns valid handle");

    // Mutate the marker — this is what abort must revert.
    g_marker = 0xBBBBBBBB;
    TAP_ASSERT(g_marker == 0xBBBBBBBB,
               "3. mid-txn mutation observed");

    long ar = syscall_txn_abort((uint32_t)h);
    TAP_ASSERT(ar == 0, "4. txn_abort returns 0");

    TAP_ASSERT(g_marker == 0xAAAAAAAA,
               "5. post-abort marker restored to captured value");

    // After abort, follow-up begin/commit must still work.
    long h2 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "stage_f_abort_2");
    long c2 = syscall_txn_commit((uint32_t)h2);
    TAP_ASSERT(h2 >= 0 && c2 == 0,
               "6. follow-up begin/commit works post-abort");

    tap_done();
    syscall_exit(0);
}
