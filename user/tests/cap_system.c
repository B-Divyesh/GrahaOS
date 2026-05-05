// user/tests/cap_system.c — Phase 26 FU25.F + Phase 27 FU26.D TAP gate.
//
// Verifies the CAP_KIND_SYSTEM substrate + the cap-inheritance-on-spawn
// wiring (Phase 27 Stage C2 / FU26.D):
//
//   1. SELF_SCOPE txn_begin still works (substrate didn't break Phase 25 v1).
//   2. txn_commit on the empty SELF_SCOPE txn returns 0.
//   3. Phase 27 Stage C2: GLOBAL_SCOPE now SUCCEEDS — ktest is autorun=PID 1
//      so cap_system_install_to_pid puts a CAP_KIND_SYSTEM/RIGHT_INVOKE
//      cap into ktest's handle table at boot; ktest spawns this test which
//      INHERITS it via the FU26.D walk in sched_create_user_process. Stage
//      C1 in tree before this expected the negative -EPERM path; once
//      FU26.D lands, this flips to the positive path.
//   4. SELF_SCOPE still works after a GLOBAL_SCOPE call — the substrate's
//      success path doesn't corrupt the txn state machine.
//
// Substrate exercised end-to-end:
//   - cap_system_init() at kernel boot (klog message in serial log)
//   - cap_system_install_to_pid(autorun_pid) at autorun_register_init_pid
//   - sched_create_user_process inheritance walk for CAP_FLAG_INHERITABLE
//   - cap_system_resolve on the GLOBAL_SCOPE attempt (test 3)

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    // 1. SELF_SCOPE works (Phase 25 v1 path unchanged).
    long h1 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "cap_sys_t1");
    TAP_ASSERT(h1 >= 0, "1. txn_begin(SELF_SCOPE) returns valid handle");
    long c1 = (h1 >= 0) ? syscall_txn_commit((uint32_t)h1) : -99;
    TAP_ASSERT(c1 == 0, "2. txn_commit on empty SELF_SCOPE txn returns 0");

    // 3. GLOBAL_SCOPE — POSITIVE path now that FU26.D inheritance is wired.
    //    Test runs as ktest's child (autorun=ktest); ktest is PID 1 with
    //    CAP_KIND_SYSTEM granted by autorun_register_init_pid. The Phase 27
    //    Stage C2 walk in sched_create_user_process derives the cap to this
    //    child's handle table on spawn, so cap_system_resolve passes.
    long h2 = syscall_txn_begin(TXN_FLAG_GLOBAL_SCOPE, "cap_sys_t2");
    if (h2 < 0) printf("# txn_begin(GLOBAL_SCOPE) rc=%ld\n", h2);
    long c2 = (h2 >= 0) ? syscall_txn_commit((uint32_t)h2) : -99;
    TAP_ASSERT(h2 >= 0 && c2 == 0,
               "3. txn_begin(GLOBAL_SCOPE) succeeds via inherited CAP_KIND_SYSTEM");

    // 4. State machine unaffected — fresh SELF_SCOPE still works.
    long h3 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "cap_sys_t3");
    long c3 = (h3 >= 0) ? syscall_txn_commit((uint32_t)h3) : -99;
    TAP_ASSERT(h3 >= 0 && c3 == 0,
               "4. SELF_SCOPE still works after GLOBAL_SCOPE call");

    tap_done();
    syscall_exit(0);
}
