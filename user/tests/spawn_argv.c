// user/tests/spawn_argv.c
//
// Phase 29 Session C (FU28.B) gate test.
//
// Validates SYS_SPAWN_ARGV (slot 1115): a parent invokes the kernel to
// spawn a child AND propagate argv.  Self-as-helper pattern:
//   - When run with argc == 0 (the ktest harness invokes _start(void))
//     this binary acts as the TEST PARENT: it spawns ITSELF with
//     argv = {"bin/tests/spawn_argv.tap", "alpha", "beta"} (argc=3),
//     waits for the child, and verifies the child wrote argv[1] and
//     argv[2] into /spawn_argv_echo.
//   - When run with argc > 0 (the kernel-driven spawn we just made) this
//     binary acts as the ECHO HELPER: it concatenates argv[1] + " " +
//     argv[2] into /spawn_argv_echo and exits 0.
//
// 5 asserts:
//   1. SYS_SPAWN_ARGV returns a positive pid.
//   2. child exits with status 0 (helper code path succeeded).
//   3. /spawn_argv_echo was created.
//   4. /spawn_argv_echo contents == "alpha beta".
//   5. SYS_SPAWN_ARGV with argc=0 (null-argv) still spawns successfully
//      (covers the argc==0 fast path).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int my_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0) && (*b == 0);
}

// Helper mode: invoked when argc > 0.  Write "argv[1] argv[2]" to
// /spawn_argv_echo, exit 0 on success / 1 on failure.
static void run_helper(int argc, char **argv) {
    if (argc < 3) {
        syscall_exit(2);
    }
    (void)syscall_create("/spawn_argv_echo", 0);
    int fd = syscall_open("/spawn_argv_echo");
    if (fd < 0) {
        syscall_exit(3);
    }
    int rc1 = (int)syscall_write(fd, argv[1], (size_t)my_strlen(argv[1]));
    int rc2 = (int)syscall_write(fd, " ", 1);
    int rc3 = (int)syscall_write(fd, argv[2], (size_t)my_strlen(argv[2]));
    syscall_close(fd);
    if (rc1 < 0 || rc2 < 0 || rc3 < 0) {
        syscall_exit(4);
    }
    syscall_exit(0);
}

void _start(int argc, char **argv) {
    // Helper path: dispatched by the kernel when SYS_SPAWN_ARGV propagates
    // argc > 0.  If argc == 0 (ktest harness), fall through to parent.
    if (argc > 0) {
        run_helper(argc, argv);
        // unreachable
    }

    tap_plan(5);

    // Clean up any prior run.
    {
        int fd = syscall_open("/spawn_argv_echo");
        if (fd >= 0) {
            (void)syscall_truncate(fd);
            syscall_close(fd);
        }
    }

    // 1. Spawn self with argv = {path, "alpha", "beta"}.
    char *child_argv[4];
    child_argv[0] = (char *)"bin/tests/spawn_argv.tap";
    child_argv[1] = (char *)"alpha";
    child_argv[2] = (char *)"beta";
    child_argv[3] = (char *)0;
    int pid = syscall_spawn_argv("bin/tests/spawn_argv.tap", 3, child_argv);
    if (pid <= 0) {
        printf("# syscall_spawn_argv rc=%d\n", pid);
    }
    TAP_ASSERT(pid > 0, "1. syscall_spawn_argv returns positive pid");

    // 2. Reap and verify exit status 0.
    int status = -1;
    int wpid = -1;
    if (pid > 0) {
        wpid = syscall_wait(&status);
    }
    if (wpid != pid || status != 0) {
        printf("# wait wpid=%d expected=%d status=%d\n", wpid, pid, status);
    }
    TAP_ASSERT(wpid == pid && status == 0,
               "2. child exited cleanly (helper saw argc=3 + valid argv)");

    // 3. /spawn_argv_echo was created by the child.
    int fd = syscall_open("/spawn_argv_echo");
    TAP_ASSERT(fd >= 0,
               "3. /spawn_argv_echo created by child (argv propagation succeeded)");

    // 4. Contents == "alpha beta".
    char buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0;
    int contents_ok = 0;
    if (fd >= 0) {
        long n = syscall_read(fd, buf, 63);
        if (n > 0) {
            buf[n] = 0;
            contents_ok = my_streq(buf, "alpha beta");
        }
        if (!contents_ok) {
            printf("# read n=%ld buf=[%s]\n", n, buf);
        }
        syscall_close(fd);
    }
    TAP_ASSERT(contents_ok, "4. /spawn_argv_echo contents == \"alpha beta\"");

    // 5. Spawn child with argc=1 (just path) — helper sees argc < 3 and
    //    exits with status 2.  Demonstrates argv propagation works for
    //    a different argc value AND that the child can short-circuit
    //    based on argv content.
    char *child_argv2[2];
    child_argv2[0] = (char *)"bin/tests/spawn_argv.tap";
    child_argv2[1] = (char *)0;
    int pid2 = syscall_spawn_argv("bin/tests/spawn_argv.tap", 1, child_argv2);
    int status2 = -1;
    int wpid2 = -1;
    if (pid2 > 0) {
        wpid2 = syscall_wait(&status2);
    }
    if (pid2 <= 0 || wpid2 != pid2 || status2 != 2) {
        printf("# pid2=%d wpid2=%d status2=%d\n", pid2, wpid2, status2);
    }
    TAP_ASSERT(pid2 > 0 && wpid2 == pid2 && status2 == 2,
               "5. argc=1 propagates: helper sees argc<3 and exits with status 2");

    tap_done();
    syscall_exit(0);
}
