// user/tests/gash_txn_commit.c — FU25.A.2 gate test (Phase 26 closeout).
//
// Verifies the gash `txn { ... } commit` parser dispatches SYS_TXN_BEGIN +
// SYS_TXN_COMMIT correctly. The substrate (cmd_txn_block in user/gash.c)
// landed in Phase 26 Stage A.3; this test was deferred because gash had
// no scripted-mode entrypoint. FU25.A.2 v1 path: sentinel file at
// /tmp/.gash-script. Test stages the script body there, spawns gash, gash
// auto-runs the sentinel (run_script_mode), self-cleans, exits.
//
// Asserts:
//   1. sentinel + result-file setup OK
//   2. gash spawn OK
//   3. gash exited 0
//   4. AUDIT_TXN_COMMIT (42) was emitted by the dispatched txn body

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int strcmp(const char *, const char *);
extern int strlen(const char *);

// Phase 26 closeout: GrahaFS doesn't have /tmp by default (clustertest +
// fstest_v2 use /), so we put the sentinel + marker at root.
#define SENTINEL_PATH "/.gash-script"
#define MARKER_PATH   "/gash_txn_commit_x"

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

    // Stage the script. We use an EMPTY txn body to keep the test fast in
    // TCG mode (FS ops via channel-mode ahcid run ~100ms+ per op; a body
    // with `echo > /file` redirects can take 60+ seconds in TCG and blow
    // the gate's 580s wall budget). Empty body still dispatches
    // SYS_TXN_BEGIN + SYS_TXN_COMMIT through gash's parser, which is what
    // FU25.A.2 actually verifies. The "abort reverts FS write" semantic
    // (FU25.A.3) is documented as a Phase 28 gap and not asserted here.
    const char *script =
        "txn {} commit\n";
    int wr = write_file(SENTINEL_PATH, script);
    TAP_ASSERT(wr == 0, "1. sentinel script staged at /.gash-script");

    // Spawn gash. It will detect the non-empty sentinel via
    // try_run_script_sentinel() at _start, run the script, truncate the
    // sentinel, and exit.
    int pid = syscall_spawn("bin/gash");
    TAP_ASSERT(pid > 0, "2. spawn bin/gash returns valid PID");

    int status = -1;
    int wpid = syscall_wait(&status);
    TAP_ASSERT(wpid == pid && status == 0,
               "3. gash exited 0 (script-mode completed)");

    // Verify AUDIT_TXN_COMMIT was emitted. since_ns=0, until_ns=~0,
    // event_mask only allows TXN_COMMIT.
    audit_entry_u_t records[8];
    long n_records = syscall_audit_query(0, ~(uint64_t)0,
                                          (1u << AUDIT_TXN_COMMIT),
                                          records, 8);
    TAP_ASSERT(n_records >= 1,
               "4. AUDIT_TXN_COMMIT (42) emitted by gash txn parser");

    tap_done();
    syscall_exit(0);
}
