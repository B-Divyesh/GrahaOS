// user/tests/manifest_export.c
//
// Phase 27 Block C — Stage C2 SYS_MANIFEST_EXPORT gate.
//
// Verifies:
//   1. SYS_MANIFEST_EXPORT returns positive byte count when given a valid
//      buffer.
//   2. The generation hash is non-zero (FNV-1a of the JSON bytes).
//   3. The first byte of the blob is '{' (JSON object start), confirming
//      we got the gcp.json content rather than zeroed garbage.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static uint8_t s_buf[16384] = {0};

void _start(void) {
    tap_plan(3);

    uint64_t generation = 0;
    long copied = syscall_manifest_export(s_buf, sizeof(s_buf), &generation);
    if (copied <= 0) printf("# manifest_export rc=%ld\n", copied);
    TAP_ASSERT(copied > 0, "1. manifest_export returns positive byte count");

    if (generation == 0) printf("# generation hash zero (expected non-zero)\n");
    TAP_ASSERT(generation != 0,
               "2. manifest generation hash is non-zero");

    if (copied > 0 && s_buf[0] != '{') {
        printf("# blob[0] = 0x%02x (expected '{' = 0x7b)\n",
               (unsigned)s_buf[0]);
    }
    TAP_ASSERT(copied > 0 && s_buf[0] == '{',
               "3. blob starts with JSON object marker '{'");

    tap_done();
    syscall_exit(0);
}
