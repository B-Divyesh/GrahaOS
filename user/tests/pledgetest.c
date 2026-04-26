// user/tests/pledgetest.c — Phase 15b gate test for pledge classes.
//
// Covers SYS_PLEDGE (1062), the pledge-guard enforcement on ~40 syscalls,
// and the audit-entry side effect of successful narrowing. Uses the
// build-gated DEBUG_READ_PLEDGE (50) helper to inspect the current mask.
//
// Assertion groups (total 20):
//   G1 (2): default mask is PLEDGE_ALL
//   G2 (3): SYS_PLEDGE narrows successfully (mask is updated, DEBUG_READ confirms)
//   G3 (2): SYS_PLEDGE rejects widening with -EPERM
//   G4 (3): SYS_PLEDGE rejects PLEDGE_NONE + reserved bits with -EINVAL
//   G5 (6): per-class enforcement: dropped class -> -EPLEDGE on matching syscall
//   G6 (2): audit log shows PLEDGE_NARROW + correct old/new
//   G7 (2): monotonic narrowing — further narrows still produce AUDIT_PLEDGE_NARROW

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// Pull current pledge mask via the test-only DEBUG_READ_PLEDGE (50).
static uint16_t read_my_pledge(void) {
    return (uint16_t)syscall_debug3(DEBUG_READ_PLEDGE, 0, 0);
}

