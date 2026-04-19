// user/tests/klog_basic.c
// Phase 13 gate test: round-trips SYS_KLOG_WRITE / SYS_KLOG_READ with
// the same input shapes the spec calls out:
//   - FATAL must be rejected for user-space writers.
//   - subsys < 10 must be rejected.
//   - level_mask must actually filter.
//   - tail_count must return (at most) that many of our entries.
//   - Pathological inputs (empty message, NULL message + len=0) must
//     succeed without tripping the kernel.
//
// The ring is shared across the whole boot, so we uniquify each write
// with a marker string we can grep for — that way other kernel logs
// emitted concurrently don't fool assertions.

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// The spec's klog_entry_t is 256 bytes; sanity-check the layout
// the kernel exposes to user-space via SYS_KLOG_READ.
_Static_assert(sizeof(klog_entry_t) == 256, "klog_entry_t ABI drifted");

// Temp read buffer. Allocated in BSS to keep the test binary small.
static klog_entry_t s_buf[64];

// Find the last entry in `s_buf` whose message contains `needle`.
// Returns -1 if not present.
static int find_marker(int count, const char *needle) {
    for (int i = count - 1; i >= 0; i--) {
        if (strstr(s_buf[i].message, needle) != NULL) {
            return i;
        }
    }
    return -1;
}

static int count_markers(int count, const char *needle) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (strstr(s_buf[i].message, needle) != NULL) n++;
    }
    return n;
}

static void test_rejections(void) {
    // Phase 15b: first user subsys is now 11 (10 is AUDIT).
    int r = syscall_klog_write(KLOG_INFO, 11, "KB_REJ_VALID", 12);
    TAP_ASSERT(r == 0, "valid write accepted (INFO, subsys=11)");

    // FATAL from userspace must be refused — panics belong to the kernel.
    r = syscall_klog_write(KLOG_FATAL, 11, "KB_REJ_FATAL", 12);
    TAP_ASSERT(r == -1, "KLOG_FATAL rejected from userspace");

    // Subsys 9 is TEST — reserved for the kernel test subsystem.
    r = syscall_klog_write(KLOG_INFO, 9, "KB_REJ_SUB9", 11);
    TAP_ASSERT(r == -1, "subsys=9 rejected (kernel TEST)");

    // Subsys 10 is AUDIT (Phase 15b) — reserved for the kernel audit subsystem.
    r = syscall_klog_write(KLOG_INFO, 10, "KB_REJ_SUB10", 12);
    TAP_ASSERT(r == -1, "subsys=10 rejected (kernel AUDIT)");

    // Subsys 0 is CORE — reserved.
    r = syscall_klog_write(KLOG_INFO, 0, "KB_REJ_SUB0", 11);
    TAP_ASSERT(r == -1, "subsys=0 rejected (kernel CORE)");
}

static void test_roundtrip(void) {
    const char *marker = "KB_RT_abcd123";
    int r = syscall_klog_write(KLOG_INFO, 100, marker, (uint32_t)strlen(marker));
    TAP_ASSERT(r == 0, "write with subsys=100 succeeded");

    int n = syscall_klog_read(0, 16, s_buf, sizeof(s_buf));
    TAP_ASSERT(n > 0, "klog_read returned at least one entry");

    int idx = find_marker(n, marker);
    TAP_ASSERT(idx >= 0, "round-trip: our marker appears in the ring");
    if (idx >= 0) {
        // Entry fields should carry through verbatim.
        TAP_ASSERT((s_buf[idx].level & KLOG_LEVEL_MASK) == KLOG_INFO,
                   "round-trip: entry level is INFO");
        TAP_ASSERT(s_buf[idx].subsystem_id == 100,
                   "round-trip: entry subsys is 100");
    } else {
        tap_not_ok("round-trip: entry level is INFO", "marker missing");
        tap_not_ok("round-trip: entry subsys is 100", "marker missing");
    }
}

static void test_level_filter(void) {
    const char *info_marker = "KB_LVLF_INFO_xyz";
    const char *warn_marker = "KB_LVLF_WARN_xyz";
    syscall_klog_write(KLOG_INFO, 101, info_marker, (uint32_t)strlen(info_marker));
    syscall_klog_write(KLOG_WARN, 101, warn_marker, (uint32_t)strlen(warn_marker));

    // Level mask = only WARN (bit 3).
    int n = syscall_klog_read(1u << KLOG_WARN, 0, s_buf, sizeof(s_buf));
    TAP_ASSERT(n >= 1, "level filter returned at least one WARN entry");

    int info_hits = count_markers(n, info_marker);
    int warn_hits = count_markers(n, warn_marker);
    TAP_ASSERT(info_hits == 0, "level filter excludes INFO marker");
    TAP_ASSERT(warn_hits >= 1, "level filter includes WARN marker");
}

static void test_tail_count(void) {
    // Write five markers we can identify. The ring may contain other
    // kernel entries in between, so we read back a generous tail and
    // assert that all five appear.
    const char *markers[] = {
        "KB_TAIL_1xxx", "KB_TAIL_2xxx", "KB_TAIL_3xxx",
        "KB_TAIL_4xxx", "KB_TAIL_5xxx",
    };
    for (int i = 0; i < 5; i++) {
        syscall_klog_write(KLOG_INFO, 102, markers[i],
                           (uint32_t)strlen(markers[i]));
    }

    int n = syscall_klog_read(0, 32, s_buf, sizeof(s_buf));
    TAP_ASSERT(n > 0, "tail read returned entries");

    int found = 0;
    for (int i = 0; i < 5; i++) {
        if (find_marker(n, markers[i]) >= 0) found++;
    }
    TAP_ASSERT(found == 5, "tail=32 captured all 5 distinct markers");
}

static void test_truncation_and_empty(void) {
    // A 250-byte message should be truncated to fit in the 223-byte
    // payload (plus null terminator = 224). We should still succeed.
    char long_msg[251];
    for (int i = 0; i < 250; i++) long_msg[i] = 'A' + (char)(i % 26);
    long_msg[250] = '\0';
    // Mark the first bytes so we can find this entry.
    const char *trunc_marker = "KB_TRUNC_head";
    for (int i = 0; i < 13; i++) long_msg[i] = trunc_marker[i];

    int r = syscall_klog_write(KLOG_INFO, 103, long_msg, 250);
    TAP_ASSERT(r == 0, "oversize message (250 B) accepted");

    int n = syscall_klog_read(0, 8, s_buf, sizeof(s_buf));
    int idx = find_marker(n, trunc_marker);
    TAP_ASSERT(idx >= 0 && strlen(s_buf[idx].message) < KLOG_MSG_LEN,
               "oversize message truncated to < 224 bytes");

    // Empty message path: len=0 with NULL pointer should still succeed,
    // since the kernel only dereferences when len > 0.
    r = syscall_klog_write(KLOG_INFO, 104, NULL, 0);
    TAP_ASSERT(r == 0, "empty write (NULL msg, len=0) accepted");
}

void _start(void) {
    printf("=== klog_basic: Phase 13 klog syscall tests ===\n");

    // 4 rejections + 4 round-trip + 4 level-filter + 2 tail + 3 trunc/empty = 17.
    // Spec mandates ≥10 so we have margin.
    tap_plan(18);

    test_rejections();
    test_roundtrip();
    test_level_filter();
    test_tail_count();
    test_truncation_and_empty();

    tap_done();
    exit(0);
}
