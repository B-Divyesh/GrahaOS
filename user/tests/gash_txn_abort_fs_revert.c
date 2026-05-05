// user/tests/gash_txn_abort_fs_revert.c — FU25.A.3 gate test
// (pre-Phase-28 sweep B.3).
//
// Verifies that gash's `txn { echo X > /file } abort` actually reverts
// the FS write on existing files. FU25.A.3 substrate: cmd_echo /
// cmd_touch call syscall_txn_pin_path before opening the target so the
// active_txn's backing snapshot captures a pre-write fs_pin; on abort
// the kernel walks fs_pins[] and calls grahafs_revert_to_version per
// inode, which restores the pre-write content.
//
// v1 scope (matches FU25.A.3 spec): existing-file modification reverts.
// New-file create-then-abort reverting (delete on abort) is Phase 28
// day-1 work because it requires a DELETE pin type.
//
// Asserts:
//   1. /pre_revert seeded with "OLD\n" before txn
//   2. sentinel staged (gash auto-runs /.gash-script)
//   3. spawn bin/gash returns valid PID
//   4. gash exited 0 (abort path is success-path for the script)
//   5. /pre_revert content reverted to "OLD\n" (NOT "NEW\n")
//
// Note: AUDIT_TXN_ABORT (43) emission is already covered by
// gash_txn_abort.tap; this test focuses on the FS-revert SEMANTIC.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

extern int strcmp(const char *, const char *);
extern int strlen(const char *);

#define SENTINEL_PATH "/.gash-script"
#define TARGET_PATH   "/pre_revert"

static int write_file_full(const char *path, const char *content) {
    syscall_create(path, 0644);
    int fd = syscall_open(path);
    if (fd < 0) return -1;
    (void)syscall_truncate(fd);
    int len = strlen(content);
    ssize_t n = syscall_write(fd, content, (size_t)len);
    syscall_close(fd);
    return (n == len) ? 0 : -1;
}

static int read_file_full(const char *path, char *buf, int max) {
    int fd = syscall_open(path);
    if (fd < 0) return -1;
    ssize_t n = syscall_read(fd, buf, (size_t)(max - 1));
    syscall_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

void _start(void) {
    tap_plan(5);

    /* Cleanup any stale state from previous gate iters. */
    {
        int fd = syscall_open(SENTINEL_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
    }

    /* Stage 1: write OLD content to /pre_revert. This MUST happen
     * outside any txn so the file's version_chain_head_id reflects
     * "OLD\n" by the time gash's cmd_echo inside the txn body
     * captures the pre-write pin. */
    int rc = write_file_full(TARGET_PATH, "OLD\n");
    TAP_ASSERT(rc == 0, "1. /pre_revert seeded with OLD\\n pre-txn");

    /* Stage 2: stage the gash script with txn-abort-overwrite. */
    const char *script =
        "txn { echo NEW > /pre_revert } abort\n";
    rc = write_file_full(SENTINEL_PATH, script);
    TAP_ASSERT(rc == 0, "2. sentinel script staged at /.gash-script");

    /* Stage 3: spawn gash; it auto-runs the sentinel script, exits. */
    int pid = syscall_spawn("bin/gash");
    TAP_ASSERT(pid > 0, "3. spawn bin/gash returns valid PID");

    int status = -1;
    int wpid = syscall_wait(&status);
    TAP_ASSERT(wpid == pid && status == 0,
               "4. gash exited 0 (script-mode completed)");

    /* Stage 5: read /pre_revert and verify content is "OLD\n", NOT
     * "NEW\n". If FU25.A.3 is wired correctly the pin captured by
     * cmd_echo's syscall_txn_pin_path call lets snap_restore revert
     * via grahafs_revert_to_version on abort. */
    char content[64];
    int n = read_file_full(TARGET_PATH, content, sizeof(content));
    int reverted = (n == 4 && content[0] == 'O' && content[1] == 'L' &&
                    content[2] == 'D' && content[3] == '\n');
    if (!reverted) {
        printf("# observed content (n=%d): ", n);
        for (int i = 0; i < n && i < 16; i++) {
            printf("%c", content[i] >= 32 && content[i] < 127 ?
                          content[i] : '.');
        }
        printf("\n");
    }
    TAP_ASSERT(reverted, "5. /pre_revert reverted to OLD\\n after abort");

    tap_done();
    syscall_exit(0);
}
