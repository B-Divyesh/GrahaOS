// user/tests/userdrv_respawn_stress.c
//
// Phase 23 S1 — Stress test the userdrv MMIO/IRQ/channel cap synchronous
// cleanup path (P22.G.4 fix). Each iteration spawns /bin/e1000d, waits for
// its rawframe publish (signals successful drv_register including MMIO map +
// IRQ vector + channel pair allocation), then kills the daemon. The kill
// triggers sched_reap_zombie → userdrv_on_owner_death → cap_object_destroy
// on each cap_object the daemon held. If S1's synchronous destroy works,
// the next iteration's drv_register sees a fully-cleaned slot and succeeds.
//
// Without S1, kernel-side cap_object slots and underlying VMO refcounts
// would leak; eventually drv_register fails and the spawn-poll-bind loop
// times out. 10 iterations × 6 caps per iter = 60 cap_objects we exercise
// the create+destroy path on.
//
// 10 asserts (one per iteration confirming the cycle).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

static void spin_ms_approx(uint64_t ms) {
    uint64_t loops = ms * 100000ull;
    for (volatile uint64_t i = 0; i < loops; i++) { }
}

static int rawframe_connectable(void) {
    cap_token_u_t wr = {.raw = 0}, rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/net/rawframe", 17, &wr, &rd);
    return (rc == 0);
}

static int run_one_cycle(int iter_num) {
    int pid = syscall_spawn("bin/e1000d");
    if (pid <= 0) {
        printf("[respawn_stress] iter=%d spawn failed\n", iter_num);
        return 0;
    }

    int bound = 0;
    for (int t = 0; t < 200; t++) {
        if (rawframe_connectable()) { bound = 1; break; }
        spin_ms_approx(5);
    }
    if (!bound) {
        printf("[respawn_stress] iter=%d rawframe never published\n", iter_num);
        syscall_kill(pid, 9);
        int s = 0; (void)syscall_wait(&s);
        return 0;
    }

    syscall_kill(pid, 9);
    int status = 0;
    (void)syscall_wait(&status);

    // Allow a short settle window for sched_reap_zombie to run cleanup.
    spin_ms_approx(20);

    // Confirm the registry slot is clear (rawframe should NOT be connectable
    // — the publisher is gone). This validates rawnet_on_peer_death + the
    // userdrv cap destroys ran synchronously.
    int unbound = 1;
    for (int t = 0; t < 100; t++) {
        if (rawframe_connectable()) { unbound = 0; break; }
        spin_ms_approx(5);
    }
    if (!unbound) {
        printf("[respawn_stress] iter=%d rawframe still bound after kill\n", iter_num);
        return 0;
    }
    return 1;
}

void _start(void) {
    tap_plan(10);

    char namebuf[48];
    for (int i = 1; i <= 10; i++) {
        int ok = run_one_cycle(i);
        // Format assertion name without snprintf (libc lacks it).
        const char *prefix = "respawn_stress iter ";
        int p = 0;
        while (prefix[p]) { namebuf[p] = prefix[p]; p++; }
        namebuf[p++] = '0' + (i / 10);
        namebuf[p++] = '0' + (i % 10);
        namebuf[p++] = ':';
        namebuf[p++] = ' ';
        const char *suffix = "spawn+bind+kill cycle";
        int s = 0;
        while (suffix[s] && p < 47) { namebuf[p++] = suffix[s++]; }
        namebuf[p] = 0;
        TAP_ASSERT(ok, namebuf);
    }

    tap_done();
    syscall_exit(0);
}
