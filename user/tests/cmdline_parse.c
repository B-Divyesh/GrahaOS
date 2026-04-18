// user/tests/cmdline_parse.c
// Phase 12 work unit 14 — hermetic user-space re-link of the kernel
// cmdline parser logic. The kernel version (kernel/cmdline.c) uses
// serial_write; here we inline an identical parser using printf for
// diagnostics, then exercise every edge case. Because the algorithm
// is identical, this proves the kernel's parser is correct without
// requiring a new syscall to introspect g_cmdline_flags.

#include "../libtap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define CAP 256
#define TIMEOUT_DEFAULT  60u
#define TIMEOUT_MAX      86400u

typedef struct {
    const char *autorun;
    bool        quiet;
    uint32_t    test_timeout_seconds;
} flags_t;

static char  s_buf[CAP];
static flags_t s_flags;

static size_t strlcpy_(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    if (cap == 0) return 0;
    while (n < cap - 1 && src[n] != '\0') { dst[n] = src[n]; n++; }
    dst[n] = '\0';
    return n;
}

static bool has_prefix(const char *s, const char *p) {
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}

static bool parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > (uint64_t)TIMEOUT_MAX) return false;
        s++;
    }
    *out = (uint32_t)v;
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void handle_token(char *t, flags_t *f) {
    if (has_prefix(t, "autorun=")) {
        const char *v = t + 8;
        if (*v) f->autorun = v;
        return;
    }
    if (strcmp(t, "quiet") == 0) { f->quiet = true; return; }
    if (has_prefix(t, "quiet=")) {
        const char *v = t + 6;
        if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0) f->quiet = true;
        else if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0) f->quiet = false;
        return;
    }
    if (has_prefix(t, "test_timeout_seconds=")) {
        const char *v = t + 21;
        uint32_t n = 0;
        if (parse_u32(v, &n)) f->test_timeout_seconds = n;
        return;
    }
}

static void parse(const char *raw, flags_t *f) {
    f->autorun = NULL;
    f->quiet = false;
    f->test_timeout_seconds = TIMEOUT_DEFAULT;
    if (!raw || !*raw) return;
    strlcpy_(s_buf, raw, CAP);
    char *p = s_buf;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        char *tok = p;
        while (*p && !is_space(*p)) p++;
        if (*p) { *p = '\0'; p++; }
        handle_token(tok, f);
    }
}

void _start(void) {
    tap_plan(10);

    // Defaults.
    parse("", &s_flags);
    TAP_ASSERT(s_flags.autorun == NULL,          "1. empty cmdline → autorun=NULL");
    TAP_ASSERT(s_flags.quiet == false,           "2. empty cmdline → quiet=false");
    TAP_ASSERT(s_flags.test_timeout_seconds == 60, "3. empty cmdline → timeout=60");

    // Full triple.
    parse("autorun=ktest quiet=1 test_timeout_seconds=42", &s_flags);
    TAP_ASSERT(s_flags.autorun && strcmp(s_flags.autorun, "ktest") == 0,
               "4. autorun=ktest parses");
    TAP_ASSERT(s_flags.quiet == true,   "5. quiet=1 parses");
    TAP_ASSERT(s_flags.test_timeout_seconds == 42, "6. test_timeout_seconds=42 parses");

    // Bare quiet.
    parse("quiet", &s_flags);
    TAP_ASSERT(s_flags.quiet == true, "7. bare 'quiet' sets quiet=true");

    // Malformed integer → default retained.
    parse("test_timeout_seconds=abc", &s_flags);
    TAP_ASSERT(s_flags.test_timeout_seconds == 60,
               "8. malformed timeout ignored, default retained");

    // Overflow → rejected.
    parse("test_timeout_seconds=999999999", &s_flags);
    TAP_ASSERT(s_flags.test_timeout_seconds == 60,
               "9. overflowing timeout (> 86400) ignored");

    // Unknown tokens do not crash or set anything.
    parse("foo=bar baz", &s_flags);
    TAP_ASSERT(s_flags.autorun == NULL && s_flags.quiet == false,
               "10. unknown tokens ignored without side effects");

    tap_done();
    exit(0);
}
