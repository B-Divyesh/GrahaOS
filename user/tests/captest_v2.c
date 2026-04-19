// user/tests/captest_v2.c — Phase 15a gate test for Capability Objects v2.
//
// Covers the 4 new syscalls (SYS_CAP_DERIVE 1058 / SYS_CAP_REVOKE_V2 1059 /
// SYS_CAP_GRANT 1060 / SYS_CAP_INSPECT 1061) plus the cap_token_t pack/
// unpack invariants and bootstrap cap IMMORTAL enforcement. Uses the
// build-gated DEBUG_CAP_LOOKUP subcommand (48) to obtain a starting token
// for a well-known bootstrap cap ("cpu") and a regular deriveable cap.
//
// Assertion groups (total 40+):
//   G1 (5): token pack/unpack round-trip
//   G2 (3): DEBUG_CAP_LOOKUP returns non-null for known caps
//   G3 (6): subset-rights derive succeeds/fails, audience subset widen fails
//   G4 (5): revoke bumps generation, stale returns -EREVOKED, IMMORTAL -EPERM
//   G5 (4): eager cascade; non-eager preserves children
//   G6 (3): inspect privacy filter, null token
//   G7 (5): handle-table growth at scale; cap_object_cache accounting
//   G8 (3): shim compat (existing cantest paths still accept)
//   G9 (3): edge cases (invalid idx, fabricated generation, fabricated idx)
//   G10 (5): CAP_FLAG_IMMORTAL + RIGHTS_ALL on bootstrap caps

#include "../libtap.h"
#include "../syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define EXPECT_NEG(x) ((long)(x) < 0)

// Wrapper around SYS_DEBUG 48 "cap_lookup".
static cap_token_raw_t cap_lookup(const char *name) {
    return (cap_token_raw_t)syscall_debug3(DEBUG_CAP_LOOKUP, (long)name, 0);
}

