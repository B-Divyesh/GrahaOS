// user/tests/inject_pmm_fail.c
//
// Phase 28 Session G.1 — fault injection harness.  Validates that the
// pmm_alloc_page(s) countdown hook (arch/x86_64/mm/pmm.c) fires when
// the soak harness sets g_debug_pmm_fail_nth via SYS_DEBUG.
//
// We drive pmm via syscall_vmo_create with VMO_CONTIGUOUS|VMO_PINNED|
// VMO_ZEROED so the backing page is eagerly allocated.  Flag values
// must match kernel/mm/vmo.h exactly — they are NOT the obvious
// constants (VMO_PINNED=0x4, VMO_CONTIGUOUS=0x20).

#include "../libtap.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

// Kernel/mm/vmo.h flag bits (must stay in sync).
#define VMO_FLAG_ZEROED      0x1u
#define VMO_FLAG_ONDEMAND    0x2u
#define VMO_FLAG_PINNED      0x4u
#define VMO_FLAG_CONTIGUOUS  0x20u

void _start(void) {
    tap_plan(3);

    syscall_debug_inject_reset_all();

    uint32_t flags = VMO_FLAG_ZEROED | VMO_FLAG_PINNED | VMO_FLAG_CONTIGUOUS;

    // Baseline: vmo_create should succeed without injection.
    long base = syscall_vmo_create(4096, flags);
    if (base <= 0) printf("# baseline vmo_create rc=%ld\n", base);
    TAP_ASSERT(base > 0, "1. baseline vmo_create succeeds with no injection");

    // Arm pmm_fail_nth=200 — high enough to absorb background pmm
    // traffic before our vmo_create loop drives its own page allocs.
    syscall_debug_inject_pmm_fail_nth(200);

    int n_ok = 0, n_fail = 0;
    for (int i = 0; i < 256; i++) {
        long rc = syscall_vmo_create(4096, flags);
        if (rc > 0) n_ok++;
        else n_fail++;
    }
    if (n_fail == 0) {
        printf("# pmm inject: 0 fails in 256 attempts (ok=%d) — hook silent\n",
               n_ok);
    } else {
        printf("# pmm inject: ok=%d fail=%d\n", n_ok, n_fail);
    }
    TAP_ASSERT(n_fail >= 1,
               "2. at least one vmo_create fails when pmm hook is armed");

    syscall_debug_inject_reset_all();
    long after = syscall_vmo_create(4096, flags);
    if (after <= 0) printf("# after-reset vmo_create rc=%ld\n", after);
    TAP_ASSERT(after > 0, "3. vmo_create works again after reset");

    tap_done();
    syscall_exit(0);
}
