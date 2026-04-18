// user/tests/spawntest.c
// Phase 12: TAP 1.4 port of user/spawntest.c (Phase 7d process mgmt tests).

#include "../libtap.h"
#include "../syscalls.h"
#include <stdio.h>
#include <stdlib.h>

void _start(void) {
    printf("=== Phase 7d: Process Management Tests ===\n\n");

    tap_plan(7);

    // Test 1: getpid returns valid PID
    {
        int pid = syscall_getpid();
        int ok = (pid > 0);
        if (ok) {
            printf("  My PID: %d\n", pid);
        }
        TAP_ASSERT(ok, "getpid returns valid PID");
    }

    // Test 2: spawn a valid program → expands to two assertions in original
    //         (test_pass "spawn returns valid child PID" + "wait reaps spawned child")
    {
        printf("\n--- Test: spawn valid program ---\n");
        int child_pid = syscall_spawn("bin/printf_test");
        int spawn_ok = (child_pid > 0);
        int wait_ok = 0;
        if (spawn_ok) {
            printf("  Spawned child PID: %d\n", child_pid);
            int status;
            int reaped = syscall_wait(&status);
            if (reaped >= 0) {
                printf("  Reaped PID: %d status: %d\n", reaped, status);
                wait_ok = 1;
            }
        }
        TAP_ASSERT(spawn_ok, "spawn returns valid child PID");
        TAP_ASSERT(wait_ok, "wait reaps spawned child");
    }

    // Test 3: spawn invalid program
    {
        printf("\n--- Test: spawn invalid program ---\n");
        int pid = syscall_spawn("bin/nonexistent");
        TAP_ASSERT(pid < 0, "spawn rejects invalid path");
    }

    // Test 4: wait with no children
    {
        printf("\n--- Test: wait with no children ---\n");
        int status;
        int ret = syscall_wait(&status);
        TAP_ASSERT(ret < 0, "wait returns error with no children");
    }

    // Test 5: kill a process (invalid PID)
    {
        printf("\n--- Test: kill with invalid PID ---\n");
        int ret = syscall_kill(999, 1); // SIGTERM to nonexistent PID
        TAP_ASSERT(ret < 0, "kill rejects invalid PID");
    }

    // Test 6: getpid consistency
    {
        printf("\n--- Test: getpid consistency ---\n");
        int pid1 = syscall_getpid();
        int pid2 = syscall_getpid();
        TAP_ASSERT(pid1 == pid2 && pid1 > 0, "getpid returns consistent PID");
    }

    tap_done();
    exit(0);
}
