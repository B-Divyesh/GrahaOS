// user/tests/cap_inherit.c
//
// Phase 27 Block C — Stage C2 / FU26.D capability inheritance gate.
//
// Verifies that CAP_KIND_SYSTEM (set with CAP_FLAG_INHERITABLE on init by
// cap_system_install_to_pid) is propagated to ktest's children at spawn
// time via the walk in sched_create_user_process. The end-to-end signal:
// txn_begin(TXN_FLAG_GLOBAL_SCOPE) — gated on cap_system_resolve seeing a
// CAP_KIND_SYSTEM cap with RIGHT_INVOKE in the caller's handle table —
// SUCCEEDS for this test child.
//
// Pre-FU26.D: ktest's child had an empty handle table → txn_begin(GLOBAL)
// returned -EPERM. Post-FU26.D: child inherits → returns valid handle.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    // 1. txn_begin(GLOBAL_SCOPE) succeeds — proves CAP_KIND_SYSTEM was
    // inherited from ktest at spawn time.
    long h = syscall_txn_begin(TXN_FLAG_GLOBAL_SCOPE, "cap_inherit_t1");
    if (h < 0) printf("# txn_begin(GLOBAL_SCOPE) rc=%ld\n", h);
    TAP_ASSERT(h >= 0,
               "1. txn_begin(GLOBAL_SCOPE) succeeds via inherited CAP_KIND_SYSTEM");

    // 2. Commit the empty txn cleanly.
    long c = (h >= 0) ? syscall_txn_commit((uint32_t)h) : -99;
    if (c != 0) printf("# txn_commit rc=%ld\n", c);
    TAP_ASSERT(c == 0, "2. txn_commit on inherited-cap txn returns 0");

    // 3. Repeat — inheritance is stable across multiple invocations.
    h = syscall_txn_begin(TXN_FLAG_GLOBAL_SCOPE, "cap_inherit_t2");
    if (h < 0) printf("# repeat txn_begin rc=%ld\n", h);
    TAP_ASSERT(h >= 0,
               "3. inheritance persists — second txn_begin(GLOBAL_SCOPE) succeeds");
    if (h >= 0) (void)syscall_txn_commit((uint32_t)h);

    // 4. SELF_SCOPE still works (no regression in non-cap-gated path).
    h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "cap_inherit_t3");
    TAP_ASSERT(h >= 0, "4. SELF_SCOPE unchanged after inheritance landed");
    if (h >= 0) (void)syscall_txn_commit((uint32_t)h);

    // 5. Spawning a child of THIS test doesn't crash. We don't have a
    // syscall to inspect the child's caps directly from here, but a
    // successful spawn-and-exit confirms the inheritance walk is safe
    // for nested process trees (init → ktest → cap_inherit → ???).
    // For now, verify the inheritance walk didn't break syscall_getpid.
    long pid = syscall_getpid();
    TAP_ASSERT(pid > 0, "5. syscall_getpid still works (smoke test)");

    tap_done();
    syscall_exit(0);
}
