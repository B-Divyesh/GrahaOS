// user/tests/audit_query_since.c
//
// Phase 29 Session C (FU28.E) gate test.
//
// Regression test for audit_query's since_ns scoping. The kernel-side
// filter at kernel/audit.c:1313 ALREADY discards entries with
// ns_timestamp < since_ns; this test demonstrates the filter is wired
// correctly end-to-end (no userspace-side workaround required).
//
// Approach:
//   1. Drain the in-memory ring to obtain a baseline count + a recent
//      timestamp.
//   2. Emit N synthetic PLAN_BEGIN events via DEBUG_AUDIT_EMIT_PLAN.
//   3. Query with since_ns = saved-timestamp — must see >= N new entries.
//   4. Query with since_ns = a guaranteed-future timestamp — must see 0.
//
// 4 asserts:
//   1. baseline syscall_audit_query returns >= 0 (sanity).
//   2. after emitting 3 PLAN_BEGIN events, the all-time count grew by
//      AT LEAST 3.
//   3. since_ns = baseline-last-ts filter returns >= 3 new entries.
//   4. since_ns = ULONG_MAX returns exactly 0.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static audit_entry_u_t g_buf[64];

void _start(void) {
    tap_plan(4);

    // 1. Baseline: query the in-memory ring with no filter, capture the
    //    timestamp of the most recent entry (or 0 if ring is empty).
    long n_before = syscall_audit_query(0, 0, 0, g_buf, 64);
    uint64_t baseline_ns = 0;
    if (n_before > 0) {
        baseline_ns = g_buf[n_before - 1].ns_timestamp;
    }
    TAP_ASSERT(n_before >= 0,
               "1. baseline audit_query returns non-negative count");

    // Emit 3 PLAN_BEGIN events. We want their ns_timestamps to be STRICTLY
    // GREATER than baseline_ns so the since-filter cleanly separates them
    // out. audit_ns_now() == g_timer_ticks * 10000000 (10 ms granularity),
    // so we spin for at least 30 ms to guarantee tick advancement under
    // both KVM (real time) and TCG (TSC-calibrated busy-wait).
    spin_us(30000);  // 30 ms — guarantees >= 3 timer ticks past baseline
    (void)syscall_debug_audit_emit_plan(U_AUDIT_PLAN_BEGIN, 0xAA01);
    (void)syscall_debug_audit_emit_plan(U_AUDIT_PLAN_BEGIN, 0xAA02);
    (void)syscall_debug_audit_emit_plan(U_AUDIT_PLAN_BEGIN, 0xAA03);

    // 2. Query with PLAN_BEGIN-only filter for entries STRICTLY AFTER
    //    baseline_ns. Filter precisely on event_type to exclude unrelated
    //    audit traffic from other CPUs / kernel daemons. Should see >= 3
    //    new PLAN_BEGIN events.
    uint64_t since = baseline_ns + 1ULL;  // strict ">" against the filter
    long n_plan_since = syscall_audit_query(since, 0,
                                            1u << U_AUDIT_PLAN_BEGIN,
                                            g_buf, 64);
    if (n_plan_since < 3) {
        printf("# n_plan_since=%ld since=%lu baseline_ns=%lu\n",
               n_plan_since, (unsigned long)since, (unsigned long)baseline_ns);
    }
    TAP_ASSERT(n_plan_since >= 3,
               "2. since_ns filter + PLAN_BEGIN mask returns >= 3 new events");

    // 3. ALL returned entries from the since_ns query must have
    //    ns_timestamp >= since.  This is the load-bearing kernel
    //    invariant being tested (audit.c:1313).
    int bad_ts = 0;
    for (long i = 0; i < n_plan_since; i++) {
        if (g_buf[i].ns_timestamp < since) bad_ts++;
    }
    if (bad_ts > 0) {
        printf("# %d entries violated ns_timestamp >= since_ns invariant\n", bad_ts);
    }
    TAP_ASSERT(bad_ts == 0,
               "3. every returned entry satisfies ns_timestamp >= since_ns");

    // 4. since_ns = ULLONG_MAX returns 0 (no entries can have
    //    ns_timestamp >= ULLONG_MAX).
    long n_future = syscall_audit_query(0xFFFFFFFFFFFFFFFFULL, 0, 0, g_buf, 64);
    if (n_future != 0) {
        printf("# n_future=%ld (expected 0)\n", n_future);
    }
    TAP_ASSERT(n_future == 0,
               "4. since_ns = ULLONG_MAX filters out all entries");

    tap_done();
    syscall_exit(0);
}
