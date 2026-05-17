// user/tests/inject_spinlock_timeout.c
//
// Phase 28 Session G.1 — fault injection harness.  The spinlock hook
// is OBSERVE-ONLY by design (panic-on-sample would brick the kernel's
// own syscall return path).  We confirm the hook is reachable by
// arming it, driving heavy spinlock traffic via syscall_putc, then
// reading g_debug_spinlock_injection_hits back via DEBUG_INJECT_RESET_ALL.

#include "../libtap.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(3);

    // Reset and capture the baseline.  The return value is the prior
    // hits count; on a fresh boot it is 0.
    long base_hits = syscall_debug_inject_reset_all();
    TAP_ASSERT(base_hits >= 0, "1. reset_all returns prior hits count");

    // Arm the spinlock injection observer.
    syscall_debug_inject_spinlock_timeout_rate(1);

    // Drive ~16 K spinlock acquires via syscall_putc.  Each putc grabs
    // fb_lock plus sched_lock during the dispatch, so we expect roughly
    // ~64 hits at 1/256 sampling.
    for (int i = 0; i < 16384; i++) {
        syscall_putc('.');
        if ((i & 0x7FF) == 0) syscall_putc('\n');
    }
    syscall_putc('\n');

    // Disable the hook and read back the cumulative hits.
    long hits = syscall_debug_inject_reset_all();
    if (hits == 0) {
        printf("# spinlock inject: 0 hits after 16 K syscall_putc — silent\n");
    }
    TAP_ASSERT(hits > 0,
               "2. spinlock injection hits > 0 after heavy lock traffic");
    TAP_ASSERT(hits < 100000,
               "3. spinlock injection hits stay within sample bound");

    tap_done();
    syscall_exit(0);
}
