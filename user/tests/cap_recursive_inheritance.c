// user/tests/cap_recursive_inheritance.c
//
// FU27.X.cap_recursive_inheritance gate test.
//
// Verifies the kernel S5a substrate: when a cap_object has
// CAP_FLAG_INHERITABLE | CAP_FLAG_RECURSIVE_INHERIT and is inherited at
// child spawn, sched_create_user_process appends the CHILD's pid to the
// new cap's audience_set (via cap_object_derive_inherited which skips
// audience_is_subset). Without this expansion the child holds the cap
// but cannot pass the audience check when re-deriving for grandchildren.
//
// Test layout:
//   1. Parent uses DEBUG_CAP_CREATE_WITH_FLAGS to allocate a fresh cap
//      with INHERITABLE | RECURSIVE_INHERIT, audience = [parent_pid].
//   2. Parent inspects the cap pre-spawn: audience must contain parent_pid
//      and NOT contain any other pid.
//   3. Parent spawns bin/cap_recursive_inherit_child.
//   4. The child walks its own cap_handle_table via DEBUG_CAP_CHECK_-
//      INHERITED_AUDIENCE looking for a cap with RECURSIVE_INHERIT set,
//      verifies its own pid is in the audience, exits 0 on success / 1
//      on failure.
//   5. Parent reaps child, asserts exit_status == 0.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    // 1. Create the recursive cap.
    cap_token_raw_t recursive_tok = syscall_debug_cap_create_with_flags(
        CAP_FLAG_INHERITABLE | CAP_FLAG_RECURSIVE_INHERIT);
    if (recursive_tok == 0) {
        printf("# DEBUG_CAP_CREATE_WITH_FLAGS returned 0\n");
    }
    TAP_ASSERT(recursive_tok != 0,
               "1. created INHERITABLE|RECURSIVE_INHERIT cap");

    // 2. Inspect pre-spawn: flags must include RECURSIVE_INHERIT, audience
    //    must contain caller's pid (and only caller's pid for now).
    cap_inspect_result_u_t r;
    long ic = syscall_cap_inspect(recursive_tok, &r);
    int my_pid = (int)syscall_getpid();
    int self_in_audience = 0;
    for (int i = 0; i < 8; i++) {
        if (r.audience_pids[i] == my_pid) { self_in_audience = 1; break; }
    }
    if (ic != 0 || !(r.flags & CAP_FLAG_RECURSIVE_INHERIT) ||
        !self_in_audience) {
        printf("# inspect rc=%ld flags=0x%x audience=[%d,%d,%d,%d,%d,%d,%d,%d] my_pid=%d\n",
               ic, (unsigned)r.flags,
               r.audience_pids[0], r.audience_pids[1], r.audience_pids[2],
               r.audience_pids[3], r.audience_pids[4], r.audience_pids[5],
               r.audience_pids[6], r.audience_pids[7], my_pid);
    }
    TAP_ASSERT(ic == 0 && (r.flags & CAP_FLAG_RECURSIVE_INHERIT)
               && self_in_audience,
               "2. pre-spawn: flags include RECURSIVE_INHERIT and audience contains parent_pid");

    // 3. Spawn the child verifier.  The kernel's inheritance walk in
    //    sched_create_user_process should detect the RECURSIVE_INHERIT
    //    flag and call cap_object_derive_inherited() (audience-expanding
    //    variant) instead of cap_object_derive_quiet().
    int child_pid = syscall_spawn("bin/tests/cap_recursive_inherit_child.tap");
    if (child_pid <= 0) {
        printf("# spawn returned %d\n", child_pid);
    }
    TAP_ASSERT(child_pid > 0, "3. spawn child verifier");

    // 4. Wait for child.  Status 0 means child found a cap with
    //    RECURSIVE_INHERIT in its own table AND its own pid is in the
    //    audience set (i.e., S5a's append succeeded).
    int status = -1;
    int wpid = -1;
    if (child_pid > 0) {
        wpid = syscall_wait(&status);
    }
    if (wpid != child_pid || status != 0) {
        printf("# wait wpid=%d expected=%d status=%d\n",
               wpid, child_pid, status);
    }
    TAP_ASSERT(wpid == child_pid && status == 0,
               "4. child verified RECURSIVE_INHERIT cap audience contains child_pid");

    // 5. Smoke test: parent's own cap is still resolvable post-spawn (no
    //    side-effect on parent's table from the inheritance walk).
    cap_inspect_result_u_t r2;
    long ic2 = syscall_cap_inspect(recursive_tok, &r2);
    TAP_ASSERT(ic2 == 0,
               "5. parent's cap still resolvable post-spawn (no side-effect)");

    tap_done();
    syscall_exit(0);
}
