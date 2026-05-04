// user/tests/cap_system.c — Phase 26 FU25.F TAP gate test.
//
// Verifies the CAP_KIND_SYSTEM substrate landed and the
// TXN_FLAG_GLOBAL_SCOPE gate now goes through cap_system_resolve:
//
//   1. SELF_SCOPE txn_begin still works (substrate didn't break Phase 25 v1).
//   2. GLOBAL_SCOPE from a non-privileged caller (ktest) returns -EPERM.
//      Phase 25 v1 returned TXN_EPERM (-1) unconditionally; Phase 26 FU25.F
//      now lets it succeed for callers holding CAP_KIND_SYSTEM/RIGHT_INVOKE
//      (init at boot via cap_system_install_to_pid; future delegated daemons
//      via SYS_CAP_DERIVE + SYS_CAP_GRANT). ktest does not currently inherit
//      init's caps on spawn (CAP_FLAG_INHERITABLE inheritance not yet wired
//      in sched_create_user_process), so the negative path is the
//      deterministic-observable behavior here.
//   3. SELF_SCOPE still works after the GLOBAL_SCOPE rejection — the
//      substrate's reject path doesn't corrupt the txn state machine.
//
// Note on positive-path coverage: the cap-inheritance-on-spawn wiring is
// tracked as a Phase 26 follow-up; once it lands, a positive test from
// init's process tree confirms cap_system_resolve's match path. Until then,
// the substrate is exercised end-to-end via:
//   - cap_system_init() at kernel boot (klog message visible in serial log)
//   - cap_system_install_to_pid(init_pid) at autorun_register_init_pid
//   - cap_system_resolve called on EVERY GLOBAL_SCOPE attempt (this test)

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

void _start(void) {
    tap_plan(4);

    // 1. SELF_SCOPE works (Phase 25 v1 path unchanged).
    long h1 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "cap_sys_t1");
    TAP_ASSERT(h1 >= 0, "1. txn_begin(SELF_SCOPE) returns valid handle");
    long c1 = (h1 >= 0) ? syscall_txn_commit((uint32_t)h1) : -99;
    TAP_ASSERT(c1 == 0, "2. txn_commit on empty SELF_SCOPE txn returns 0");

    // 2. GLOBAL_SCOPE substrate gate. Without CAP_KIND_SYSTEM/RIGHT_INVOKE
    //    in caller's handle table, cap_system_resolve returns CAP_V2_EPERM
    //    and txn_begin propagates TXN_EPERM (== -1).
    long h2 = syscall_txn_begin(TXN_FLAG_GLOBAL_SCOPE, "cap_sys_t2");
    TAP_ASSERT(h2 == -1,
               "3. txn_begin(GLOBAL_SCOPE) returns -EPERM without CAP_KIND_SYSTEM");

    // 3. State machine unaffected — fresh SELF_SCOPE still works.
    long h3 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "cap_sys_t3");
    long c3 = (h3 >= 0) ? syscall_txn_commit((uint32_t)h3) : -99;
    TAP_ASSERT(h3 >= 0 && c3 == 0,
               "4. SELF_SCOPE still works after GLOBAL_SCOPE rejection");

    tap_done();
    syscall_exit(0);
}
