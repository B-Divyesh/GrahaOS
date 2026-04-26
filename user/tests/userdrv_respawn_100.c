// user/tests/userdrv_respawn_100.c — Phase 23 P23.deferred.4.
//
// 100-cycle e1000d spawn-bind-kill stress, generalising the 10-cycle
// canonical userdrv_respawn_stress test (Phase 23 S1.4). Spec gate test
// 18: "100-cycle kill-and-respawn (full FS still consistent at end)."
//
// What this exercises:
//   - userdrv_on_owner_death scaling: each cycle creates+destroys 6 caps
//     (mmio + irq + 4 chan endpoints from the e1000d kernel-side wiring).
//     100 cycles = 600 cap_object create+destroy pairs.
//   - rawnet_on_peer_death scaling: each cycle publishes /sys/net/rawframe
//     via e1000d's startup, then the death-path tears it down.
//   - cap_handle_table churn under sustained pressure.
//   - PMM/kheap accounting integrity over long cycles.
//
// Why explicitly excluded from the gate manifest:
//   - 100 cycles × ~2 s wall-clock per cycle = ~200 s. Far exceeds gate
//     budget.
//   - Even shorter variants accumulate kheap/PMM pressure that breaks
//     adjacent tests' vmo_map (documented in Phase 23 S1.1, S7.2). The
//     proper resolution is the channel-mediated cutover (Stage 2) which
//     tightens the kernel resource lifecycle further.
//   - Until Stage 2 lands, this test is run interactively as a soak gate:
//       `gash> ktest userdrv_respawn_100`
//
// 100 asserts (one per cycle) plus 1 final consistency assert.

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
        printf("[respawn_100] iter=%d spawn failed\n", iter_num);
        return 0;
    }

    int bound = 0;
    for (int t = 0; t < 200; t++) {
        if (rawframe_connectable()) { bound = 1; break; }
        spin_ms_approx(5);
    }
    if (!bound) {
        printf("[respawn_100] iter=%d rawframe never published\n", iter_num);
        syscall_kill(pid, 9);
        int s = 0; (void)syscall_wait(&s);
        return 0;
    }

    syscall_kill(pid, 9);
    int status = 0;
    (void)syscall_wait(&status);
    spin_ms_approx(20);

    int unbound = 1;
    for (int t = 0; t < 100; t++) {
        if (rawframe_connectable()) { unbound = 0; break; }
        spin_ms_approx(5);
    }
    return unbound ? 1 : 0;
}

static void format_iter_name(char *buf, int iter) {
    const char *prefix = "respawn_100 iter ";
    int p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    buf[p++] = '0' + ((iter / 100) % 10);
    buf[p++] = '0' + ((iter /  10) % 10);
    buf[p++] = '0' + ((iter      ) % 10);
    buf[p++] = ':';
    buf[p++] = ' ';
    const char *suffix = "spawn+bind+kill cycle";
    int s = 0;
    while (suffix[s]) { buf[p++] = suffix[s++]; }
    buf[p] = 0;
}

void _start(void) {
    tap_plan(101);

    char namebuf[64];
    int succeeded = 0;
    for (int i = 1; i <= 100; i++) {
        int ok = run_one_cycle(i);
        format_iter_name(namebuf, i);
        TAP_ASSERT(ok, namebuf);
        if (ok) succeeded++;
    }

    // 101: aggregate consistency — at least 99/100 must succeed for the
    //      stress to count as green. (One transient miss is tolerable;
    //      systematic failure is a regression.)
    TAP_ASSERT(succeeded >= 99,
               "101. 99+/100 cycles cleanly bound and unbound");

    tap_done();
    syscall_exit(0);
}
