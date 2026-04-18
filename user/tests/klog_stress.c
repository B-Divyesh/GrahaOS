// user/tests/klog_stress.c
// Phase 13 stress gate test. Writes a large batch of klog entries
// via SYS_KLOG_WRITE, then reads back via SYS_KLOG_READ and asserts
// the ring kept up — every marker landed and seq numbers stayed
// strictly monotonic.
//
// Sizing: STRESS_N=2000 entries, well below the 16384-entry ring
// capacity so we can read the whole batch back in one shot. The
// kernel spinlock is exercised by interleaved kernel klog calls
// (timer ticks, indexer task, etc.) — pure single-process from
// user-space won't race itself, but it does prove the ring
// preserves order under interleaved writers.
//
// Each marker carries a sequence index so we can verify *order* in
// addition to *presence*.
//
// 4 fixed asserts per gate test plan; we deliberately do NOT emit
// "ok N - entry K" per entry because that would flood TAP output
// and turn the harness into the bottleneck.

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STRESS_N        2000
#define STRESS_SUBSYS   200
#define MARKER_PREFIX   "KSTRESS_"

static klog_entry_t s_buf[STRESS_N + 64];

static int prefix_match(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

// Parse the integer suffix from "KSTRESS_<N>". Returns -1 on miss.
static int parse_marker_idx(const char *msg) {
    if (!prefix_match(msg, MARKER_PREFIX)) return -1;
    const char *p = msg + 8;  // strlen("KSTRESS_")
    int v = 0;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

void _start(void) {
    printf("=== klog_stress: writing %d entries ===\n", STRESS_N);

    tap_plan(4);

    // Phase 1: bulk writes. Track any rejection.
    int rejects = 0;
    for (int i = 0; i < STRESS_N; i++) {
        char msg[24];
        // Inline format — printf goes through SYS_PUTC which is slow
        // and would dominate the run-time. We hand-format.
        int p = 0;
        msg[p++] = 'K'; msg[p++] = 'S'; msg[p++] = 'T'; msg[p++] = 'R';
        msg[p++] = 'E'; msg[p++] = 'S'; msg[p++] = 'S'; msg[p++] = '_';
        // print integer i
        char tmp[12]; int n = 0;
        int v = i;
        if (v == 0) tmp[n++] = '0';
        while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
        while (n > 0) msg[p++] = tmp[--n];
        msg[p] = '\0';
        if (syscall_klog_write(KLOG_INFO, STRESS_SUBSYS, msg, (uint32_t)p) != 0) {
            rejects++;
        }
    }
    TAP_ASSERT(rejects == 0, "all bulk writes accepted");

    // Phase 2: bulk read. Get the most recent N + slack so kernel
    // chatter (timer/indexer) interleaved between our writes won't
    // push our markers out of view.
    int n = syscall_klog_read(0, STRESS_N + 60, s_buf, sizeof(s_buf));
    TAP_ASSERT(n > 0, "klog_read returned entries");

    // Phase 3: count markers and verify monotonic seq for our entries.
    int found = 0;
    int prev_idx = -1;
    int prev_seq = -1;
    int order_ok = 1;
    int seq_ok = 1;
    for (int i = 0; i < n; i++) {
        int idx = parse_marker_idx(s_buf[i].message);
        if (idx < 0) continue;
        if (s_buf[i].subsystem_id != STRESS_SUBSYS) continue;
        found++;
        if (prev_idx >= 0) {
            // Marker indices must increase in their original write
            // order (we assigned them in the for-loop).
            if (idx <= prev_idx) order_ok = 0;
            // seq numbers between successive markers must strictly
            // increase too — enforced by the kernel's atomic next_seq.
            if ((int)s_buf[i].seq <= prev_seq) seq_ok = 0;
        }
        prev_idx = idx;
        prev_seq = (int)s_buf[i].seq;
    }
    printf("# klog_stress: found %d / %d markers in ring\n", found, STRESS_N);

    TAP_ASSERT(found == STRESS_N, "every marker preserved across writes");
    TAP_ASSERT(order_ok && seq_ok,
               "marker order and seq monotonicity preserved");

    tap_done();
    exit(0);
}
