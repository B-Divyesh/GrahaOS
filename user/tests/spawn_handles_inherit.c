// user/tests/spawn_handles_inherit.c
//
// Phase 29 Session C (FU25.H) gate test.
//
// Validates SYS_SPAWN_EX with SPAWN_ATTR_HAS_HANDLES_U: handles_to_inherit[]
// transfers cap_token raws from caller's table to the child's table at
// spawn time (in addition to the existing CAP_FLAG_INHERITABLE walk).
//
// Self-as-helper via sentinel-file pattern (matches grahai's --txn flow):
//   - On entry, check /.spawn_handles_role.  If absent, run as TEST PARENT.
//     If "BASELINE", run baseline-helper.  If "WITH", run inherit-helper.
//   - Parent creates the sentinel before each spawn, deletes it after.
//
// 5 asserts:
//   1. DEBUG_CAP_CREATE_WITH_FLAGS returns a non-zero token for tok1.
//   2. DEBUG_CAP_CREATE_WITH_FLAGS returns a non-zero token for tok2.
//   3. baseline spawn (no handles_to_inherit) completes; helper recorded
//      a valid handle count.
//   4. inherit-spawn (with handles_to_inherit=[tok1,tok2]) completes;
//      helper recorded a valid handle count.
//   5. inherit-count == baseline-count + 2 (proves both handles were
//      transferred into the child's table).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static int my_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0) && (*b == 0);
}

static int read_text_file(const char *path, char *out, int max) {
    int fd = syscall_open(path);
    if (fd < 0) return -1;
    for (int i = 0; i < max; i++) out[i] = 0;
    long n = syscall_read(fd, out, (size_t)(max - 1));
    syscall_close(fd);
    if (n <= 0) return -1;
    out[n] = 0;
    return (int)n;
}

static int read_count_file(const char *path) {
    char buf[32];
    int n = read_text_file(path, buf, sizeof(buf));
    if (n <= 0) return -1;
    int v = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
    }
    return v;
}

// FU29.X.shell_sentinel_flake: robust verified write. Loops on short writes
// (FU24.A channel-mode FS returns short under kheap load), retries the whole
// open, and READS BACK to confirm the content is durable. Returns 0 only if a
// subsequent read sees exactly the bytes we wrote — callers use this to avoid
// spawning a child against a half-written sentinel (which causes the fork bomb).
static int write_text_file(const char *path, const char *text, int len) {
    for (int attempt = 0; attempt < 6; attempt++) {
        (void)syscall_create(path, 0);
        int fd = syscall_open(path);
        if (fd < 0) continue;
        (void)syscall_truncate(fd);
        int total = 0;
        while (total < len) {
            ssize_t n = syscall_write(fd, text + total, (size_t)(len - total));
            if (n <= 0) break;
            total += (int)n;
        }
        syscall_close(fd);
        if (total != len) continue;
        // Verify read-back.
        char vbuf[64];
        int vfd = syscall_open(path);
        if (vfd < 0) continue;
        ssize_t vn = syscall_read(vfd, vbuf, sizeof(vbuf) - 1);
        syscall_close(vfd);
        if (vn == len) {
            int ok = 1;
            for (int i = 0; i < len; i++) if (vbuf[i] != text[i]) { ok = 0; break; }
            if (ok) return 0;
        }
    }
    return -1;
}

static void write_count_file(const char *path, int count) {
    char buf[16];
    int idx = 0;
    if (count == 0) {
        buf[idx++] = '0';
    } else {
        char digits[12];
        int d = 0;
        int c = count;
        while (c > 0) { digits[d++] = '0' + (c % 10); c /= 10; }
        for (int i = d - 1; i >= 0; i--) buf[idx++] = digits[i];
    }
    write_text_file(path, buf, idx);
}

static void delete_file_by_truncate(const char *path) {
    int fd = syscall_open(path);
    if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
}

