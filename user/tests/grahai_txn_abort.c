// user/tests/grahai_txn_abort.c — FU25.B gate test (pre-Phase-28 sweep).
//
// Verifies grahai --txn --abort (sentinel-file activation /.grahai-abort)
// wraps the plan in SYS_TXN_BEGIN + force-aborts via SYS_TXN_ABORT. The
// sentinel-file mechanism mirrors gash's try_run_script_sentinel and
// /.grahai-txn (FU25.B v1).
//
// /.grahai-abort implies --txn (force_abort && !use_txn is rejected at
// _start by the user-facing error path; the sentinel-file sets BOTH).
//
// Asserts:
//   1. /.grahai-abort sentinel staged
//   2. spawn bin/grahai returns valid PID
//   3. grahai exited 0 (--abort is success-path, not failure)
//   4. AUDIT_TXN_ABORT (43) emitted at least once

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

#define ABORT_SENTINEL "/.grahai-abort"

static int touch_sentinel(const char *path) {
    syscall_create(path, 0644);
    int fd = syscall_open(path);
    if (fd < 0) return -1;
    (void)syscall_truncate(fd);
    char b = '1';
    syscall_write(fd, &b, 1);
    syscall_close(fd);
    return 0;
}

void _start(void) {
    tap_plan(4);

    int rc = touch_sentinel(ABORT_SENTINEL);
    TAP_ASSERT(rc == 0, "1. /.grahai-abort sentinel staged");

    int pid = syscall_spawn("bin/grahai");
    TAP_ASSERT(pid > 0, "2. spawn bin/grahai returns valid PID");

    int status = -1;
    int wpid = syscall_wait(&status);
    TAP_ASSERT(wpid == pid && status == 0,
               "3. grahai exited 0 (--abort is success-path)");

    /* AUDIT_TXN_ABORT (code 43) must have fired at least once. */
    audit_entry_u_t records[16];
    long n_abort = syscall_audit_query(0, ~(uint64_t)0,
                                       (1u << AUDIT_TXN_ABORT),
                                       records, 16);
    TAP_ASSERT(n_abort >= 1,
               "4. AUDIT_TXN_ABORT (43) emitted by grahai --abort");

    tap_done();
    syscall_exit(0);
}
