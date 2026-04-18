// user/tests/ktest_capture.c
// Phase 12 work unit 14 — gate test for ktest's per-test spawn/wait
// capture path. Exercises the same primitives ktest uses: spawn a
// child, wait, read exit code. We use bin/printf_test (NOT a TAP
// binary) as the child so its output doesn't confuse the TAP parser
// of the enclosing run.

#include "../libtap.h"
#include "../syscalls.h"
#include <stdio.h>
#include <stdlib.h>

void _start(void) {
    tap_plan(4);

    int pid = syscall_spawn("bin/printf_test");
    TAP_ASSERT(pid > 0, "1. syscall_spawn returns valid PID for a real binary");
    if (pid <= 0) {
        tap_bail_out("spawn failed — cannot exercise capture");
    }

    int status = -999;
    int reaped = syscall_wait(&status);

    TAP_ASSERT(reaped == pid,
               "2. syscall_wait reaps the spawned child (PID match)");

    // printf_test exits 0 when it completes successfully.
    TAP_ASSERT(status == 0,
               "3. child (printf_test) exits with status 0");

    // Spawn of a non-existent path returns -1 cleanly (no crash).
    int bad = syscall_spawn("bin/no_such_binary_zzz");
    TAP_ASSERT(bad < 0,
               "4. spawn of non-existent path returns < 0 without crash");

    tap_done();
    exit(0);
}
