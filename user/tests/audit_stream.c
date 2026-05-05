// user/tests/audit_stream.c
//
// Phase 27 Block C — Stage C1 audit subscriber broadcast gate test.
//
// Subscribes to all PLAN_* events, emits one synthetically via
// DEBUG_AUDIT_EMIT_PLAN, drains the subscriber ring, and verifies the
// kernel broadcast actually copied an audit_entry_t into our slot.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    // 1. Subscribe with a filter mask that includes all PLAN_* events.
    uint64_t mask = (1ULL << U_AUDIT_PLAN_BEGIN) |
                    (1ULL << U_AUDIT_PLAN_STEP) |
                    (1ULL << U_AUDIT_PLAN_COMMIT) |
                    (1ULL << U_AUDIT_PLAN_ABORT);
    long slot = syscall_audit_subscribe(mask);
    if (slot < 0) printf("# subscribe rc=%ld\n", slot);
    TAP_ASSERT(slot >= 0, "1. audit_subscribe returns slot id");

    // 2. Drain — should be empty initially (or whatever boot emitted that
    // matched our filter; PLAN_* aren't emitted at boot so should be empty).
    audit_entry_u_t buf[16] = {0};
    long n = syscall_audit_stream_read((int)slot, buf, 16);
    if (n < 0) printf("# stream_read rc=%ld\n", n);
    TAP_ASSERT(n >= 0, "2. audit_stream_read returns count >=0");

    // 3. Emit a PLAN_BEGIN synthetically.
    long rc = syscall_debug_audit_emit_plan(U_AUDIT_PLAN_BEGIN, 0xCAFEBABEull);
    if (rc != 0) printf("# debug_audit_emit_plan rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "3. debug_audit_emit_plan(PLAN_BEGIN) returns 0");

    // 4. Drain again — should now have at least 1 entry, of type PLAN_BEGIN.
    n = syscall_audit_stream_read((int)slot, buf, 16);
    if (n < 1) printf("# stream_read after emit n=%ld\n", n);
    TAP_ASSERT(n >= 1,
               "4. audit_stream_read returns >=1 after PLAN_BEGIN emission");

    // 5. The entry's event_type matches PLAN_BEGIN.
    if (n >= 1 && buf[0].event_type != U_AUDIT_PLAN_BEGIN) {
        printf("# event_type=%u (expected %u)\n",
               (unsigned)buf[0].event_type, U_AUDIT_PLAN_BEGIN);
    }
    TAP_ASSERT(n >= 1 && buf[0].event_type == U_AUDIT_PLAN_BEGIN,
               "5. drained entry has event_type=PLAN_BEGIN");

    tap_done();
    syscall_exit(0);
}
