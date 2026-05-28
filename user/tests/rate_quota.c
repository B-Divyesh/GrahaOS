// user/tests/rate_quota.c
//
// Phase 29 Session I (FU27.X.rate_check_syscall_path) — token-bucket
// syscall rate quota gate test.
//
// The kernel adds a per-task rate quota with sample-gated (1/256) check
// at syscall dispatch entry.  We set a tight limit, blast a large number
// of syscalls, and verify:
//
//   1. syscall_rate_exceeded_count goes from 0 to > 0 in soft mode.
//   2. Soft mode does NOT change return values — calls continue.
//   3. After resetting the limit to 0 (unlimited), the counter no longer
//      grows.
//   4. Hard mode causes a syscall to return -EAGAIN once budget is hit.
//   5. Asserting syscall_rate_exceeded > 0 after a controlled burst.

#include "../libtap.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    // 1. Baseline: counter must be 0 at start (we haven't enabled anything).
    long c0 = syscall_debug_syscall_rate_exceeded();
    TAP_ASSERT(c0 == 0,
               "1. rate-exceeded counter starts at 0");

    // Set a tight soft-mode limit: 1 sampled syscall/sec (so any burst
    // exceeds immediately).  Soft mode: audit-only.
    (void)syscall_debug_syscall_rate_set(1, 0);

    // Blast 20K syscalls to ensure many samples (at 1/256 sampling that's
    // ~80 sample events, well past the limit of 1).
    for (int i = 0; i < 20000; i++) {
        syscall_putc('.');
        if ((i & 0xFFF) == 0xFFF) syscall_putc('\n');
    }
    syscall_putc('\n');

    long c1 = syscall_debug_syscall_rate_exceeded();
    if (c1 == 0) {
        printf("# rate_quota: 0 exceeds after 20K syscalls — check 1/256 sampling\n");
    }
    TAP_ASSERT(c1 > 0,
               "2. soft-mode counter increments past limit on heavy burst");

    // 3. Disable limit.  Repeat the burst.  Counter must NOT advance further.
    (void)syscall_debug_syscall_rate_set(0, 0);
    for (int i = 0; i < 4096; i++) syscall_putc('.');
    syscall_putc('\n');

    long c2 = syscall_debug_syscall_rate_exceeded();
    TAP_ASSERT(c2 == c1,
               "3. limit=0 disables the check (counter stable)");

    // 4. Hard mode: at least one call should observe -EAGAIN.  Note: the
    // dispatcher hard-mode path returns -EAGAIN INSTEAD of dispatching;
    // syscall_putc returns void in our wrapper so we use a syscall with
    // an observable return value.  Use SYS_GETPID — should always succeed
    // unless rate-limited.
    (void)syscall_debug_syscall_rate_set(1, 1);  // hard mode
    long eagain_seen = 0;
    for (int i = 0; i < 2000; i++) {
        long pid = syscall_getpid();
        if (pid == -11) eagain_seen++;  // -EAGAIN
    }
    (void)syscall_debug_syscall_rate_set(0, 0); // disable
    if (eagain_seen == 0) {
        printf("# rate_quota: 0 -EAGAIN seen after 2000 getpid in hard mode\n");
    }
    TAP_ASSERT(eagain_seen > 0,
               "4. hard mode returns -EAGAIN on overflow");

    // 5. Final: exceeded counter strictly greater than after step 2.
    long c3 = syscall_debug_syscall_rate_exceeded();
    TAP_ASSERT(c3 > c1,
               "5. exceeded counter advanced through the hard-mode burst");

    tap_done();
    syscall_exit(0);
}
