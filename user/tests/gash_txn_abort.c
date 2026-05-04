// user/tests/gash_txn_abort.c — FU25.A.2 gate test (Phase 26 closeout).
//
// Symmetric to gash_txn_commit: verifies the gash `txn { ... } abort`
// parser dispatches SYS_TXN_BEGIN + SYS_TXN_ABORT correctly. Substrate is
// Phase 26 Stage A.3 cmd_txn_block.
//
// FU25.A.3 (deferred to Phase 28 entry-gate): Phase 25's txn_abort reverts
// FS state via grahafs_revert_to_version per fs_pin captured at txn_begin.
// Files opened-and-closed inside the txn body (which gash's `echo > X`
// builtin does) do NOT get fs_pin captured, so their writes are NOT
// reverted in v1. The test therefore asserts on the syscall + audit path
// (proven correct), not on file revert (a Phase 28 deliverable).
//
// Asserts:
//   1. sentinel script staged
//   2. gash spawn OK
//   3. gash exited 0
//   4. AUDIT_TXN_ABORT (43) was emitted by the dispatched txn body

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int strcmp(const char *, const char *);
extern int strlen(const char *);

// Phase 26 closeout: GrahaFS doesn't have /tmp by default (clustertest +
// fstest_v2 use /), so we put the sentinel + marker at root.
#define SENTINEL_PATH "/.gash-script"
#define MARKER_PATH   "/gash_txn_abort_y"

static int write_file(const char *path, const char *content) {
    syscall_create(path, 0);
    int fd = syscall_open(path);
    if (fd < 0) return -1;
    (void)syscall_truncate(fd);
    int len = strlen(content);
    ssize_t n = syscall_write(fd, content, (size_t)len);
    syscall_close(fd);
    return (n == len) ? 0 : -1;
}

void _start(void) {
    tap_plan(4);

    // Cleanup any stale state from previous runs.
    {
        int fd = syscall_open(SENTINEL_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
        fd = syscall_open(MARKER_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
    }

    // Stage the abort script. Empty txn body (see gash_txn_commit.c rationale
    // for FS-op cost in TCG). Empty body still dispatches SYS_TXN_BEGIN +
    // SYS_TXN_ABORT through gash's parser, which is what FU25.A.2 verifies.
    const char *script =
        "txn {} abort\n";
    int wr = write_file(SENTINEL_PATH, script);
    TAP_ASSERT(wr == 0, "1. sentinel script staged at /.gash-script");

    // Spawn gash. Auto-runs the sentinel.
    int pid = syscall_spawn("bin/gash");
    TAP_ASSERT(pid > 0, "2. spawn bin/gash returns valid PID");

    int status = -1;
    int wpid = syscall_wait(&status);
    TAP_ASSERT(wpid == pid && status == 0,
               "3. gash exited 0 (script-mode completed)");

    // Verify AUDIT_TXN_ABORT was emitted.
    audit_entry_u_t records[8];
    long n_records = syscall_audit_query(0, ~(uint64_t)0,
                                          (1u << AUDIT_TXN_ABORT),
                                          records, 8);
    TAP_ASSERT(n_records >= 1,
               "4. AUDIT_TXN_ABORT (43) emitted by gash txn parser");

    tap_done();
    syscall_exit(0);
}
