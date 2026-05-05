// user/tests/audit_plan_codes.c
//
// Phase 27 Block C — Stage C1 PLAN_BEGIN/STEP/COMMIT/ABORT routability gate.
//
// Verifies that all four PLAN_* codes round-trip through the audit subscriber
// broadcast path with their event_type preserved. This locks the AUDIT_EVENT_MAX
// extension (49→54) and the kernel-side PLAN_* writers in tree.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static int verify_event(int slot, uint16_t want, const char *label,
                        uint64_t plan_id) {
    long rc = syscall_debug_audit_emit_plan(want, plan_id);
    if (rc != 0) {
        printf("# %s emit rc=%ld\n", label, rc);
        return 0;
    }
    audit_entry_u_t buf[8] = {0};
    long n = syscall_audit_stream_read(slot, buf, 8);
    if (n < 1) {
        printf("# %s stream_read n=%ld\n", label, n);
        return 0;
    }
    // Walk the drained entries; the test isolates one PLAN code per call,
    // but boot-time noise + earlier PLAN_BEGIN from this same test session
    // may also be present. Match the most-recent matching event_type.
    for (long i = n - 1; i >= 0; i--) {
        if (buf[i].event_type == want) return 1;
    }
    printf("# %s: no entry with event_type=%u in %ld drained\n",
           label, (unsigned)want, n);
    return 0;
}

void _start(void) {
    tap_plan(4);

    uint64_t mask = (1ULL << U_AUDIT_PLAN_BEGIN) |
                    (1ULL << U_AUDIT_PLAN_STEP) |
                    (1ULL << U_AUDIT_PLAN_COMMIT) |
                    (1ULL << U_AUDIT_PLAN_ABORT);
    long slot = syscall_audit_subscribe(mask);
    if (slot < 0) {
        printf("# audit_subscribe rc=%ld; cannot run\n", slot);
        TAP_ASSERT(0, "1. PLAN_BEGIN routable");
        TAP_ASSERT(0, "2. PLAN_STEP routable");
        TAP_ASSERT(0, "3. PLAN_COMMIT routable");
        TAP_ASSERT(0, "4. PLAN_ABORT routable");
        tap_done();
        syscall_exit(1);
    }

    TAP_ASSERT(verify_event((int)slot, U_AUDIT_PLAN_BEGIN, "PLAN_BEGIN",
                            0x1ull),
               "1. PLAN_BEGIN routable through subscriber");
    TAP_ASSERT(verify_event((int)slot, U_AUDIT_PLAN_STEP, "PLAN_STEP",
                            0x2ull),
               "2. PLAN_STEP routable through subscriber");
    TAP_ASSERT(verify_event((int)slot, U_AUDIT_PLAN_COMMIT, "PLAN_COMMIT",
                            0x3ull),
               "3. PLAN_COMMIT routable through subscriber");
    TAP_ASSERT(verify_event((int)slot, U_AUDIT_PLAN_ABORT, "PLAN_ABORT",
                            0x4ull),
               "4. PLAN_ABORT routable through subscriber");

    tap_done();
    syscall_exit(0);
}
