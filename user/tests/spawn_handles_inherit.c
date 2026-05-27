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

static void write_text_file(const char *path, const char *text, int len) {
    (void)syscall_create(path, 0);
    int fd = syscall_open(path);
    if (fd < 0) return;
    (void)syscall_truncate(fd);
    (void)syscall_write(fd, text, (size_t)len);
    syscall_close(fd);
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

void _start(void) {
    /* Sentinel-based role detection (matches grahai's --txn pattern).
     * Parent creates /.spawn_handles_role with "BASELINE" or "WITH"
     * before each spawn; child reads it on entry. */
    char role[32];
    int role_n = read_text_file("/.spawn_handles_role", role, sizeof(role));
    if (role_n > 0) {
        /* Helper mode. */
        int hc = (int)syscall_debug3(DEBUG_HANDLE_COUNT, 0, 0);
        if (my_streq(role, "WITH")) {
            write_count_file("/spawn_handles_with", hc);
        } else {
            write_count_file("/spawn_handles_baseline", hc);
        }
        syscall_exit(0);
    }

    /* Parent mode. */
    tap_plan(5);

    /* Clean up any prior run output. */
    delete_file_by_truncate("/spawn_handles_baseline");
    delete_file_by_truncate("/spawn_handles_with");
    delete_file_by_truncate("/.spawn_handles_role");

    /* 1+2. Create 2 caps via the debug helper.  We use INHERITABLE
     * (not PUBLIC) — INHERITABLE means the FU26.D cap-inheritance walk
     * in sched_create_user_process would normally pick these up too.
     * Since we want to PROVE handles_to_inherit ALSO works, we accept
     * the baseline count already counts the FU26.D inheritance walk's
     * effect on both spawns (it runs uniformly).  handles_to_inherit
     * just adds 2 more on top. */
    cap_token_raw_t tok1 = syscall_debug_cap_create_with_flags(CAP_FLAG_PUBLIC);
    if (tok1 == 0) printf("# tok1 create failed\n");
    TAP_ASSERT(tok1 != 0, "1. created cap tok1 via DEBUG_CAP_CREATE_WITH_FLAGS");

    cap_token_raw_t tok2 = syscall_debug_cap_create_with_flags(CAP_FLAG_PUBLIC);
    if (tok2 == 0) printf("# tok2 create failed\n");
    TAP_ASSERT(tok2 != 0, "2. created cap tok2 via DEBUG_CAP_CREATE_WITH_FLAGS");

    /* 3. Baseline spawn: child runs in helper mode and writes its
     * handle count to /spawn_handles_baseline. */
    write_text_file("/.spawn_handles_role", "BASELINE", 8);
    int pid_base = syscall_spawn("bin/tests/spawn_handles_inherit.tap");
    int status_base = -1;
    if (pid_base > 0) (void)syscall_wait(&status_base);
    delete_file_by_truncate("/.spawn_handles_role");
    int baseline_count = read_count_file("/spawn_handles_baseline");
    if (pid_base <= 0 || status_base != 0 || baseline_count < 0) {
        printf("# pid_base=%d status=%d baseline_count=%d\n",
               pid_base, status_base, baseline_count);
    }
    TAP_ASSERT(pid_base > 0 && status_base == 0 && baseline_count >= 0,
               "3. baseline spawn (no handles_to_inherit) records valid count");

    /* 4. Inherit spawn: SYS_SPAWN_EX with handles_to_inherit=[tok1,tok2]. */
    write_text_file("/.spawn_handles_role", "WITH", 4);

    spawn_rlimits_t attrs = {0};
    attrs.flags = SPAWN_ATTR_HAS_HANDLES_U;
    attrs.handles_to_inherit[0] = tok1;
    attrs.handles_to_inherit[1] = tok2;
    attrs.n_handles_to_inherit  = 2;

    int pid_with = syscall_spawn_ex("bin/tests/spawn_handles_inherit.tap",
                                     &attrs);
    int status_with = -1;
    if (pid_with > 0) (void)syscall_wait(&status_with);
    delete_file_by_truncate("/.spawn_handles_role");
    int inherit_count = read_count_file("/spawn_handles_with");
    if (pid_with <= 0 || status_with != 0 || inherit_count < 0) {
        printf("# pid_with=%d status=%d inherit_count=%d\n",
               pid_with, status_with, inherit_count);
    }
    TAP_ASSERT(pid_with > 0 && status_with == 0 && inherit_count >= 0,
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

    tap_done();
    syscall_exit(0);
}
