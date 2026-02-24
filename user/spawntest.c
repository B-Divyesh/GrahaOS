// user/spawntest.c - Phase 7d: Process management test suite
#include "syscalls.h"

// Simple helpers (not using libc to keep test minimal)
void print(const char *str) {
    while (*str) syscall_putc(*str++);
}

void print_int(int n) {
    if (n < 0) {
        syscall_putc('-');
        n = -n;
    }
    if (n == 0) {
        syscall_putc('0');
        return;
    }
    char buf[12];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        syscall_putc(buf[--i]);
    }
}

static int tests_passed = 0;
static int tests_failed = 0;

void test_pass(const char *name) {
    print("[PASS] ");
    print(name);
    print("\n");
    tests_passed++;
}

void test_fail(const char *name, const char *reason) {
    print("[FAIL] ");
    print(name);
    print(": ");
    print(reason);
    print("\n");
    tests_failed++;
}

void _start(void) {
    print("=== Phase 7d: Process Management Tests ===\n\n");

    // Test 1: getpid returns valid PID
    {
        int pid = syscall_getpid();
        if (pid > 0) {
            print("  My PID: ");
            print_int(pid);
            print("\n");
            test_pass("getpid returns valid PID");
        } else {
            test_fail("getpid returns valid PID", "returned <= 0");
        }
    }

    // Test 2: spawn a valid program
    {
        print("\n--- Test: spawn valid program ---\n");
        int child_pid = syscall_spawn("bin/printf_test");
        if (child_pid > 0) {
            print("  Spawned child PID: ");
            print_int(child_pid);
            print("\n");
            test_pass("spawn returns valid child PID");

            // Wait for child
            int status;
            int reaped = syscall_wait(&status);
            if (reaped >= 0) {
                print("  Reaped PID: ");
                print_int(reaped);
                print(" status: ");
                print_int(status);
                print("\n");
                test_pass("wait reaps spawned child");
            } else {
                test_fail("wait reaps spawned child", "wait returned error");
            }
        } else {
            test_fail("spawn returns valid child PID", "spawn returned error");
        }
    }

    // Test 3: spawn invalid program
    {
        print("\n--- Test: spawn invalid program ---\n");
        int pid = syscall_spawn("bin/nonexistent");
        if (pid < 0) {
            test_pass("spawn rejects invalid path");
        } else {
            test_fail("spawn rejects invalid path", "spawn succeeded for nonexistent");
        }
    }

    // Test 4: wait with no children
    {
        print("\n--- Test: wait with no children ---\n");
        int status;
        int ret = syscall_wait(&status);
        if (ret < 0) {
            test_pass("wait returns error with no children");
        } else {
            test_fail("wait returns error with no children", "wait succeeded unexpectedly");
        }
    }

    // Test 5: kill a process (send SIGTERM to self - should be default terminate)
    {
        print("\n--- Test: kill with invalid PID ---\n");
        int ret = syscall_kill(999, 1); // SIGTERM to nonexistent PID
        if (ret < 0) {
            test_pass("kill rejects invalid PID");
        } else {
            test_fail("kill rejects invalid PID", "kill succeeded for invalid PID");
        }
    }

    // Test 6: getpid consistency
    {
        print("\n--- Test: getpid consistency ---\n");
        int pid1 = syscall_getpid();
        int pid2 = syscall_getpid();
        if (pid1 == pid2 && pid1 > 0) {
            test_pass("getpid returns consistent PID");
        } else {
            test_fail("getpid returns consistent PID", "inconsistent PIDs");
        }
    }

    // Summary
    print("\n=== RESULTS ===\n");
    print("Passed: ");
    print_int(tests_passed);
    print("\nFailed: ");
    print_int(tests_failed);
    print("\n");

    if (tests_failed == 0) {
        print("\nALL TESTS PASSED!\n");
    } else {
        print("\nSOME TESTS FAILED!\n");
    }

    syscall_exit(tests_failed);
}
