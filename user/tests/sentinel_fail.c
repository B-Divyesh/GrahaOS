// user/tests/sentinel_fail.c
// Phase 12: deliberately-failing TAP test used ONLY by
// `make test-sentinel-meta` to prove the harness catches failures.
// Not linked into the default test build (see WITH_SENTINEL_FAIL flag
// in user/Makefile and top-level Makefile).

#include "../libtap.h"
#include <stdlib.h>

void _start(void) {
    tap_plan(1);
    tap_not_ok("expected failure (meta-test of harness)",
               "this test exists solely to exercise the failure path");
    tap_done();
    exit(0);
}
