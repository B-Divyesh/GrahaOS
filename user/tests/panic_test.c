// user/tests/panic_test.c
// Phase 13 fault-injection binary. NOT a TAP test — it intentionally
// never returns. Invoked via `make test-panic`, which boots QEMU with
// autorun=panic_test so the kernel spawns /bin/panic_test as PID 1.
//
// Sequence inside QEMU:
//   1. _start calls syscall_debug(DEBUG_PANIC, "...").
//   2. Kernel's SYS_DEBUG handler calls kpanic(reason).
//   3. kpanic emits the full ==OOPS== block to serial and invokes
//      kernel_shutdown() — under `-no-reboot` QEMU exits.
//
// Host-side validation:
//   scripts/parse_oops.py checks the serial log for a parseable
//   oops frame (reason, registers, stack trace, klog tail). If the
//   whole pipeline works, `make test-panic` exits 0.

#include "../syscalls.h"
#include "../libtap.h"

#include <stdio.h>
#include <stdlib.h>

void _start(void) {
    printf("# panic_test starting pid=%d\n", syscall_getpid());
    printf("# about to trigger controlled panic via SYS_DEBUG\n");

    // Some serial FIFO flushing before we yank the rug out from
    // under the kernel.
    for (volatile int i = 0; i < 1000; i++) { /* spin */ }

    int r = syscall_debug(DEBUG_PANIC,
                          "phase13 controlled panic (panic_test)");

    // We should never reach here — SYS_DEBUG(DEBUG_PANIC) is
    // __attribute__((noreturn)) in the kernel. If we do, the build
    // is missing WITH_DEBUG_SYSCALL, so emit a diagnostic and die.
    printf("# ERROR: SYS_DEBUG returned %d (expected: never return)\n", r);
    printf("# Is WITH_DEBUG_SYSCALL defined at kernel build time?\n");
    exit(2);
}
