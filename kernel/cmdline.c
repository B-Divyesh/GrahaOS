// kernel/cmdline.c
// Phase 12: kernel command-line parser.
// Supports: autorun=<name>, quiet[=0|1], test_timeout_seconds=<uint>.
// Unknown or malformed tokens are logged via serial and ignored.

#include "cmdline.h"
#include "../arch/x86_64/drivers/serial/serial.h"
#include <stddef.h>
#include "log.h"

// Phase 22 Stage F: strcmp shim moved from kernel/net/klib.h (deleted) into
// kernel/main.c.  Forward-declare here so the deleted include doesn't bite.
extern int strcmp(const char *s1, const char *s2);

#define CMDLINE_BUF_BYTES         256
#define TIMEOUT_DEFAULT_SECONDS   60u
#define TIMEOUT_MAX_SECONDS       86400u   // 24 hours

static char g_cmdline_buf[CMDLINE_BUF_BYTES];

cmdline_flags_t g_cmdline_flags = {
    .autorun              = NULL,
    .quiet                = false,
    .test_timeout_seconds = TIMEOUT_DEFAULT_SECONDS,
    .inject_klog_preinit  = 0,
    .inject_ring_wrap     = 0,
    .klog_mirror          = -1,
};

// Copy up to dst_cap-1 bytes from src to dst; NUL-terminate. Returns
// actual length copied.
static size_t cmdline_strlcpy(char *dst, const char *src, size_t dst_cap) {
    size_t n = 0;
    if (dst_cap == 0) return 0;
    while (n < dst_cap - 1 && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}

static bool has_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        s++;
        prefix++;
    }
    return true;
}

// Parse a decimal unsigned 32-bit integer from `s`. On success, stores
// value in *out and returns true. Returns false on empty or non-digit
// input, or on overflow beyond TIMEOUT_MAX_SECONDS.
static bool parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > (uint64_t)TIMEOUT_MAX_SECONDS) return false;
        s++;
    }
    *out = (uint32_t)v;
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static void log_skip(const char *token) {
    klog(KLOG_INFO, SUBSYS_CORE, "cmdline: ignoring '%s'", token);
}

static void handle_token(char *tok) {
    if (has_prefix(tok, "autorun=")) {
        const char *val = tok + 8;
        if (!*val) {
            log_skip(tok);
            return;
        }
        g_cmdline_flags.autorun = val;   // pointer into g_cmdline_buf
        return;
    }

    if (strcmp(tok, "quiet") == 0) {
        g_cmdline_flags.quiet = true;
        return;
    }
    if (has_prefix(tok, "quiet=")) {
        const char *val = tok + 6;
        if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0) {
            g_cmdline_flags.quiet = true;
        } else if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0) {
            g_cmdline_flags.quiet = false;
        } else {
            log_skip(tok);
        }
        return;
    }

    if (has_prefix(tok, "test_timeout_seconds=")) {
        const char *val = tok + 21;
        uint32_t v = 0;
        if (parse_u32(val, &v)) {
            g_cmdline_flags.test_timeout_seconds = v;
        } else {
            log_skip(tok);
        }
        return;
    }

    // Phase 13 fault-injection knobs.
    if (has_prefix(tok, "inject_klog_preinit=")) {
        const char *val = tok + 20;
        uint32_t v = 0;
        if (parse_u32(val, &v)) {
            g_cmdline_flags.inject_klog_preinit = v;
        } else {
            log_skip(tok);
        }
        return;
    }
    if (has_prefix(tok, "inject_ring_wrap=")) {
        const char *val = tok + 17;
        uint32_t v = 0;
        if (parse_u32(val, &v)) {
            g_cmdline_flags.inject_ring_wrap = v;
        } else {
            log_skip(tok);
        }
        return;
    }
    if (has_prefix(tok, "klog_mirror=")) {
        const char *val = tok + 12;
        if (strcmp(val, "0") == 0)      { g_cmdline_flags.klog_mirror = 0; }
        else if (strcmp(val, "1") == 0) { g_cmdline_flags.klog_mirror = 1; }
        else                            { log_skip(tok); }
        return;
    }

    // Bootloader / Limine often pass their own tokens we don't care
    // about (e.g., LIMINE-managed options). Log but don't fail boot.
    log_skip(tok);
}

void cmdline_parse(const char *raw) {
    // Reset to defaults every call (makes reinit / tests deterministic).
    g_cmdline_flags.autorun              = NULL;
    g_cmdline_flags.quiet                = false;
    g_cmdline_flags.test_timeout_seconds = TIMEOUT_DEFAULT_SECONDS;

    if (!raw) return;
    if (!*raw) return;

    cmdline_strlcpy(g_cmdline_buf, raw, CMDLINE_BUF_BYTES);

    // In-place tokenise on whitespace.
    char *p = g_cmdline_buf;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        char *tok = p;
        while (*p && !is_space(*p)) p++;
        if (*p) { *p = '\0'; p++; }
        handle_token(tok);
    }

    // Single consolidated klog so the cmdline summary lands as one
    // line (vs the earlier serial_write fragments).
    klog(KLOG_INFO, SUBSYS_CORE,
         "cmdline: autorun=%s quiet=%s test_timeout_seconds=%lu"
         " inject_klog_preinit=%lu inject_ring_wrap=%lu klog_mirror=%d",
         g_cmdline_flags.autorun ? g_cmdline_flags.autorun : "(none)",
         g_cmdline_flags.quiet ? "1" : "0",
         (unsigned long)g_cmdline_flags.test_timeout_seconds,
         (unsigned long)g_cmdline_flags.inject_klog_preinit,
         (unsigned long)g_cmdline_flags.inject_ring_wrap,
         (int)g_cmdline_flags.klog_mirror);
}
