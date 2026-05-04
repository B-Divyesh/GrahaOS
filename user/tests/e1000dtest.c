// user/tests/e1000dtest.c
//
// Phase 22 Stage F — End-to-end TAP test for the userspace E1000 daemon.
//
// Pre-Stage-F this test asserted MAC + link-state via the kernel-side
// e1000_proxy (SYS_NET_IFCONFIG).  The proxy is retired with Mongoose;
// the only signal we have outside netd that "e1000d is alive and bound"
// is whether `/sys/net/rawframe` answers SYS_CHAN_CONNECT.  This rewrite
// asserts the bind/unbind/rebind cycle through that signal exclusively.
//
// 6 asserts (FU24.G: legacy assertions 7-10 deleted; respawn coverage moved
// to user/tests/userdrv_respawn_stress.c, 10-cycle stress, Phase 23 S1):
//   1. /bin/e1000d spawn succeeds.
//   2. /sys/net/rawframe is connectable within 2 s (publish OK).
//   3. (skip)  MAC byte non-zero — moved to netd-side telemetry; not
//              reachable from a kernel-only test harness.
//   4. (skip)  MAC OUI matches QEMU (52:54:00) — same reason.
//   5. (skip)  link byte is binary — same reason.
//   6. After SIGKILL on the daemon, the registry slot clears (-EBADF
//      from SYS_CHAN_CONNECT within 500 ms).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);

static int rawframe_connectable(void) {
    cap_token_u_t wr = {.raw = 0}, rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/net/rawframe", 17, &wr, &rd);
    return (rc == 0);
}

void _start(void) {
    tap_plan(6);

    int pid = syscall_spawn("bin/e1000d");
    TAP_ASSERT(pid > 0, "1. /bin/e1000d spawn succeeds");
    if (pid <= 0) {
        for (int i = 2; i <= 6; i++) {
            tap_skip("e1000dtest assertion",
                     "FU24.G: e1000d daemon spawn failure (environmental)");
        }
        tap_done();
        syscall_exit(0);
    }

    // 2: rawframe binds.  Phase 24a (path A): bumped 200→2000 polls (~10 s)
    // because channel-mode FS adds ~100 ms per ELF page load plus extra
    // scheduling pressure from ahcid running concurrently.  See
    // feedback_phase24a_tcg_ahci_floor.md.
    int bound = 0;
    int polls_used = 0;
    for (int try_ = 0; try_ < 2000; try_++) {
        if (rawframe_connectable()) { bound = 1; polls_used = try_; break; }
        spin_ms_approx(5);
    }
    printf("[e1000dtest] rawframe bound after %d polls\n", polls_used);
    TAP_ASSERT(bound, "2. /sys/net/rawframe binds after daemon spawn");

    // 3-5: MAC/link asserts retired (proxy gone). Skip with reason.
    tap_skip("3. MAC is non-zero",
             "FU24.G/Phase22-F: MAC visible only via netd telemetry (kernel proxy retired)");
    tap_skip("4. MAC OUI 52:54:00",
             "FU24.G/Phase22-F: netd /sys/net/service is the only post-F MAC source");
    tap_skip("5. link state byte binary",
             "FU24.G/Phase22-F: netd is the only post-F link signal");

    // 6: kill daemon, rawframe disappears.
    syscall_kill(pid, 9);
    int status = 0;
    (void)syscall_wait(&status);
    spin_ms_approx(20);

    int unbound = 1;
    for (int try_ = 0; try_ < 100 && unbound; try_++) {
        if (rawframe_connectable()) { unbound = 0; break; }
        spin_ms_approx(5);
    }
    TAP_ASSERT(unbound, "6. rawframe deregisters after daemon kill");

    // FU24.G: legacy assertions 7-10 (respawn re-publish, while-alive sanity,
    // post-kill sanity, second-respawn) deleted in this audit pass.
    // Respawn coverage now lives entirely in user/tests/userdrv_respawn_stress.c
    // (10-cycle spawn-kill-respawn stress, Phase 23 S1, 10/10 in gate). The
    // single-spawn bind/unbind path here covers the wave-A behaviour.

    tap_done();
    syscall_exit(0);
}
