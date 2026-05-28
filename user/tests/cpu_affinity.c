// user/tests/cpu_affinity.c
//
// Phase 29 Session I (FU24.E) — SYS_SET_CPU_AFFINITY gate test.
//
// Exercises the syscall surface only.  The kernel mapping (mask -> cpu_pinned
// via lowest-set-bit) is verified through return-code observation: bad masks
// produce -EINVAL, valid masks return 0, mask==0xFFFFFFFFu (all-1s, "unpin")
// returns 0.  Cross-CPU dispatch verification is out of scope here (it would
// need extra DEBUG syscalls); the goal is "the syscall is wired and the
// semantic of the mask is consistent".

#include "../libtap.h"
#include "../syscalls.h"

void _start(void) {
    tap_plan(4);

    // 1. Self-targeted (pid=0) with mask=0xFFFFFFFFu must succeed.
    long rc1 = syscall_set_cpu_affinity(0, 0xFFFFFFFFu);
    TAP_ASSERT(rc1 == 0,
               "1. self-targeted unpin (mask=all-1s) returns 0");

    // 2. Mask=0 must return -EINVAL (-22).
    long rc2 = syscall_set_cpu_affinity(0, 0);
    TAP_ASSERT(rc2 == -22,
               "2. mask=0 returns -EINVAL");

    // 3. Self-targeted with mask=1 (CPU 0) must succeed.
    long rc3 = syscall_set_cpu_affinity(0, 0x1u);
    TAP_ASSERT(rc3 == 0,
               "3. self-targeted pin to CPU 0 (mask=0x1) returns 0");

    // 4. Self-targeted with mask=(1<<31) — likely a non-existent CPU on test
    // hardware (we run with smp.cpus=4 typically).  Should return -EINVAL.
    long rc4 = syscall_set_cpu_affinity(0, 1u << 31);
    TAP_ASSERT(rc4 == -22,
               "4. self-targeted pin to non-existent CPU (mask=1<<31) returns -EINVAL");

    tap_done();
    syscall_exit(0);
}
