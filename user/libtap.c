// user/libtap.c
// Phase 12: TAP 1.4 producer implementation.
//
// Every call routes through printf (SYS_PUTC path) for byte-by-byte
// serial writes. No internal buffering — this is deliberate: the 16550
// FIFO on the QEMU serial port is 16 bytes, and we want every TAP line
// to hit the host log even if the kernel shuts down immediately after.

#include "libtap.h"
#include <stdio.h>
#include <stdlib.h>

static int s_planned = 0;
static int s_counter = 0;
static int s_passed  = 0;
static int s_failed  = 0;

void tap_plan(int n) {
    s_planned = n;
    s_counter = 0;
    s_passed  = 0;
    s_failed  = 0;
    printf("1..%d\n", n);
}

void tap_ok(const char *name) {
    s_counter++;
    s_passed++;
    printf("ok %d - %s\n", s_counter, name ? name : "");
}

void tap_not_ok(const char *name, const char *reason) {
    s_counter++;
    s_failed++;
    printf("not ok %d - %s\n", s_counter, name ? name : "");
    if (reason && *reason) {
        printf("# %s\n", reason);
    }
}

void tap_skip(const char *name, const char *reason) {
    s_counter++;
    s_passed++;  // SKIPs count as passes per TAP 1.4
    printf("ok %d - %s # SKIP %s\n",
           s_counter,
           name ? name : "",
           reason ? reason : "");
}

void tap_bail_out(const char *reason) {
    printf("Bail out! %s\n", reason ? reason : "");
    exit(77);
}

void tap_done(void) {
    printf("# passed %d/%d", s_passed, s_planned > 0 ? s_planned : s_counter);
    if (s_failed > 0) {
        printf(" (%d failed)", s_failed);
    }
    printf("\n");
}

int tap_get_planned(void) { return s_planned; }
int tap_get_passed(void)  { return s_passed;  }
int tap_get_failed(void)  { return s_failed;  }
