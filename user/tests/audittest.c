// user/tests/audittest.c — Phase 15b gate test for audit log subsystem.
//
// Covers SYS_AUDIT_QUERY (1063), event/time filtering, CAP_VIOLATION
// emission on failed authority checks, audit_source distinction, and
// basic time-ordering invariants on the in-memory ring.
//
// Assertion groups (total 15):
//   G1 (3): basic write+query — cap_derive emits AUDIT_CAP_DERIVE
//   G2 (2): event_mask filter selects only requested events
//   G3 (2): time window filter (since_ns) restricts returned entries
//   G4 (3): unauthorized revoke -> CAP_VIOLATION with correct rights fields
//   G5 (2): entries are time-ordered within a query
//   G6 (2): audit_source is NATIVE for v2 API calls
//   G7 (1): query with max=0 is rejected

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static audit_entry_u_t g_buf[64];

static cap_token_raw_t cap_lookup(const char *name) {
    return (cap_token_raw_t)syscall_debug3(DEBUG_CAP_LOOKUP, (long)name, 0);
}

void _start(void) {
    tap_plan(15);

    // Ask for our own pid via SYS_GETPID for later filtering.
    int32_t my_pid = (int32_t)syscall_getpid();

    // =======================================================================
    // G1: Basic write+query — cap_derive emits AUDIT_CAP_DERIVE (3 asserts)
    // =======================================================================
    cap_token_raw_t cpu_tok = cap_lookup("cpu");
    TAP_ASSERT(cpu_tok != 0, "DEBUG_CAP_LOOKUP('cpu') returns non-null token");

    // Derive a child — should produce an AUDIT_CAP_DERIVE entry.
    int32_t audience[8];
    for (int i = 0; i < 8; i++) audience[i] = -1;
    audience[0] = my_pid;
    long before_q = syscall_audit_query(0, 0, 1u << AUDIT_CAP_DERIVE, g_buf, 64);
    cap_token_raw_t child = syscall_cap_derive(cpu_tok,
                                               RIGHT_READ | RIGHT_INSPECT,
                                               audience, 0);
    TAP_ASSERT((long)child > 0, "cap_derive succeeds");
    long after_q = syscall_audit_query(0, 0, 1u << AUDIT_CAP_DERIVE, g_buf, 64);
    TAP_ASSERT(after_q > before_q,
               "AUDIT_CAP_DERIVE count increased after cap_derive");

    // =======================================================================
    // G2: event_mask filter selects only requested events (2 asserts)
    // =======================================================================
    long all_events = syscall_audit_query(0, 0, 0, g_buf, 64);
    TAP_ASSERT(all_events >= after_q,
               "querying all events returns >= filtered result");
    long only_derive = syscall_audit_query(0, 0, 1u << AUDIT_CAP_DERIVE, g_buf, 64);
    int non_derive = 0;
    for (long i = 0; i < only_derive; i++) {
        if (g_buf[i].event_type != AUDIT_CAP_DERIVE) non_derive++;
    }
    TAP_ASSERT(non_derive == 0, "event_mask filter excludes non-matching events");

    // =======================================================================
    // G3: Time window filter (2 asserts)
    // =======================================================================
    long nfull = syscall_audit_query(0, 0, 0, g_buf, 64);
    uint64_t mid_ts = 0;
    if (nfull >= 2) mid_ts = g_buf[nfull / 2].ns_timestamp;
    long ntail = syscall_audit_query(mid_ts, 0, 0, g_buf, 64);
    TAP_ASSERT(ntail > 0 && ntail <= nfull,
               "since_ns filter yields subset of all entries");
    // Verify all returned entries satisfy ns_timestamp >= mid_ts.
    int bad_time = 0;
    for (long i = 0; i < ntail; i++) {
        if (g_buf[i].ns_timestamp < mid_ts) bad_time++;
    }
    TAP_ASSERT(bad_time == 0, "all time-filtered entries have ns_timestamp >= since_ns");

    // =======================================================================
    // G4: Unauthorized revoke attempt produces CAP_VIOLATION (3 asserts)
    // =======================================================================
    // Fabricate a token with wrong generation — cap_token_resolve fails.
    // Packing format: {gen:32, idx:24, flags:8}.
    cap_token_raw_t fake_tok = ((cap_token_raw_t)0xDEAD << 32)
                               | ((cap_token_raw_t)1 << 8)  // idx=1 (kernel sentinel)
                               | 0;
    long before_v = syscall_audit_query(0, 0, 1u << AUDIT_CAP_VIOLATION, g_buf, 64);
    long rr = syscall_cap_revoke_v2(fake_tok);
    TAP_ASSERT(rr == CAP_V2_EPERM, "unauthorized revoke returns -EPERM");
    long after_v = syscall_audit_query(0, 0, 1u << AUDIT_CAP_VIOLATION, g_buf, 64);
    TAP_ASSERT(after_v > before_v,
               "AUDIT_CAP_VIOLATION count increased after failed revoke");

    // Verify at least one CAP_VIOLATION has rights_required == RIGHT_REVOKE.
    int found_rights = 0;
    for (long i = 0; i < after_v; i++) {
        if (g_buf[i].event_type == AUDIT_CAP_VIOLATION &&
            g_buf[i].rights_required == RIGHT_REVOKE) {
            found_rights = 1;
            break;
        }
    }
    TAP_ASSERT(found_rights == 1,
               "CAP_VIOLATION for failed revoke carries rights_required=RIGHT_REVOKE");

    // =======================================================================
    // G5: Entries are time-ordered within a single query (2 asserts)
    // =======================================================================
    long nord = syscall_audit_query(0, 0, 0, g_buf, 64);
    TAP_ASSERT(nord >= 2, "query returned >=2 entries to test ordering");
    int ordering_ok = 1;
    for (long i = 1; i < nord; i++) {
        if (g_buf[i].ns_timestamp < g_buf[i-1].ns_timestamp) {
            ordering_ok = 0;
            break;
        }
    }
    TAP_ASSERT(ordering_ok == 1, "entries are returned in non-decreasing ns_timestamp order");

    // =======================================================================
    // G6: audit_source is NATIVE for v2 API calls (2 asserts)
    // =======================================================================
    long nnat = syscall_audit_query(0, 0, 1u << AUDIT_CAP_DERIVE, g_buf, 64);
    int found_native = 0;
    int found_shim = 0;
    for (long i = 0; i < nnat; i++) {
        if (g_buf[i].audit_source == AUDIT_SRC_NATIVE) found_native = 1;
        if (g_buf[i].audit_source == AUDIT_SRC_SHIM)   found_shim = 1;
    }
    TAP_ASSERT(found_native == 1, "at least one CAP_DERIVE entry has audit_source=NATIVE");
    (void)found_shim;  // SHIM entries exist iff shim path was exercised; not required here.
    TAP_ASSERT(g_buf[0].magic == 0x31445541u,
               "audit_entry magic is AUD1 ASCII (0x31445541)");

    // =======================================================================
    // G7: Query with max=0 is rejected (1 assert)
    // =======================================================================
    long rbad = syscall_audit_query(0, 0, 0, g_buf, 0);
    TAP_ASSERT(rbad == CAP_V2_EINVAL,
               "SYS_AUDIT_QUERY(max=0) returns -EINVAL");

    tap_done();
    exit(0);
}