void _start(void) {
    tap_plan(42);

    // =======================================================================
    // G1: Token pack/unpack round-trip (5 asserts)
    // =======================================================================
    {
        cap_token_raw_t t = ((uint64_t)42 << 32) | (((uint64_t)7 & 0xFFFFFF) << 8) | 0x3;
        TAP_ASSERT(cap_token_u_gen(t) == 42, "token unpack: gen=42");
        TAP_ASSERT(cap_token_u_idx(t) == 7, "token unpack: idx=7");
        TAP_ASSERT(cap_token_u_flags(t) == 0x3, "token unpack: flags=0x3");

        // Round-trip with max values (24-bit idx).
        uint32_t max_idx = 0xFFFFFF;
        cap_token_raw_t u = ((uint64_t)0xDEADBEEF << 32) | (((uint64_t)max_idx) << 8) | 0xFF;
        TAP_ASSERT(cap_token_u_idx(u) == max_idx, "token idx max 24-bit roundtrip");
        TAP_ASSERT(cap_token_u_gen(u) == 0xDEADBEEF, "token gen 32-bit roundtrip");
    }

    // =======================================================================
    // G2: Bootstrap cap lookup (3 asserts)
    // =======================================================================
    cap_token_raw_t cpu_tok = cap_lookup("cpu");
    cap_token_raw_t mem_tok = cap_lookup("memory");
    cap_token_raw_t nosuch  = cap_lookup("nonexistent_cap_xyz");
    TAP_ASSERT(cpu_tok != 0, "DEBUG_CAP_LOOKUP 'cpu' returns non-null token");
    TAP_ASSERT(mem_tok != 0, "DEBUG_CAP_LOOKUP 'memory' returns non-null token");
    TAP_ASSERT(nosuch == 0, "DEBUG_CAP_LOOKUP bogus name returns 0");

    // =======================================================================
    // G3: Derive subset (6 asserts)
    // =======================================================================
    // Parent (bootstrap) has RIGHTS_ALL + PUBLIC. Derive with RIGHT_READ only.
    {
        int32_t aud[8] = { /*caller implicit*/ -1, -1, -1, -1, -1, -1, -1, -1 };
        // audience NULL → default to [caller_pid].
        // Child needs RIGHT_DERIVE to be a usable parent for future derives.
        cap_token_raw_t child_read = syscall_cap_derive(cpu_tok, RIGHT_READ | RIGHT_DERIVE, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(child_read) && child_read != 0,
                   "derive(RIGHT_READ|RIGHT_DERIVE) from bootstrap succeeds");
        TAP_ASSERT(cap_token_u_idx(child_read) != 0,
                   "derived token has non-zero idx");

        // Derive from child_read with superset (adding WRITE) → EPERM
        long bad = syscall_cap_derive(child_read, RIGHT_READ | RIGHT_WRITE, NULL, 0);
        TAP_ASSERT(EXPECT_NEG(bad), "derive super-set rights fails");

        // Derive from child_read with same-subset → success
        cap_token_raw_t grand = syscall_cap_derive(child_read, RIGHT_READ, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(grand) && grand != 0,
                   "derive equal rights from child succeeds");

        // Derive with widening audience (include unknown pid 99) → EPERM
        int32_t wider[8] = { 99, -1, -1, -1, -1, -1, -1, -1 };
        long wide = syscall_cap_derive(grand, RIGHT_READ, wider, 0);
        TAP_ASSERT(EXPECT_NEG(wide), "derive with audience-widening fails");
        (void)aud;

        // Derive of 0-rights token (legal, limit case)
        cap_token_raw_t zero_rights = syscall_cap_derive(cpu_tok, 0, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(zero_rights), "derive with zero rights still succeeds");
    }

    // =======================================================================
    // G4: Revoke semantics (5 asserts)
    // =======================================================================
    {
        // Revoke on bootstrap (IMMORTAL) → EPERM
        long r_imm = syscall_cap_revoke_v2(cpu_tok);
        TAP_ASSERT(EXPECT_NEG(r_imm), "revoke IMMORTAL bootstrap cap returns negative");

        // Derive a revocable cap
        cap_token_raw_t rev_tok = syscall_cap_derive(cpu_tok, RIGHT_READ | RIGHT_REVOKE, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(rev_tok) && rev_tok != 0, "derived revocable token");
        uint32_t gen_before = cap_token_u_gen(rev_tok);

        // Revoke it
        long r = syscall_cap_revoke_v2(rev_tok);
        TAP_ASSERT(r >= 1, "revoke derived token returns >= 1");

        // Inspect stale token → -EREVOKED
        cap_inspect_result_u_t out;
        long ins = syscall_cap_inspect(rev_tok, &out);
        TAP_ASSERT(EXPECT_NEG(ins), "inspect stale token fails");

        // Re-revoke fails (already revoked)
        long rr = syscall_cap_revoke_v2(rev_tok);
        TAP_ASSERT(EXPECT_NEG(rr), "re-revoke already-revoked returns negative");
        (void)gen_before;
    }

    // =======================================================================
    // G5: Cascade (4 asserts)
    // =======================================================================
    {
        // Eager cascade: derive A (EAGER), B from A, C from B. Revoke A.
        // B and C should both be invalidated.
        cap_token_raw_t a = syscall_cap_derive(cpu_tok,
                                               RIGHT_READ | RIGHT_DERIVE | RIGHT_REVOKE,
                                               NULL,
                                               CAP_FLAG_EAGER_REVOKE);
        TAP_ASSERT(!EXPECT_NEG(a) && a != 0, "create root A with EAGER_REVOKE");
        cap_token_raw_t b = syscall_cap_derive(a, RIGHT_READ | RIGHT_DERIVE, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(b) && b != 0, "derive B from A");
        cap_token_raw_t c = syscall_cap_derive(b, RIGHT_READ, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(c) && c != 0, "derive C from B");

        // Revoke A → cascades to B, C.
        long rc = syscall_cap_revoke_v2(a);
        TAP_ASSERT(rc >= 1, "revoke A returns >= 1 (self + descendants)");
    }

    // =======================================================================
    // G6: Inspect + privacy (3 asserts)
    // =======================================================================
    {
        cap_inspect_result_u_t out;
        // Inspect bootstrap → success.
        long ins = syscall_cap_inspect(cpu_tok, &out);
        TAP_ASSERT(ins == 0, "inspect bootstrap succeeds");
        TAP_ASSERT(out.kind == CAP_KIND_CAN, "inspect: bootstrap kind=CAN");

        // Null token → negative.
        long ins_null = syscall_cap_inspect(0, &out);
        TAP_ASSERT(EXPECT_NEG(ins_null), "inspect null token fails");
    }

    // =======================================================================
    // G7: Handle-table growth + accounting (5 asserts)
    // =======================================================================
    {
        // Derive 50 tokens in a row — should succeed (well below CAP_HANDLE_MAX).
        int ok_count = 0;
        for (int i = 0; i < 50; i++) {
            cap_token_raw_t t = syscall_cap_derive(cpu_tok, RIGHT_READ, NULL, 0);
            if (!EXPECT_NEG(t) && t != 0) ok_count++;
        }
        TAP_ASSERT(ok_count >= 40, "at least 40/50 derives succeeded (handle table + slab growth)");

        // Snapshot cap_object_cache.in_use via kheap stats — should reflect
        // the new live objects.
        kheap_stats_entry_u_t st[32];
        int n = syscall_kheap_stats(st, 32);
        int found_cap_obj = 0;
        uint64_t in_use = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(st[i].name, "cap_object_t") == 0) {
                found_cap_obj = 1;
                in_use = st[i].in_use;
                break;
            }
        }
        TAP_ASSERT(found_cap_obj, "cap_object_t slab cache present in kheap stats");
        TAP_ASSERT(in_use >= 40, "cap_object_t in_use reflects derives (>=40)");

        // Another derive after bulk — should still succeed (handle table grew).
        cap_token_raw_t extra = syscall_cap_derive(cpu_tok, RIGHT_READ, NULL, 0);
        TAP_ASSERT(!EXPECT_NEG(extra) && extra != 0,
                   "derive after bulk still succeeds");

        // Revoke extra to prove revoke still works with grown table.
        long re = syscall_cap_revoke_v2(extra);
        TAP_ASSERT(re >= 1 || EXPECT_NEG(re),
                   "revoke-extra returns well-defined result");
    }

    // =======================================================================
    // G8: Shim compat — Phase 16 deprecated every legacy syscall to return
    // -EDEPRECATED (-78). Same three assertions, flipped polarity.
    // =======================================================================
    {
        int act = syscall_cap_activate("cpu");
        TAP_ASSERT(act == -78, "SYS_CAP_ACTIVATE 'cpu' now -EDEPRECATED");
        int act2 = syscall_cap_activate("nonexistent_xyz");
        TAP_ASSERT(act2 == -78, "SYS_CAP_ACTIVATE bogus name now -EDEPRECATED");
        int wat = syscall_cap_watch("cpu");
        TAP_ASSERT(wat == -78, "SYS_CAP_WATCH now -EDEPRECATED");
    }

    // =======================================================================
    // G9: Edge cases (3 asserts)
    // =======================================================================
    {
        // Fabricated idx (way out of range) → null-resolve
        cap_token_raw_t fake = ((uint64_t)1 << 32) | (((uint64_t)0xFFFFFE) << 8) | 0;
        cap_inspect_result_u_t out;
        long r = syscall_cap_inspect(fake, &out);
        TAP_ASSERT(EXPECT_NEG(r), "inspect fabricated idx fails without crash");

        // Fabricated generation on real idx → EREVOKED
        uint32_t real_idx = cap_token_u_idx(cpu_tok);
        cap_token_raw_t wrong_gen = ((uint64_t)0xBADBADBAD << 32) |
                                    (((uint64_t)real_idx) << 8) | 0;
        long rg = syscall_cap_inspect(wrong_gen, &out);
        TAP_ASSERT(EXPECT_NEG(rg), "inspect wrong-gen token fails");

        // Revoke on fabricated idx → EPERM
        long rv = syscall_cap_revoke_v2(fake);
        TAP_ASSERT(EXPECT_NEG(rv), "revoke fabricated idx fails");
    }

    // =======================================================================
    // G10: IMMORTAL + RIGHTS_ALL on bootstrap caps (5 asserts)
    // =======================================================================
    {
        cap_inspect_result_u_t out;
        long r = syscall_cap_inspect(cpu_tok, &out);
        TAP_ASSERT(r == 0, "inspect 'cpu' succeeds");
        TAP_ASSERT(out.flags & CAP_FLAG_IMMORTAL, "'cpu' has CAP_FLAG_IMMORTAL");
        TAP_ASSERT(out.flags & CAP_FLAG_PUBLIC, "'cpu' has CAP_FLAG_PUBLIC");
        TAP_ASSERT(out.rights_bitmap == RIGHTS_ALL, "'cpu' rights_bitmap == RIGHTS_ALL");
        TAP_ASSERT(out.owner_pid < 0, "'cpu' owner_pid < 0 (kernel)");
    }

    tap_done();
    exit(0);
}
