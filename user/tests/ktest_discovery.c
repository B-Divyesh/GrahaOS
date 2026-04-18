// user/tests/ktest_discovery.c
// Phase 12 work unit 14 — gate test for ktest's manifest-driven
// discovery. Opens /bin/tests/manifest.txt directly (same file ktest
// reads), parses it, and asserts:
//   1. File opens OK
//   2. At least one non-comment, non-blank name is present
//   3. Own test name (ktest_discovery) is in the manifest
//   4. No duplicate names
//   5. Every name is a plausible identifier (alnum + underscore only)

#include "../libtap.h"
#include "../syscalls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAMES 64
#define MAX_NAME_LEN 48

static char names[MAX_NAMES][MAX_NAME_LEN];
static int  count = 0;

static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

void _start(void) {
    tap_plan(5);

    int fd = syscall_open("bin/tests/manifest.txt");
    TAP_ASSERT(fd >= 0, "1. manifest.txt opens via initrd fallback");
    if (fd < 0) {
        tap_done();
        exit(0);
    }

    char buf[4096];
    int n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    TAP_ASSERT(n > 0, "2. manifest.txt has content");
    if (n <= 0) {
        tap_done();
        exit(0);
    }
    buf[n] = '\0';

    // Split on newline, skip comments and blanks.
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            int s = start;
            while (s < i && (buf[s] == ' ' || buf[s] == '\t')) s++;
            int e = i;
            while (e > s && (buf[e-1] == ' ' || buf[e-1] == '\t' || buf[e-1] == '\r')) e--;
            if (s < e && buf[s] != '#') {
                int len = e - s;
                if (len >= MAX_NAME_LEN) len = MAX_NAME_LEN - 1;
                if (count < MAX_NAMES) {
                    memcpy(names[count], buf + s, len);
                    names[count][len] = '\0';
                    count++;
                }
            }
            start = i + 1;
        }
    }
    TAP_ASSERT(count >= 1, "3. at least one test name in manifest");

    // Assert this test's own name is in the list.
    int self_found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "ktest_discovery") == 0) {
            self_found = 1;
            break;
        }
    }
    TAP_ASSERT(self_found == 1, "4. own test name (ktest_discovery) in manifest");

    // All names are valid identifiers; no duplicates.
    int ok_all_valid = 1;
    int no_dupes = 1;
    for (int i = 0; i < count; i++) {
        // Validate characters.
        for (int j = 0; names[i][j]; j++) {
            if (!is_ident_char(names[i][j])) {
                ok_all_valid = 0;
                break;
            }
        }
        // Check for duplicates.
        for (int k = i + 1; k < count; k++) {
            if (strcmp(names[i], names[k]) == 0) {
                no_dupes = 0;
                break;
            }
        }
    }
    TAP_ASSERT(ok_all_valid && no_dupes,
               "5. all names valid identifiers, no duplicates");

    tap_done();
    exit(0);
}
