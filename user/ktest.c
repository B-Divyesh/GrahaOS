// user/ktest.c
// Phase 12: TAP test runner. Two modes:
//
//   1. autorun mode (PID 1): discover tests from bin/tests/manifest.txt,
//      spawn each in sequence, wrap output in `# TAP BEGIN/END`
//      markers, emit `# TAP DONE`, and exit. The kernel then shuts
//      QEMU down via ACPI (autorun_on_init_exit → kernel_shutdown).
//
//   2. interactive mode (spawned by gash): same behaviour but gash's
//      `ktest` builtin usually short-circuits to spawning a single
//      test directly. Running /bin/ktest by hand still runs the
//      whole suite.
//
// Hang protection: per-test SIGKILL was attempted in an earlier draft
// but the kernel's SYS_WAIT blocks the caller at the kernel level
// (returns -99 to userspace only as a wake-up signal), so userspace
// polling cannot detect a timeout while blocked. Without a new
// syscall (forbidden by Phase 12 spec), the per-test deadline cannot
// be enforced from ktest. The kernel watchdog (test_timeout_seconds,
// default 90 s, armed at init spawn) remains the hang backstop: a
// single hung test will consume the watchdog budget and trigger
// TEST_TIMEOUT kernel_shutdown, which parse_tap.py reports as an
// incomplete gate failure. Deferring a true per-test SIGKILL to
// Phase 13 (when structured logging adds SYS_WAIT_TIMEOUT).

#include "syscalls.h"
#include "libtap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define MANIFEST_PATH      "bin/tests/manifest.txt"
#define TEST_DIR_PREFIX    "bin/tests/"
#define TEST_EXT           ".tap"
#define MAX_TESTS          64
#define MAX_NAME           48
#define READ_BUF_BYTES     4096

static char s_names[MAX_TESTS][MAX_NAME];
static int  s_name_count = 0;

static int s_passed_tests = 0;
static int s_failed_tests = 0;
static int s_total_tests  = 0;

// Append one manifest line (strip comments / blanks / trailing \r).
static void accept_line(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return;
    if (line[i] == '#') return;
    size_t j = len;
    while (j > i && (line[j-1] == ' ' || line[j-1] == '\t' ||
                     line[j-1] == '\r' || line[j-1] == '\n')) {
        j--;
    }
    if (j <= i) return;
    size_t name_len = j - i;
    if (name_len >= MAX_NAME) name_len = MAX_NAME - 1;
    if (s_name_count >= MAX_TESTS) return;
    memcpy(s_names[s_name_count], line + i, name_len);
    s_names[s_name_count][name_len] = '\0';
    s_name_count++;
}

static int load_manifest(void) {
    int fd = syscall_open(MANIFEST_PATH);
    if (fd < 0) {
        printf("ktest: cannot open %s\n", MANIFEST_PATH);
        return -1;
    }
    static char buf[READ_BUF_BYTES];
    ssize_t n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n <= 0) {
        printf("ktest: manifest is empty\n");
        return -1;
    }
    buf[n] = '\0';
    size_t start = 0;
    for (size_t p = 0; p < (size_t)n; p++) {
        if (buf[p] == '\n') {
            accept_line(buf + start, p - start);
            start = p + 1;
        }
    }
    if (start < (size_t)n) accept_line(buf + start, (size_t)n - start);
    return s_name_count;
}

static size_t build_test_path(char *out, size_t cap, const char *name) {
    size_t i = 0;
    const char *prefix = TEST_DIR_PREFIX;
    while (prefix[i] && i < cap - 1) { out[i] = prefix[i]; i++; }
    size_t j = 0;
    while (name[j] && i < cap - 1) { out[i++] = name[j++]; }
    const char *ext = TEST_EXT;
    size_t k = 0;
    while (ext[k] && i < cap - 1) { out[i++] = ext[k++]; }
    out[i] = '\0';
    return i;
}

static int run_one_test(const char *name) {
    char path[96];
    build_test_path(path, sizeof(path), name);

    printf("# TAP BEGIN %s\n", name);

    int pid = syscall_spawn(path);
    if (pid < 0) {
        printf("# spawn failed: %s\n", path);
        printf("# TAP END %s\n", name);
        printf("# exit=-1\n");
        printf("                \n");
        return -1;
    }

    int status = 0;
    (void)syscall_wait(&status);

    printf("# TAP END %s\n", name);
    printf("# exit=%d\n", status);
    printf("                \n");  // FIFO flush
    return status;
}

void _start(void) {
    int pid = syscall_getpid();
    printf("# ktest starting, pid=%d\n", pid);

    int n = load_manifest();
    if (n <= 0) {
        printf("# TAP DONE\n");
        exit(1);
    }
    s_total_tests = n;
    printf("# ktest: %d test(s) discovered\n", n);

    for (int i = 0; i < n; i++) {
        int st = run_one_test(s_names[i]);
        if (st == 0) s_passed_tests++;
        else         s_failed_tests++;
    }

    printf("# ktest: %d passed, %d failed (of %d)\n",
           s_passed_tests, s_failed_tests, s_total_tests);
    printf("# TAP DONE\n");
    printf("                \n");
    exit(0);
}
