// user/tests/spinlock_timeout.c
//
// Phase 29 Session I (FU28.D) — spinlock_acquire_with_timeout substrate.
//
// The kernel adds a non-panicking timeout variant of spinlock_acquire.
// We can't exercise an actual contended path from user-space (no test-only
// kernel lock surface), but we CAN verify:
//
//   1. The g_spinlock_timeout_count diagnostic is readable through
//      DEBUG_SPINLOCK_TIMEOUT_COUNT.
//   2. Under normal gate workload the counter is bounded (i.e., no
//      runaway contention is happening — the variant is observe-mode
//      until a caller opts in).
//   3. The counter is monotone-nondecreasing across two samples
//      separated by heavy lock traffic.

#include "../libtap.h"
#include "../syscalls.h"

void _start(void) {
    tap_plan(3);

    // 1. Read the counter — must not error.
    long c1 = syscall_debug_spinlock_timeout_count();
    TAP_ASSERT(c1 >= 0,
               "1. DEBUG_SPINLOCK_TIMEOUT_COUNT readable (>= 0)");

    // 2. Drive ~4K syscall_putc calls — each grabs sched + fb locks.
    // The legacy spinlock_acquire path stays in the panic-on-budget
    // mode; the timeout counter must remain bounded because no current
    // call site uses the timeout variant yet.
    for (int i = 0; i < 4096; i++) {
        syscall_putc('.');
        if ((i & 0xFF) == 0xFF) syscall_putc('\n');
    }
    syscall_putc('\n');

    long c2 = syscall_debug_spinlock_timeout_count();
    TAP_ASSERT(c2 >= c1,
               "2. counter monotone-nondecreasing after heavy lock traffic");

    // 3. No site uses the timeout variant in production gate yet, so the
    //    delta must be 0 — confirms the API addition is gate-neutral.
    TAP_ASSERT((c2 - c1) == 0,
               "3. legacy spinlock_acquire callers do NOT increment timeout counter");

    tap_done();
    syscall_exit(0);
}
