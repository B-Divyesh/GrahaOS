// user/tests/rlimit_syscall_rate.c
//
// Phase 27 Block C — Stage C2 syscall-rate quota substrate gate.
//
// Verifies that:
//   1. The AUDIT_RLIMIT_SYSCALL_RATE code (54) is reachable through the
//      audit subsystem — emit via DEBUG, drain via subscriber, observe.
//   2. AUDIT_EVENT_MAX is now 54 (extended from 49).
//   3. The audit_entry_t carries the kernel-formatted detail (limit=...
//      observed=...) so AI agents can parse it.
//   4. Repeated emissions are routable (not just one-shot).
//
// Note: the runtime token-bucket check that fires this audit code from
// the syscall dispatcher is deferred to FU27.X.rate_check_syscall_path
// (the substrate — writer + audit code + subscriber routing — is what
// this test locks in).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    long slot = syscall_audit_subscribe(1ULL << U_AUDIT_RLIMIT_SYSCALL_RATE);
    if (slot < 0) {
        printf("# subscribe rc=%ld\n", slot);
        TAP_ASSERT(0, "1. RLIMIT_SYSCALL_RATE routable");
        TAP_ASSERT(0, "2. AUDIT_EVENT_MAX bumped");
        TAP_ASSERT(0, "3. detail field populated");
        TAP_ASSERT(0, "4. repeated emissions routable");
        tap_done();
        syscall_exit(1);
    }

    long rc = syscall_debug_audit_emit_plan(U_AUDIT_RLIMIT_SYSCALL_RATE,
                                            /*plan_id, used as limit*/ 100ull);
    if (rc != 0) printf("# emit rc=%ld\n", rc);
    audit_entry_u_t buf[8] = {0};
    long n = syscall_audit_stream_read((int)slot, buf, 8);
    if (n < 1) printf("# stream_read n=%ld\n", n);
    TAP_ASSERT(rc == 0 && n >= 1 &&
               buf[0].event_type == U_AUDIT_RLIMIT_SYSCALL_RATE,
               "1. RLIMIT_SYSCALL_RATE routable through subscriber");

    // 2. Implicit: if AUDIT_EVENT_MAX wasn't extended, the writer would
    // bail or emit a zero event_type. The TAP_ASSERT above implicitly
    // covers this; explicitly check that buf[0].event_type == 54.
    TAP_ASSERT(buf[0].event_type == 54,
               "2. AUDIT_EVENT_MAX extended to 54 (RLIMIT_SYSCALL_RATE present)");

    // 3. Detail field carries kernel-formatted text starting "limit=".
    int detail_ok = (buf[0].detail[0] == 'l' && buf[0].detail[1] == 'i' &&
                     buf[0].detail[2] == 'm' && buf[0].detail[3] == 'i' &&
                     buf[0].detail[4] == 't' && buf[0].detail[5] == '=');
    if (!detail_ok) {
        printf("# detail starts: %c%c%c%c%c%c\n",
               buf[0].detail[0], buf[0].detail[1], buf[0].detail[2],
               buf[0].detail[3], buf[0].detail[4], buf[0].detail[5]);
    }
    TAP_ASSERT(detail_ok,
               "3. audit_entry detail carries 'limit=...' format");

    // 4. Repeated emissions are routable.
    rc = syscall_debug_audit_emit_plan(U_AUDIT_RLIMIT_SYSCALL_RATE, 200ull);
    n = syscall_audit_stream_read((int)slot, buf, 8);
    if (n < 1) printf("# repeat n=%ld\n", n);
    TAP_ASSERT(rc == 0 && n >= 1,
               "4. repeated RLIMIT_SYSCALL_RATE emissions routable");

    tap_done();
    syscall_exit(0);
}
