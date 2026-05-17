// user/tests/soak_inject_apply.c
//
// Phase 28 Session G.2 — soak harness fault-injection front-end.
//
// Reads /etc/soak_inject.conf (a single-line `key=value` file written by
// scripts/run_soak.sh between iterations) and applies the requested
// injection via DEBUG_INJECT_* subops.  The rest of the gate then runs
// with the hook armed.
//
// Supported keys:
//   pmm=N          set g_debug_pmm_fail_nth = N
//   kmalloc=N      set g_debug_kmalloc_fail_nth = N
//   chan_rate=R    set g_debug_chan_send_fail_rate = R
//   spin_rate=R    set g_debug_spinlock_timeout_rate = R
//
// Absent file or unknown key  => no-op (test passes silently).  The
// test always reports two asserts so the soak summary can sanity-check
// that the driver actually ran.

#include "../libtap.h"
#include "../syscalls.h"
#include <string.h>

extern int printf(const char *fmt, ...);

static int parse_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

void _start(void) {
    tap_plan(2);

    // First reset everything to a known-clean state.  This ensures iter
    // N's injection does not bleed into iter N+1 (the harness only
    // writes a conf file when it wants injection — but reset is cheap).
    syscall_debug_inject_reset_all();

    int fd = syscall_open("/etc/soak_inject.conf");
    if (fd < 0) {
        // No injection requested this iter.
        tap_ok("1. /etc/soak_inject.conf absent — no injection");
        tap_ok("2. counters reset");
        tap_done();
        syscall_exit(0);
    }
    TAP_ASSERT(fd >= 0, "1. opened /etc/soak_inject.conf");

    char buf[64] = {0};
    ssize_t n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n <= 0) {
        tap_ok("2. empty conf — counters stay reset");
        tap_done();
        syscall_exit(0);
    }

    // Trim trailing newline.
    if (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[n-1] = '\0';

    // Parse "key=value".
    char *eq = strchr(buf, '=');
    if (!eq) {
        printf("# soak_inject_apply: malformed conf '%s'\n", buf);
        tap_not_ok("2. counters applied", "malformed conf");
        tap_done();
        syscall_exit(1);
    }
    *eq = '\0';
    const char *key = buf;
    const char *val = eq + 1;
    int v = parse_int(val);

    long rc = 0;
    if (strcmp(key, "pmm") == 0) {
        rc = syscall_debug_inject_pmm_fail_nth(v);
    } else if (strcmp(key, "kmalloc") == 0) {
        rc = syscall_debug_inject_kmalloc_fail_nth(v);
    } else if (strcmp(key, "chan_rate") == 0) {
        rc = syscall_debug_inject_chan_send_fail_rate((uint32_t)v);
    } else if (strcmp(key, "spin_rate") == 0) {
        rc = syscall_debug_inject_spinlock_timeout_rate((uint32_t)v);
    } else {
        printf("# soak_inject_apply: unknown key '%s'\n", key);
        rc = -1;
    }
    printf("# soak_inject_apply: %s=%d rc=%ld\n", key, v, rc);
    TAP_ASSERT(rc == 0, "2. inject SYS_DEBUG call succeeded");

    tap_done();
    syscall_exit(0);
}
