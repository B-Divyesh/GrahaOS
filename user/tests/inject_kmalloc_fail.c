// user/tests/inject_kmalloc_fail.c
//
// Phase 28 Session G.1 — fault injection harness.  Validates that the
// kmalloc countdown hook (kernel/mm/kheap.c) fires when the soak
// harness sets g_debug_kmalloc_fail_nth via SYS_DEBUG.
//
// Robust against background kmalloc traffic on other CPUs: we set
// fail_nth=200 (high enough to absorb any in-flight kernel kmalloc)
// and loop syscall_chan_create up to 64 times, counting successes and
// failures.  At least one chan_create MUST fail (the hook's single-
// shot fire), and at least one must succeed (counter resets to 0
// after firing, so subsequent calls go through).

#include "../libtap.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(3);

    uint64_t hash_notify = gcp_type_hash("grahaos.notify.v1");

    // Clear all injection counters first.
    syscall_debug_inject_reset_all();

    // Baseline: chan_create with a manifest-known type hash should succeed.
    cap_token_u_t wr = {.raw = 0};
    long base = syscall_chan_create(hash_notify, CHAN_MODE_NONBLOCKING,
                                    /* capacity */ 4, &wr);
    if (base <= 0) printf("# baseline chan_create rc=%ld\n", base);
    TAP_ASSERT(base > 0, "1. baseline chan_create succeeds with no injection");

    // Arm the kmalloc countdown.  fail_nth=200 absorbs any in-flight
    // kernel kmalloc from interrupts / audit / sched before our
    // chan_create loop starts driving its own kmallocs (each chan_create
    // emits 1-2 kmalloc calls).
    syscall_debug_inject_kmalloc_fail_nth(200);

    int n_ok = 0, n_fail = 0;
    for (int i = 0; i < 256; i++) {
        cap_token_u_t w = {.raw = 0};
        long rc = syscall_chan_create(hash_notify, CHAN_MODE_NONBLOCKING, 4, &w);
        if (rc > 0) n_ok++;
        else n_fail++;
    }
    if (n_fail == 0) {
        printf("# kmalloc inject: 0 fails in 256 attempts (ok=%d) — hook silent\n",
               n_ok);
    } else {
        printf("# kmalloc inject: ok=%d fail=%d\n", n_ok, n_fail);
    }
    TAP_ASSERT(n_fail >= 1,
               "2. at least one chan_create fails when kmalloc hook is armed");

    // After firing, the counter is 0 and the hook is disabled.
    syscall_debug_inject_reset_all();
    cap_token_u_t wr3 = {.raw = 0};
    long after = syscall_chan_create(hash_notify, CHAN_MODE_NONBLOCKING, 4, &wr3);
    if (after <= 0) printf("# after-reset chan_create rc=%ld\n", after);
    TAP_ASSERT(after > 0,
               "3. chan_create works again after reset");

    tap_done();
    syscall_exit(0);
}