void _start(void) {
    tap_plan(20);

    // =======================================================================
    // G1: Default mask is PLEDGE_ALL (2 asserts)
    // =======================================================================
    uint16_t initial = read_my_pledge();
    TAP_ASSERT(initial == PLEDGE_ALL, "default pledge mask is PLEDGE_ALL (0x3FFF)");
    // Phase 21: bits 12 and 13 now used (storage_server, input_server). Only
    // bits 14..15 remain reserved.
    TAP_ASSERT((initial & 0xC000) == 0, "reserved bits 14..15 are zero");

    // =======================================================================
    // G2: SYS_PLEDGE narrows successfully (3 asserts)
    // =======================================================================
    uint16_t mask_wide = PLEDGE_FS_READ | PLEDGE_FS_WRITE | PLEDGE_NET_CLIENT |
                         PLEDGE_SPAWN | PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_AI_CALL | PLEDGE_COMPUTE | PLEDGE_TIME;
    long r = syscall_pledge(mask_wide);
    TAP_ASSERT(r == 0, "narrow PLEDGE_ALL -> wide mask (drop NET_SERVER) returns 0");
    TAP_ASSERT(read_my_pledge() == mask_wide, "DEBUG_READ_PLEDGE confirms new mask");
    TAP_ASSERT((mask_wide & PLEDGE_NET_SERVER) == 0, "NET_SERVER was dropped");

    // =======================================================================
    // G3: Widening fails -EPERM (2 asserts)
    // =======================================================================
    r = syscall_pledge(PLEDGE_ALL);
    TAP_ASSERT(r == CAP_V2_EPERM, "attempt to widen back to PLEDGE_ALL returns -EPERM");
    r = syscall_pledge(mask_wide | PLEDGE_NET_SERVER);
    TAP_ASSERT(r == CAP_V2_EPERM, "attempt to add NET_SERVER bit returns -EPERM");

    // =======================================================================
    // G4: Invalid inputs rejected (3 asserts)
    // =======================================================================
    r = syscall_pledge(PLEDGE_NONE);
    TAP_ASSERT(r == CAP_V2_EINVAL, "SYS_PLEDGE(0) returns -EINVAL");
    // Phase 21: bit 12 is now PLEDGE_STORAGE_SERVER (assigned). Use bit 14
    // which is still reserved (PLEDGE_RESERVED_MASK = 0xC000 post-Phase-21).
    r = syscall_pledge(0x4000);  // reserved bit 14
    TAP_ASSERT(r == CAP_V2_EINVAL, "SYS_PLEDGE(0x4000) with reserved bit returns -EINVAL");
    TAP_ASSERT(read_my_pledge() == mask_wide, "mask unchanged after rejected narrows");

    // =======================================================================
    // G5: Per-class -EPLEDGE enforcement (6 asserts)
    // =======================================================================
    // Narrow further: keep only FS_READ + SYS_QUERY + COMPUTE + TIME so that
    // FS_WRITE / NET_CLIENT / SPAWN / SYS_CONTROL / IPC tests below return
    // -EPLEDGE. SYS_QUERY must stay so SYS_AUDIT_QUERY in G6 keeps working.
    uint16_t mask_narrow = PLEDGE_FS_READ | PLEDGE_SYS_QUERY |
                           PLEDGE_COMPUTE | PLEDGE_TIME;
    r = syscall_pledge(mask_narrow);
    TAP_ASSERT(r == 0, "further narrow to FS_READ|SYS_QUERY|COMPUTE|TIME succeeds");

    // SYS_CREATE needs FS_WRITE -> -EPLEDGE.
    r = syscall_create("/tmp/pledgetest_unused", 0644);
    TAP_ASSERT(r == CAP_V2_EPLEDGE, "SYS_CREATE after drop FS_WRITE returns -EPLEDGE");

    // SYS_HTTP_GET needs NET_CLIENT -> -EPLEDGE.
    char http_buf[16];
    r = syscall_http_get("http://127.0.0.1/", http_buf, sizeof(http_buf));
    TAP_ASSERT(r == CAP_V2_EPLEDGE, "SYS_HTTP_GET after drop NET_CLIENT returns -EPLEDGE");

    // SYS_SPAWN needs SPAWN -> -EPLEDGE.
    r = syscall_spawn("/bin/true");
    TAP_ASSERT(r == CAP_V2_EPLEDGE, "SYS_SPAWN after drop SPAWN returns -EPLEDGE");

    // SYS_KILL needs SYS_CONTROL -> -EPLEDGE.
    r = syscall_kill(9999 /*nonexistent pid*/, 15);
    TAP_ASSERT(r == CAP_V2_EPLEDGE, "SYS_KILL after drop SYS_CONTROL returns -EPLEDGE");

    // SYS_PIPE needs IPC_SEND -> -EPLEDGE.
    int pipefd[2] = {-1, -1};
    r = syscall_pipe(pipefd);
    TAP_ASSERT(r == CAP_V2_EPLEDGE, "SYS_PIPE after drop IPC_SEND returns -EPLEDGE");

    // =======================================================================
    // G6: Audit entry produced for PLEDGE_NARROW (2 asserts)
    // =======================================================================
    // Use SYS_AUDIT_QUERY (needs SYS_QUERY pledge, which we kept).
    // Static buffer big enough for ~64 entries.
    static audit_entry_u_t aq_buf[64];
    long got = syscall_audit_query(0, 0, (1u << AUDIT_PLEDGE_NARROW),
                                   aq_buf, 64);
    TAP_ASSERT(got >= 2, "audit_query for PLEDGE_NARROW returns >=2 (we did >=2 narrows)");
    // Verify at least one entry has pledge_new matching our most recent narrow.
    int found_narrow = 0;
    for (long i = 0; i < got; i++) {
        if (aq_buf[i].event_type == AUDIT_PLEDGE_NARROW &&
            aq_buf[i].pledge_new == mask_narrow) {
            found_narrow = 1;
            break;
        }
    }
    TAP_ASSERT(found_narrow == 1, "latest PLEDGE_NARROW entry matches mask_narrow");

    // =======================================================================
    // G7: Monotonic narrowing produces additional audit entries (2 asserts)
    // =======================================================================
    uint16_t mask_min = PLEDGE_SYS_QUERY | PLEDGE_COMPUTE | PLEDGE_TIME;
    r = syscall_pledge(mask_min);
    TAP_ASSERT(r == 0, "monotonic narrow to minimum-viable mask succeeds");
    got = syscall_audit_query(0, 0, (1u << AUDIT_PLEDGE_NARROW), aq_buf, 64);
    int count_narrows = 0;
    for (long i = 0; i < got; i++) {
        if (aq_buf[i].event_type == AUDIT_PLEDGE_NARROW) count_narrows++;
    }
    TAP_ASSERT(count_narrows >= 3, "audit log shows >=3 PLEDGE_NARROW entries");

    tap_done();
    exit(0);
}
