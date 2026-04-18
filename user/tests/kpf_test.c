// user/tests/kpf_test.c
// Phase 13 fault-injection binary. Trips a *kernel-mode* page fault
// via SYS_DEBUG(DEBUG_KERNEL_PF). The kernel handler's syscall switch
// (gated by WITH_DEBUG_SYSCALL) deliberately dereferences an
// unmapped kernel address — that exception belongs to the kernel,
// not to this user process, so interrupts.c routes it through
// kpanic_at("page fault at 0x...").
//
// Validation runs out-of-band: scripts/run_panic_test.sh kpf_test 1
// boots QEMU, captures serial, and asks parse_oops.py for a parseable
// frame whose reason starts with "page fault".

#include "../syscalls.h"
#include <stdio.h>
#include <stdlib.h>

void _start(void) {
    printf("# kpf_test starting pid=%d\n", syscall_getpid());
    printf("# triggering kernel-mode page fault via SYS_DEBUG\n");

    int r = syscall_debug(DEBUG_KERNEL_PF, NULL);

    // Should never return — the kernel page-fault handler calls
    // kpanic_at, which never comes back.
    printf("# ERROR: SYS_DEBUG(DEBUG_KERNEL_PF) returned %d\n", r);
    printf("# Is WITH_DEBUG_SYSCALL defined at kernel build time?\n");
    exit(2);
}
