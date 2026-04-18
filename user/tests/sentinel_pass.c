// user/tests/sentinel_pass.c
// Phase 12: trivial always-passing TAP test. Proves the harness
// plumbing works end-to-end. Referenced by manual_verification step 1
// of specs/phase-12-test-harness.yml.

#include "../libtap.h"
#include <stdlib.h>

void _start(void) {
    tap_plan(2);
    tap_ok("sentinel lives");
    tap_ok("exit path works");
    tap_done();
    exit(0);
}
