// user/tests/cap_recursive_inherit_child.c
//
// Child half of the cap_recursive_inheritance.tap gate test. Spawned by
// user/tests/cap_recursive_inheritance.c with no arguments. Walks own
// cap_handle_table via DEBUG_CAP_CHECK_INHERITED_AUDIENCE; exits 0 if
// any cap with CAP_FLAG_RECURSIVE_INHERIT has the child's own pid in
// its audience set, 1 otherwise. The kernel inheritance walk in
// sched_create_user_process should have appended this pid via
// cap_object_derive_inherited() at spawn time.
//
// This binary is NOT in manifest.txt (the harness doesn't auto-run it);
// the parent test invokes it directly via syscall_spawn.

#include "../syscalls.h"

void _start(void) {
    long rc = syscall_debug_cap_check_inherited_audience();
    syscall_exit((int)rc);
}
