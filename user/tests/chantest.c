// user/tests/chantest.c — Phase 17 TAP test.
//
// 24 TAP assertions covering channel lifecycle, send/recv round-trip,
// type-hash enforcement, full-ring EAGAIN, and pledge enforcement. All
// assertions run within a single process (cross-process tests require
// SYS_SPAWN attrs which are deferred to Phase 17.1).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static void zero_msg(chan_msg_user_t *m) {
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
}

void _start(void) {
    tap_plan(24);

    uint64_t hash_notify = gcp_type_hash("grahaos.notify.v1");
    uint64_t hash_test   = gcp_type_hash("grahaos.test.v1");

    // -------------------- G1: Create (3 asserts) --------------------
    cap_token_u_t rd = {.raw = 0}, wr = {.raw = 0};
    long rc = syscall_chan_create(hash_notify, CHAN_MODE_BLOCKING, 16, &wr);
    rd.raw = (uint64_t)rc;
    TAP_ASSERT(rc > 0, "1. chan_create returns nonzero read handle");
    TAP_ASSERT(wr.raw != 0, "2. write handle populated");
    TAP_ASSERT(rd.raw != wr.raw, "3. read and write handles differ");

    // -------------------- G2: Send+recv roundtrip (4 asserts) ---------
    chan_msg_user_t out_msg;
    zero_msg(&out_msg);
    out_msg.header.type_hash  = hash_notify;
    out_msg.header.inline_len = 16;
    out_msg.header.nhandles   = 0;
    for (int i = 0; i < 16; i++) out_msg.inline_payload[i] = (uint8_t)(0xA0 + i);
    long sres = syscall_chan_send(wr, &out_msg, 1000000000ULL);
    TAP_ASSERT(sres == 0, "4. chan_send returns 0 on happy path");

    chan_msg_user_t in_msg;
    zero_msg(&in_msg);
    long rres = syscall_chan_recv(rd, &in_msg, 1000000000ULL);
    TAP_ASSERT(rres == 16, "5. chan_recv returns inline_len (16)");
    int bytes_match = 1;
    for (int i = 0; i < 16; i++) {
        if (in_msg.inline_payload[i] != (uint8_t)(0xA0 + i)) { bytes_match = 0; break; }
    }
    TAP_ASSERT(bytes_match, "6. received bytes match sent bytes");
    TAP_ASSERT(in_msg.header.seq == 0, "7. first message has seq == 0");

    // Second round — seq should increment.
    out_msg.inline_payload[0] = 0x42;
    sres = syscall_chan_send(wr, &out_msg, 1000000000ULL);
    rres = syscall_chan_recv(rd, &in_msg, 1000000000ULL);
    (void)sres; (void)rres;

    // -------------------- G3: Handle transfer (3 asserts) --------------
    // Create aux channel B.
    cap_token_u_t b_rd = {.raw = 0}, b_wr = {.raw = 0};
    long brc = syscall_chan_create(hash_notify, CHAN_MODE_BLOCKING, 4, &b_wr);
    b_rd.raw = (uint64_t)brc;
    // Send B's write handle across primary channel.
    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 4;
    out_msg.header.nhandles = 1;
    out_msg.handles[0] = b_wr.raw;
    out_msg.inline_payload[0] = 0xCA;
    out_msg.inline_payload[1] = 0xFE;
    out_msg.inline_payload[2] = 0xBA;
    out_msg.inline_payload[3] = 0xBE;
    sres = syscall_chan_send(wr, &out_msg, 1000000000ULL);
    TAP_ASSERT(sres == 0, "8. send with embedded handle returns 0");

    zero_msg(&in_msg);
    rres = syscall_chan_recv(rd, &in_msg, 1000000000ULL);
    TAP_ASSERT(rres == 4, "9. recv after handle-bearing send returns 4");
    // In single-process tests, sender and receiver are the same task,
    // so the token's idx+gen match. Cross-process would differ (gen
    // is per-process). Just assert the token is non-zero (live).
    TAP_ASSERT(in_msg.handles[0] != 0,
               "10. recv produces a live token for the transferred handle");

    // Clean up B.
    cap_token_u_t b_wr_new = {.raw = in_msg.handles[0]};
    // (revoking b_rd/b_wr_new would require SYS_CAP_REVOKE_V2; skip.)
    (void)b_wr_new;

    // -------------------- G4: Type-hash mismatch (3 asserts) -----------
    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_test;  // wrong type
    out_msg.header.inline_len = 1;
    sres = syscall_chan_send(wr, &out_msg, 1000000000ULL);
    TAP_ASSERT(sres == -71, "11. type-hash mismatch returns -EPROTOTYPE (-71)");

    // Still usable — wrong message didn't corrupt the channel.
    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 1;
    out_msg.inline_payload[0] = 0x77;
    sres = syscall_chan_send(wr, &out_msg, 1000000000ULL);
    TAP_ASSERT(sres == 0, "12. channel still usable after mistype");
    rres = syscall_chan_recv(rd, &in_msg, 1000000000ULL);
    TAP_ASSERT(rres == 1 && in_msg.inline_payload[0] == 0x77,
               "13. recovered byte matches post-mismatch");

    // -------------------- G5: Full ring nonblock (3 asserts) -----------
    cap_token_u_t nb_rd = {.raw = 0}, nb_wr = {.raw = 0};
    long nbrc = syscall_chan_create(hash_notify, CHAN_MODE_NONBLOCKING, 4, &nb_wr);
    nb_rd.raw = (uint64_t)nbrc;
    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 1;
    int filled_ok = 1;
    for (int i = 0; i < 4; i++) {
        out_msg.inline_payload[0] = (uint8_t)i;
        if (syscall_chan_send(nb_wr, &out_msg, 0) != 0) { filled_ok = 0; break; }
    }
    TAP_ASSERT(filled_ok, "14. first 4 nonblock sends succeed");
    long fifth = syscall_chan_send(nb_wr, &out_msg, 0);
    TAP_ASSERT(fifth == -11, "15. 5th send on full nonblock ring returns -EAGAIN (-11)");

    long drc = syscall_chan_recv(nb_rd, &in_msg, 0);
    TAP_ASSERT(drc == 1, "16. recv from full ring drains one slot");

    // -------------------- G6: Timed blocking send (2 asserts) ----------
    cap_token_u_t to_rd = {.raw = 0}, to_wr = {.raw = 0};
    long torc = syscall_chan_create(hash_notify, CHAN_MODE_BLOCKING, 2, &to_wr);
    to_rd.raw = (uint64_t)torc;
    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 1;
    // Fill the 2-slot ring.
    syscall_chan_send(to_wr, &out_msg, 1000000000ULL);
    syscall_chan_send(to_wr, &out_msg, 1000000000ULL);
    // Next send should time out at ~20 ms (tick granularity).
    long tmo = syscall_chan_send(to_wr, &out_msg, 20000000ULL);
    TAP_ASSERT(tmo == -110, "17. send on full blocking ring with 20ms timeout returns -ETIMEDOUT");
    // Drain so cleanup doesn't deadlock the shutdown path.
    syscall_chan_recv(to_rd, &in_msg, 0);
    syscall_chan_recv(to_rd, &in_msg, 0);
    TAP_ASSERT(1, "18. timeout path did not panic the kernel");

    // -------------------- G7: Throughput sanity (3 asserts) -----------
    cap_token_u_t t_rd = {.raw = 0}, t_wr = {.raw = 0};
    long trc = syscall_chan_create(hash_notify, CHAN_MODE_BLOCKING, 32, &t_wr);
    t_rd.raw = (uint64_t)trc;
    TAP_ASSERT(trc > 0, "19. throughput channel created");

    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 64;
    int roundtrips_ok = 1;
    for (int i = 0; i < 200; i++) {
        out_msg.inline_payload[0] = (uint8_t)(i & 0xFF);
        if (syscall_chan_send(t_wr, &out_msg, 1000000000ULL) != 0) { roundtrips_ok = 0; break; }
        if (syscall_chan_recv(t_rd, &in_msg, 1000000000ULL) != 64) { roundtrips_ok = 0; break; }
        if (in_msg.inline_payload[0] != (uint8_t)(i & 0xFF)) { roundtrips_ok = 0; break; }
    }
    TAP_ASSERT(roundtrips_ok, "20. 200 send+recv roundtrips preserve payload");
    TAP_ASSERT(in_msg.header.seq == 199,
               "21. final seq number matches send count (199)");

    // -------------------- G8: Pledge enforcement (3 asserts) -----------
    // Narrow pledge: drop IPC_SEND. Keep IPC_RECV so chan_recv still works
    // if it's ever called, but send should now fail.
    uint16_t narrow = (uint16_t)(PLEDGE_ALL & ~PLEDGE_IPC_SEND);
    long prc = syscall_pledge(narrow);
    TAP_ASSERT(prc == 0, "22. syscall_pledge narrow succeeded");

    zero_msg(&out_msg);
    out_msg.header.type_hash = hash_notify;
    out_msg.header.inline_len = 1;
    long send_after_pledge = syscall_chan_send(t_wr, &out_msg, 0);
    TAP_ASSERT(send_after_pledge == -7,
               "23. chan_send returns -EPLEDGE after IPC_SEND dropped");

    // chan_create also requires IPC_SEND — should fail.
    cap_token_u_t x_wr = {.raw = 0};
    long xrc = syscall_chan_create(hash_notify, CHAN_MODE_BLOCKING, 4, &x_wr);
    TAP_ASSERT(xrc == -7,
               "24. chan_create returns -EPLEDGE after IPC_SEND dropped");

    tap_done();
    exit(0);
}
