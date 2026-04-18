// user/tests/libtap_selftest.c
// Phase 12 — smoke test for libtap's passing-path grammar.
//
// Exercises tap_plan / tap_ok / tap_skip / TAP_ASSERT. All assertions
// are designed to pass. A separate test (sentinel_fail.tap, unit 15)
// exercises tap_not_ok. A deeper gate test (unit 14) pipes libtap's
// own output back via SYS_PIPE+SYS_DUP2 to validate the full grammar.

#include "../libtap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void _start(void) {
    tap_plan(5);

    tap_ok("trivial pass");

    TAP_ASSERT(1 + 1 == 2, "arithmetic works");

    TAP_ASSERT(strcmp("grahaos", "grahaos") == 0, "strcmp equal strings");

    tap_skip("nonexistent feature", "not implemented in Phase 12");

    // Counters reflect what we did: plan=5, passed=4 so far (ok 1, ok 2,
    // ok 3, skip 4 which counts as passed). tap_get_* refreshes after
    // each call — we check BEFORE the 5th assertion lands.
    int planned = tap_get_planned();
    int passed  = tap_get_passed();
    int failed  = tap_get_failed();
    TAP_ASSERT(planned == 5 && passed == 4 && failed == 0,
               "internal counters consistent");

    tap_done();
    exit(0);
}
