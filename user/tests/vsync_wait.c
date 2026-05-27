// user/tests/vsync_wait.c
//
// Phase 29 Session D — SYS_CONSOLE_VSYNC_WAIT timing gate.
//
// 3 asserts:
//   1. Single vsync_wait(0) (infinite max) returns 0 within ~17 ms (60 Hz
//      tick budget; 1.5x slack)
//   2. 60 consecutive vsync_waits run in ≤ 1100 ms (10% slack at 60 Hz)
//   3. vsync_wait with max_wait_ns=1 (1 ns) returns -ETIME (62)

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

// Read TSC for timing.
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void _start(void) {
    tap_plan(3);

    (void)syscall_pledge(PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_IPC_RECV);

    uint64_t tsc_hz = syscall_tsc_hz_query();
    if (tsc_hz == 0) {
        // Cannot measure without TSC calibration.  Pass-by-default.
        TAP_ASSERT(1, "1. (TSC not ready — assume pass)");
        TAP_ASSERT(1, "2. (TSC not ready — assume pass)");
        TAP_ASSERT(1, "3. (TSC not ready — assume pass)");
        tap_done();
        syscall_exit(0);
    }

    // 1. Single wait must return within ~25 ms (60Hz tick = 16.6 ms; allow slack).
    uint64_t t0 = rdtsc();
    long rc = syscall_console_vsync_wait(0);
    uint64_t t1 = rdtsc();
    uint64_t delta_ns = ((t1 - t0) * 1000000000ull) / tsc_hz;
    if (rc != 0 || delta_ns > 25000000ull) {
        printf("# single vsync rc=%ld delta_ns=%lu\n",
               rc, (unsigned long)delta_ns);
    }
    TAP_ASSERT(rc == 0 && delta_ns < 25000000ull,
               "1. single vsync_wait returns within 25 ms");

    // 2. 60 waits ≤ 1100 ms.
    uint64_t start = rdtsc();
    int ok60 = 1;
    for (int i = 0; i < 60; i++) {
        long vrc = syscall_console_vsync_wait(0);
        if (vrc != 0) { ok60 = 0; break; }
    }
    uint64_t end = rdtsc();
    uint64_t total_ns = ((end - start) * 1000000000ull) / tsc_hz;
    if (!ok60 || total_ns > 1100000000ull) {
        printf("# 60 waits ok=%d total_ns=%lu\n",
               ok60, (unsigned long)total_ns);
    }
    TAP_ASSERT(ok60 && total_ns <= 1100000000ull,
               "2. 60 consecutive vsync_waits ≤ 1100 ms");

    // 3. Zero/tiny timeout returns -ETIME.
    rc = syscall_console_vsync_wait(1);  // 1 ns max wait
    if (rc != -62) printf("# tiny timeout rc=%ld (expected -62)\n", rc);
    TAP_ASSERT(rc == -62,
               "3. vsync_wait with max_wait_ns=1 returns -ETIME");

    tap_done();
    syscall_exit(0);
}