void _start(int argc, char **argv) {
    /* FU29.X.shell_sentinel_flake — fork-bomb-proof role detection.
     * The old design read /.spawn_handles_role to tell child from parent; under
     * the FU24.A channel-mode FS flake a child could read it empty, run the
     * PARENT path, and spawn again -> fork bomb -> 700s gate timeout. New design:
     *   - BASELINE child is spawned via SYS_SPAWN_ARGV with argv[1]="BASELINE",
     *     so it identifies via argv (no FS read) and can NEVER run the parent
     *     path -> can never fork-bomb.
     *   - WITH child must use SYS_SPAWN_EX (handles_to_inherit, argc==0); it
     *     reads the VERIFIED-durable /.spawn_handles_role. If that read flakes,
     *     the /.shi_guard two-factor guard (written+verified by the genuine
     *     parent) makes the confused child exit instead of recursing.
     *   - The genuine parent is the only argc==0 process that sees BOTH role
     *     AND /.shi_guard absent; even a flake²-confused child terminates at
     *     depth ~1 (its own children are well-behaved). */

    /* BASELINE child: argv-driven, flake-proof. */
    if (argc >= 2 && argv && argv[1] && my_streq(argv[1], "BASELINE")) {
        int hc = (int)syscall_debug3(DEBUG_HANDLE_COUNT, 0, 0);
        write_count_file("/spawn_handles_baseline", hc);
        syscall_exit(0);
    }

    /* argc==0: WITH child (reads verified role) OR the genuine parent. */
    char role[32];
    int role_n = read_text_file("/.spawn_handles_role", role, sizeof(role));
    if (role_n > 0 && my_streq(role, "WITH")) {
        int hc = (int)syscall_debug3(DEBUG_HANDLE_COUNT, 0, 0);
        write_count_file("/spawn_handles_with", hc);
        syscall_exit(0);
    }
    if (role_n > 0 && my_streq(role, "BASELINE")) {   /* defensive */
        int hc = (int)syscall_debug3(DEBUG_HANDLE_COUNT, 0, 0);
        write_count_file("/spawn_handles_baseline", hc);
        syscall_exit(0);
    }
    /* role absent/empty -> fork-bomb guard: if the genuine parent already set
     * /.shi_guard, we are a confused child whose role read flaked -> exit. */
    {
        char g[8];
        int gn = read_text_file("/.shi_guard", g, sizeof(g));
        if (gn > 0 && g[0] == 'R') syscall_exit(0);
    }

    /* ---- genuine parent ---- */
    tap_plan(5);

    delete_file_by_truncate("/spawn_handles_baseline");
    delete_file_by_truncate("/spawn_handles_with");
    delete_file_by_truncate("/.spawn_handles_role");
    delete_file_by_truncate("/.shi_guard");
    (void)write_text_file("/.shi_guard", "RUN", 3);   /* best-effort guard */

    cap_token_raw_t tok1 = syscall_debug_cap_create_with_flags(CAP_FLAG_PUBLIC);
    if (tok1 == 0) printf("# tok1 create failed\n");
    TAP_ASSERT(tok1 != 0, "1. created cap tok1 via DEBUG_CAP_CREATE_WITH_FLAGS");

    cap_token_raw_t tok2 = syscall_debug_cap_create_with_flags(CAP_FLAG_PUBLIC);
    if (tok2 == 0) printf("# tok2 create failed\n");
    TAP_ASSERT(tok2 != 0, "2. created cap tok2 via DEBUG_CAP_CREATE_WITH_FLAGS");

    /* 3. Baseline spawn via SYS_SPAWN_ARGV (argv role marker — flake-proof). */
    char *bargv[2] = { (char *)"bin/tests/spawn_handles_inherit.tap",
                       (char *)"BASELINE" };
    int pid_base = syscall_spawn_argv("bin/tests/spawn_handles_inherit.tap", 2, bargv);
    int status_base = -1;
    if (pid_base > 0) (void)syscall_wait(&status_base);
    int baseline_count = read_count_file("/spawn_handles_baseline");
    if (pid_base <= 0 || status_base != 0 || baseline_count < 0) {
        printf("# pid_base=%d status=%d baseline_count=%d\n",
               pid_base, status_base, baseline_count);
    }
    TAP_ASSERT(pid_base > 0 && status_base == 0 && baseline_count >= 0,
               "3. baseline spawn (no handles_to_inherit) records valid count");

    /* 4. Inherit spawn: SYS_SPAWN_EX with handles_to_inherit=[tok1,tok2].
     *    Stage the WITH role with a VERIFIED write; if it can't be staged
     *    (FU24.A flake) SKIP the spawn so we never spawn against an empty
     *    sentinel (which would fork-bomb). */
    int with_ok = 0, pid_with = -1, status_with = -1, inherit_count = -1;
    if (write_text_file("/.spawn_handles_role", "WITH", 4) == 0) {
        spawn_rlimits_t attrs = {0};
        attrs.flags = SPAWN_ATTR_HAS_HANDLES_U;
        attrs.handles_to_inherit[0] = tok1;
        attrs.handles_to_inherit[1] = tok2;
        attrs.n_handles_to_inherit  = 2;
        pid_with = syscall_spawn_ex("bin/tests/spawn_handles_inherit.tap", &attrs);
        if (pid_with > 0) (void)syscall_wait(&status_with);
        inherit_count = read_count_file("/spawn_handles_with");
        with_ok = (pid_with > 0 && status_with == 0 && inherit_count >= 0);
    } else {
        printf("# WITH role sentinel could not be staged (FU24.A flake) — skip spawn\n");
    }
    if (!with_ok) {
        printf("# pid_with=%d status=%d inherit_count=%d\n",
               pid_with, status_with, inherit_count);
    }
    TAP_ASSERT(with_ok,
               "4. inherit spawn (with handles_to_inherit) records valid count");

    /* 5. The CORE assertion: inherit_count == baseline_count + 2. */
    if (baseline_count >= 0 && inherit_count >= 0 &&
        inherit_count != baseline_count + 2) {
        printf("# baseline_count=%d inherit_count=%d (expected baseline+2=%d)\n",
               baseline_count, inherit_count, baseline_count + 2);
    }
    TAP_ASSERT(baseline_count >= 0 && inherit_count >= 0 &&
               inherit_count == baseline_count + 2,
               "5. handles_to_inherit added exactly 2 entries to child's handle table");

    delete_file_by_truncate("/.shi_guard");
    tap_done();
    syscall_exit(0);
}
