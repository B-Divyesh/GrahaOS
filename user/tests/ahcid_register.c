// user/tests/ahcid_register.c — Phase 23 S7.1.
//
// Smoke test: spawn /bin/ahcid and verify it publishes /sys/blk/service.
// This exercises the full registration path: drv_register (gets MMIO + IRQ
// caps), drv_mmio_map, BIOS handoff, GHC.AE, port enumeration, IDENTIFY,
// and finally syscall_chan_publish.
//
// Phase 23 S3 stage 1 caveat: ahcid claims the AHCI controller, which the
// kernel's in-tree driver also drives (until the production cutover strips
// kernel ahci.c). The two coexist because ahcid uses the userdrv framework
// to map BAR5 — the same BAR the kernel maps via vmm_map_page. Both can
// happily read the registers (idempotent), but if both try to issue
// commands the result is undefined. To avoid this, the test acknowledges
// that it primarily validates the *publish path*; we don't issue real I/O
// from this test.
//
// 5 asserts.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

static void spin_ms_approx(uint64_t ms) {
    uint64_t loops = ms * 100000ull;
    for (volatile uint64_t i = 0; i < loops; i++) { }
}

static int blk_service_connectable(void) {
    cap_token_u_t wr = {.raw = 0};
    cap_token_u_t rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/blk/service", 16, &wr, &rd);
    return (rc == 0);
}

static int blk_list_connectable(void) {
    cap_token_u_t wr = {.raw = 0};
    cap_token_u_t rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/blk/list", 13, &wr, &rd);
    return (rc == 0);
}

void _start(void) {
    tap_plan(5);

    int pid = syscall_spawn("bin/ahcid");
    TAP_ASSERT(pid > 0, "1. /bin/ahcid spawns successfully");

    if (pid <= 0) {
        for (int i = 2; i <= 5; i++) {
            tap_skip("ahcid_register",
                     "spawn failed; cannot continue");
        }
        tap_done();
        syscall_exit(0);
    }

    // 2: /sys/blk/service publishes within ~3 s. Daemon startup includes
    // hardware probing + port_init for each present port + IDENTIFY which
    // can take a while on slow QEMU TCG.
    int svc_up = 0;
    for (int t = 0; t < 600; t++) {
        if (blk_service_connectable()) { svc_up = 1; break; }
        spin_ms_approx(5);
    }
    TAP_ASSERT(svc_up, "2. /sys/blk/service publishes within ~3 s");

    // 3: /sys/blk/list also publishes (diagnostic channel).
    int lst_up = blk_list_connectable();
    TAP_ASSERT(lst_up, "3. /sys/blk/list also published");

    // 4: connect-disconnect-reconnect to /sys/blk/service is stable.
    int reconnect_ok = blk_service_connectable();
    TAP_ASSERT(reconnect_ok, "4. /sys/blk/service stays connectable across multiple connects");

    // 5: kill ahcid, /sys/blk/service goes away.
    syscall_kill(pid, 9);
    int status = 0;
    (void)syscall_wait(&status);
    spin_ms_approx(50);
    int gone = 1;
    for (int t = 0; t < 100; t++) {
        if (blk_service_connectable()) { gone = 0; break; }
        spin_ms_approx(5);
    }
    TAP_ASSERT(gone, "5. /sys/blk/service deregisters after ahcid kill");

    tap_done();
    syscall_exit(0);
}
