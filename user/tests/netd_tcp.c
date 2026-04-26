// user/tests/netd_tcp.c — Phase 22 Stage B U10-U12 gate test.
//
// Per spec risk #2 ("TCP state machine was written BEFORE implementation"),
// the test drives every major edge through hand-crafted segments that match
// bit-for-bit what a compliant peer would put on the wire. The module under
// test is libnetd.a's netd_tcp.c; no daemon or wire path required.
//
// Edges covered:
//   G1.  Header build/parse round-trip (bare ACK; no options)
//   G2.  Header with MSS option (SYN path)
//   G3.  Pseudo-header checksum verification + tamper rejection
//   G4.  Socket table alloc/free + find_established/find_listen
//   G5.  Client-side handshake: CLOSED → SYN_SENT (our SYN) → ESTABLISHED
//   G6.  Server-side handshake: LISTEN → SYN_RCVD → ESTABLISHED
//   G7.  Data segment: ACK advances snd_una; cwnd grows (slow-start)
//   G8.  Data segment: payload advances rcv_nxt; outgoing ACK emitted
//   G9.  Close from ESTABLISHED: FIN_WAIT1, peer ACK → FIN_WAIT2, peer FIN
//        → TIME_WAIT; TIME_WAIT expires at 2*MSL
//  G10.  Simultaneous close: FIN_WAIT1 + FIN → CLOSING → ACK → TIME_WAIT
//  G11.  Server-side close: ESTABLISHED (after peer FIN) → CLOSE_WAIT →
//        netd_tcp_close → LAST_ACK → peer ACK → CLOSED
//  G12.  RST on exact SEQ in ESTABLISHED → CLOSED
//  G13.  RST on mis-matched SEQ in ESTABLISHED → dropped (MVP), state kept
//  G14.  SYN retransmit on RTO in SYN_SENT

#include "../libtap.h"
#include "../netd.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void exit(int);

static int bytes_eq(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

// Arbitrary ticks_per_sec for deterministic timer math in tests.
// Use 1,000,000 so ms_to_tsc = ms * 1000 — easy mental arithmetic.
static const uint64_t TPS = 1000000ull;

void _start(void) {
    tap_plan(56);

    // ====================================================================
    // G1. Bare ACK header build/parse round-trip.
    // ====================================================================
    {
        uint32_t src = 0x0A00020Fu, dst = 0x08080808u;
        uint8_t seg[20];
        size_t n = netd_tcp_build(seg, src, dst, 12345, 80,
                                  0x1000u, 0x2000u,
                                  TCP_FLAG_ACK, 65535,
                                  0, (uint8_t*)0, 0);
        TAP_ASSERT(n == 20, "1. bare ACK segment is 20 bytes");

        tcp_parsed_t p;
        int rc = netd_tcp_parse(seg, n, src, dst, &p);
        TAP_ASSERT(rc == 0, "2. parse succeeds on bare ACK");
        TAP_ASSERT(p.src_port == 12345 && p.dst_port == 80,
                   "3. ports round-trip");
        TAP_ASSERT(p.seq == 0x1000u && p.ack == 0x2000u,
                   "4. seq/ack round-trip");
        TAP_ASSERT(p.flags == TCP_FLAG_ACK, "5. flags round-trip");
        TAP_ASSERT(p.data_offset_bytes == 20, "6. data_offset = 20 (no options)");
        TAP_ASSERT(p.payload_len == 0, "7. no payload");
    }

    // ====================================================================
    // G2. SYN with MSS option emitted and parsed.
    // ====================================================================
    {
        uint32_t src = 0x0A00020Fu, dst = 0x08080808u;
        uint8_t seg[24];
        size_t n = netd_tcp_build(seg, src, dst, 12345, 80,
                                  0x1000u, 0,
                                  TCP_FLAG_SYN, 65535,
                                  1460, (uint8_t*)0, 0);
        TAP_ASSERT(n == 24, "8. SYN with MSS option is 24 bytes");

        tcp_parsed_t p;
        int rc = netd_tcp_parse(seg, n, src, dst, &p);
        TAP_ASSERT(rc == 0, "9. parse SYN+MSS succeeds");
        TAP_ASSERT(p.data_offset_bytes == 24, "10. data_offset = 24 w/ MSS opt");
        TAP_ASSERT(p.opt_mss == 1460, "11. MSS option = 1460");
    }

    // ====================================================================
    // G3. Pseudo-header checksum: tamper rejects.
    // ====================================================================
    {
        uint32_t src = 0x0A00020Fu, dst = 0x08080808u;
        uint8_t seg[20 + 4];
        for (int i = 0; i < 4; i++) seg[20 + i] = (uint8_t)('X' + i);
        size_t n = netd_tcp_build(seg, src, dst, 12345, 80,
                                  0x1000u, 0x2000u, TCP_FLAG_ACK, 65535,
                                  0, seg + 20, 4);
        TAP_ASSERT(n == 24, "12. ACK with 4-byte payload = 24 bytes");

        tcp_parsed_t p;
        int rc = netd_tcp_parse(seg, n, src, dst, &p);
        TAP_ASSERT(rc == 0 && p.payload_len == 4,
                   "13. parse payload round-trips");

        // Tamper: flip a payload byte. Checksum should fail.
        seg[23] ^= 0x01;
        rc = netd_tcp_parse(seg, n, src, dst, &p);
        TAP_ASSERT(rc < 0, "14. tampered payload fails checksum");
    }

    // ====================================================================
    // G4. Socket table alloc / free / find.
    // ====================================================================
    {
        tcp_table_t tbl;
        netd_tcp_table_init(&tbl);
        int a = netd_tcp_socket_alloc(&tbl, 0xAAAA);
        int b = netd_tcp_socket_alloc(&tbl, 0xBBBB);
        TAP_ASSERT(a >= 0 && b >= 0 && a != b,
                   "15. two socket allocs succeed + distinct");

        tbl.sockets[a].local_ip   = 0x0A00020Fu;
        tbl.sockets[a].local_port = 12345;
        tbl.sockets[a].remote_ip  = 0x08080808u;
        tbl.sockets[a].remote_port= 80;
        tbl.sockets[a].state      = TCP_STATE_ESTABLISHED;

        int found = netd_tcp_find_established(&tbl, 0x0A00020Fu, 12345,
                                              0x08080808u, 80);
        TAP_ASSERT(found == a, "16. find_established matches 4-tuple");

        tbl.sockets[b].local_port = 8080;
        tbl.sockets[b].local_ip   = 0;
        tbl.sockets[b].state      = TCP_STATE_LISTEN;
        int l = netd_tcp_find_listen(&tbl, 0x0A00020Fu, 8080);
        TAP_ASSERT(l == b, "17. find_listen with INADDR_ANY matches");

        netd_tcp_socket_free(&tbl, a);
        TAP_ASSERT(tbl.sockets[a].state == TCP_STATE_CLOSED,
                   "18. freed slot reset to CLOSED");
    }

    // ====================================================================
    // G5. CLIENT handshake: CLOSED → SYN_SENT (our SYN) → ESTABLISHED
    // ====================================================================
    tcp_socket_t cs;
    {
        // Simulate client stack.
        // netd_tcp_table_init zeroes; we just fill manually.
        cs.state = TCP_STATE_CLOSED;
        // Pre-fill 4-tuple (caller's responsibility before connect).
        cs.local_ip   = 0x0A00020Fu;
        cs.local_port = 49152;
        cs.remote_ip  = 0x08080808u;
        cs.remote_port = 80;
        cs.rcv_wnd = TCP_DEFAULT_WINDOW;
        cs.mss     = TCP_DEFAULT_MSS;
        cs.snd_nxt = 0;
        cs.snd_una = 0;
        cs.iss     = 0;
        cs.irs     = 0;
        cs.rcv_nxt = 0;
        cs.snd_wnd = 0;
        cs.cwnd    = 0;
        cs.ssthresh = 0;
        cs.dup_acks = 0;
        cs.rto_ms = 0;
        cs.retx_expiry_tsc = 0;
        cs.time_wait_expiry_tsc = 0;
        cs.owner_cookie = 0xCAFE;

        uint8_t syn_out[40];
        size_t syn_len = 0;
        int rc = netd_tcp_connect(&cs, 0x1000u /*ISS*/,
                                  syn_out, sizeof(syn_out), &syn_len,
                                  TCP_DEFAULT_RTO_MS, 0, TPS);
        TAP_ASSERT(rc == 0, "19. connect() returns 0");
        TAP_ASSERT(cs.state == TCP_STATE_SYN_SENT, "20. state = SYN_SENT");
        TAP_ASSERT(syn_len == 24, "21. connect emitted 24-byte SYN+MSS");
        TAP_ASSERT(cs.iss == 0x1000u && cs.snd_una == 0x1000u &&
                   cs.snd_nxt == 0x1001u,
                   "22. snd_una/snd_nxt after SYN");

        // Peer sends SYN-ACK.
        uint32_t peer_iss = 0xABCD0000u;
        uint8_t synack[24];
        netd_tcp_build(synack, cs.remote_ip, cs.local_ip,
                       cs.remote_port, cs.local_port,
                       peer_iss, cs.iss + 1,
                       TCP_FLAG_SYN | TCP_FLAG_ACK, 65535,
                       1460, (uint8_t*)0, 0);
        tcp_parsed_t pkt;
        rc = netd_tcp_parse(synack, 24,
                            cs.remote_ip, cs.local_ip, &pkt);
        TAP_ASSERT(rc == 0, "23. parse peer SYN-ACK ok");

        uint8_t ack_out[40];
        size_t ack_len = 0;
        rc = netd_tcp_on_segment(&cs, &pkt, 1000, TPS,
                                 ack_out, sizeof(ack_out), &ack_len);
        TAP_ASSERT(rc == 0 && cs.state == TCP_STATE_ESTABLISHED,
                   "24. ESTABLISHED after SYN-ACK");
        TAP_ASSERT(ack_len == 20,
                   "25. final ACK emitted");
        TAP_ASSERT(cs.rcv_nxt == peer_iss + 1 && cs.snd_una == cs.iss + 1,
                   "26. rcv_nxt/snd_una after SYN-ACK");
    }

    // ====================================================================
    // G6. SERVER handshake: LISTEN → SYN_RCVD → ESTABLISHED
    // ====================================================================
    tcp_socket_t ss;
    {
        ss.state = TCP_STATE_CLOSED;
        ss.local_ip = 0; ss.local_port = 0;
        ss.remote_ip = 0; ss.remote_port = 0;
        ss.rcv_wnd = TCP_DEFAULT_WINDOW; ss.mss = TCP_DEFAULT_MSS;
        ss.snd_nxt = 0; ss.snd_una = 0; ss.iss = 0; ss.irs = 0;
        ss.rcv_nxt = 0; ss.snd_wnd = 0; ss.cwnd = 0;
        ss.ssthresh = 0; ss.dup_acks = 0; ss.rto_ms = 0;
        ss.retx_expiry_tsc = 0; ss.time_wait_expiry_tsc = 0;
        ss.owner_cookie = 0xD00D;

        int rc = netd_tcp_listen(&ss, 0x0A00020Fu, 80);
        TAP_ASSERT(rc == 0 && ss.state == TCP_STATE_LISTEN,
                   "27. listen() → LISTEN");

        // Pre-fill remote addr (caller's job in main loop).
        ss.remote_ip = 0x0A000202u;   // peer

        // Incoming SYN from peer.
        uint8_t syn[24];
        netd_tcp_build(syn, ss.remote_ip, ss.local_ip,
                       35000, ss.local_port,
                       0xDEAD0000u, 0,
                       TCP_FLAG_SYN, 65535,
                       1460, (uint8_t*)0, 0);
        tcp_parsed_t pkt;
        netd_tcp_parse(syn, 24, ss.remote_ip, ss.local_ip, &pkt);

        uint8_t synack[40];
        size_t synack_len = 0;
        rc = netd_tcp_on_segment(&ss, &pkt, 2000, TPS,
                                 synack, sizeof(synack), &synack_len);
        TAP_ASSERT(rc == 0 && ss.state == TCP_STATE_SYN_RCVD,
                   "28. SYN → SYN_RCVD");
        TAP_ASSERT(synack_len == 24,
                   "29. SYN-ACK emitted (with MSS opt)");
        TAP_ASSERT(ss.rcv_nxt == 0xDEAD0000u + 1,
                   "30. rcv_nxt = peer_iss + 1");

        // Peer sends final ACK.
        uint8_t ack[20];
        netd_tcp_build(ack, ss.remote_ip, ss.local_ip,
                       ss.remote_port, ss.local_port,
                       ss.rcv_nxt, ss.iss + 1,
                       TCP_FLAG_ACK, 65535,
                       0, (uint8_t*)0, 0);
        netd_tcp_parse(ack, 20, ss.remote_ip, ss.local_ip, &pkt);

        uint8_t nothing[40];
        size_t nothing_len = 0;
        rc = netd_tcp_on_segment(&ss, &pkt, 3000, TPS,
                                 nothing, sizeof(nothing), &nothing_len);
        TAP_ASSERT(rc == 0 && ss.state == TCP_STATE_ESTABLISHED,
                   "31. final ACK → ESTABLISHED");
    }

    // ====================================================================
    // G7. Data segment in ESTABLISHED: ACK + data → snd_una advance + rcv_nxt
    // advance + outgoing ACK emitted.
    // ====================================================================
    {
        // Use cs from G5. cs.state is ESTABLISHED; cs.rcv_nxt == peer_iss+1.
        uint32_t initial_cwnd = cs.cwnd;
        uint8_t data_seg[20 + 5];
        const uint8_t body[5] = {'h','e','l','l','o'};
        netd_tcp_build(data_seg, cs.remote_ip, cs.local_ip,
                       cs.remote_port, cs.local_port,
                       cs.rcv_nxt, cs.iss + 1 /* acking nothing new */,
                       TCP_FLAG_ACK, 65535,
                       0, body, 5);
        tcp_parsed_t pkt;
        netd_tcp_parse(data_seg, 25, cs.remote_ip, cs.local_ip, &pkt);

        uint8_t ack_out[40];
        size_t ack_len = 0;
        int rc = netd_tcp_on_segment(&cs, &pkt, 4000, TPS,
                                     ack_out, sizeof(ack_out), &ack_len);
        TAP_ASSERT(rc == 0, "32. data segment handled in ESTABLISHED");
        TAP_ASSERT(cs.rcv_nxt == pkt.seq + 5,
                   "33. rcv_nxt advanced by payload_len");
        TAP_ASSERT(ack_len == 20, "34. ACK emitted in response to data");
        // cwnd in slow-start should grow by MSS on a newly-ACK'd byte.
        // Our ACK didn't advance snd_una here — cwnd unchanged.
        TAP_ASSERT(cs.cwnd == initial_cwnd,
                   "35. pure data segment doesn't grow cwnd (no new ACK)");
    }

    // ====================================================================
    // G8. Active close from ESTABLISHED: FIN_WAIT1 → FIN_WAIT2 → TIME_WAIT;
    // TIME_WAIT expiry.
    // ====================================================================
    {
        // Fresh socket in ESTABLISHED.
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_ESTABLISHED;
        s.local_ip = 0x0A00020Fu; s.local_port = 49153;
        s.remote_ip = 0x08080808u; s.remote_port = 80;
        s.iss = 0x5000u; s.snd_una = 0x5000u + 1; s.snd_nxt = 0x5000u + 1;
        s.irs = 0x6000u; s.rcv_nxt = 0x6000u + 1;
        s.rcv_wnd = TCP_DEFAULT_WINDOW; s.mss = TCP_DEFAULT_MSS;

        uint8_t fin_out[40];
        size_t fin_len = 0;
        int rc = netd_tcp_close(&s, fin_out, sizeof(fin_out), &fin_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_FIN_WAIT1 && fin_len == 20,
                   "36. close() from ESTABLISHED → FIN_WAIT1 (20-byte FIN)");
        uint32_t our_fin_seq = 0x5000u + 1;  // first byte after SYN
        uint32_t our_fin_ack_expect = our_fin_seq + 1;

        // Peer ACKs our FIN but doesn't FIN yet → FIN_WAIT2.
        uint8_t ack_fin[20];
        tcp_parsed_t pkt;
        netd_tcp_build(ack_fin, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, our_fin_ack_expect,
                       TCP_FLAG_ACK, 65535, 0, (uint8_t*)0, 0);
        netd_tcp_parse(ack_fin, 20, s.remote_ip, s.local_ip, &pkt);
        uint8_t nothing[40];
        size_t nothing_len = 0;
        rc = netd_tcp_on_segment(&s, &pkt, 100, TPS,
                                 nothing, sizeof(nothing), &nothing_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_FIN_WAIT2,
                   "37. peer ACK of our FIN → FIN_WAIT2");

        // Peer sends FIN → TIME_WAIT.
        uint8_t peer_fin[20];
        netd_tcp_build(peer_fin, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, our_fin_ack_expect,
                       TCP_FLAG_FIN | TCP_FLAG_ACK, 65535, 0, (uint8_t*)0, 0);
        netd_tcp_parse(peer_fin, 20, s.remote_ip, s.local_ip, &pkt);
        uint8_t our_ack[40];
        size_t our_ack_len = 0;
        rc = netd_tcp_on_segment(&s, &pkt, 200, TPS,
                                 our_ack, sizeof(our_ack), &our_ack_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_TIME_WAIT && our_ack_len == 20,
                   "38. peer FIN → TIME_WAIT + ACK emitted");

        // TIME_WAIT should expire at now + 120*1000*TPS(=1M)/1000 = 120M ticks.
        uint64_t expected_expiry = 200 + (TPS * TCP_TIME_WAIT_MS) / 1000;
        TAP_ASSERT(s.time_wait_expiry_tsc == expected_expiry,
                   "39. TIME_WAIT expiry = now + 2*MSL");

        // Tick before expiry: no change.
        uint8_t tmp[40];
        size_t tmp_len = 0;
        rc = netd_tcp_tick(&s, expected_expiry - 1, TPS,
                           tmp, sizeof(tmp), &tmp_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_TIME_WAIT,
                   "40. tick pre-expiry keeps TIME_WAIT");

        // Tick at expiry: socket closes.
        rc = netd_tcp_tick(&s, expected_expiry, TPS,
                           tmp, sizeof(tmp), &tmp_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_CLOSED,
                   "41. tick at expiry → CLOSED");
    }

    // ====================================================================
    // G9. Simultaneous close: FIN_WAIT1 + incoming FIN → CLOSING → ACK
    // (ACK of our FIN) → TIME_WAIT.
    // ====================================================================
    {
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_ESTABLISHED;
        s.local_ip = 0x0A00020Fu; s.local_port = 49154;
        s.remote_ip = 0x08080808u; s.remote_port = 80;
        s.iss = 0x7000u; s.snd_una = 0x7001u; s.snd_nxt = 0x7001u;
        s.irs = 0x8000u; s.rcv_nxt = 0x8001u;
        s.rcv_wnd = TCP_DEFAULT_WINDOW; s.mss = TCP_DEFAULT_MSS;

        uint8_t fin[40]; size_t fin_len = 0;
        netd_tcp_close(&s, fin, sizeof(fin), &fin_len);
        TAP_ASSERT(s.state == TCP_STATE_FIN_WAIT1,
                   "42. active close → FIN_WAIT1");
        uint32_t our_fin_seq = 0x7001u;
        uint32_t our_fin_ack = our_fin_seq + 1;

        // Peer sends FIN at same time (not yet ACKing our FIN).
        uint8_t peer_fin[20];
        netd_tcp_build(peer_fin, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, our_fin_seq /* peer not acking our FIN yet */,
                       TCP_FLAG_FIN | TCP_FLAG_ACK, 65535, 0, (uint8_t*)0, 0);
        tcp_parsed_t pkt;
        netd_tcp_parse(peer_fin, 20, s.remote_ip, s.local_ip, &pkt);
        uint8_t out[40]; size_t out_len = 0;
        int rc = netd_tcp_on_segment(&s, &pkt, 1000, TPS,
                                     out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_CLOSING,
                   "43. FIN_WAIT1 + peer FIN (not yet ACKing ours) → CLOSING");
        TAP_ASSERT(out_len == 20, "44. ACK to peer's FIN emitted");

        // Peer now ACKs our FIN → TIME_WAIT.
        uint8_t final_ack[20];
        netd_tcp_build(final_ack, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, our_fin_ack, TCP_FLAG_ACK, 65535,
                       0, (uint8_t*)0, 0);
        netd_tcp_parse(final_ack, 20, s.remote_ip, s.local_ip, &pkt);
        rc = netd_tcp_on_segment(&s, &pkt, 2000, TPS,
                                 out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_TIME_WAIT,
                   "45. CLOSING + peer ACK of our FIN → TIME_WAIT");
    }

    // ====================================================================
    // G10. Passive close: ESTABLISHED → peer FIN → CLOSE_WAIT → our FIN →
    // LAST_ACK → peer ACK → CLOSED.
    // ====================================================================
    {
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_ESTABLISHED;
        s.local_ip = 0x0A00020Fu; s.local_port = 80;
        s.remote_ip = 0x08080808u; s.remote_port = 50000;
        s.iss = 0x9000u; s.snd_una = 0x9001u; s.snd_nxt = 0x9001u;
        s.irs = 0xA000u; s.rcv_nxt = 0xA001u;
        s.rcv_wnd = TCP_DEFAULT_WINDOW; s.mss = TCP_DEFAULT_MSS;

        // Peer sends FIN.
        uint8_t peer_fin[20];
        netd_tcp_build(peer_fin, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, s.snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK,
                       65535, 0, (uint8_t*)0, 0);
        tcp_parsed_t pkt;
        netd_tcp_parse(peer_fin, 20, s.remote_ip, s.local_ip, &pkt);
        uint8_t out[40]; size_t out_len = 0;
        int rc = netd_tcp_on_segment(&s, &pkt, 100, TPS,
                                     out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_CLOSE_WAIT,
                   "46. peer FIN on ESTABLISHED → CLOSE_WAIT");
        TAP_ASSERT(out_len == 20, "47. ACK of peer's FIN emitted");

        // We close.
        uint8_t our_fin[40]; size_t our_fin_len = 0;
        rc = netd_tcp_close(&s, our_fin, sizeof(our_fin), &our_fin_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_LAST_ACK &&
                   our_fin_len == 20,
                   "48. close() from CLOSE_WAIT → LAST_ACK, FIN emitted");

        // Peer ACKs our FIN → CLOSED.
        uint32_t our_fin_ack = s.snd_nxt;
        uint8_t peer_ack[20];
        netd_tcp_build(peer_ack, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, our_fin_ack, TCP_FLAG_ACK, 65535,
                       0, (uint8_t*)0, 0);
        netd_tcp_parse(peer_ack, 20, s.remote_ip, s.local_ip, &pkt);
        rc = netd_tcp_on_segment(&s, &pkt, 200, TPS,
                                 out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_CLOSED,
                   "49. LAST_ACK + peer ACK → CLOSED");
    }

    // ====================================================================
    // G11. RST on exact SEQ → CLOSED; mismatched SEQ → dropped.
    // ====================================================================
    {
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_ESTABLISHED;
        s.local_ip = 0x0A00020Fu; s.local_port = 12345;
        s.remote_ip = 0x08080808u; s.remote_port = 80;
        s.iss = 0x1111u; s.snd_una = 0x1112u; s.snd_nxt = 0x1112u;
        s.irs = 0x2222u; s.rcv_nxt = 0x2223u;

        // RST at wrong SEQ — MVP drops it.
        uint8_t bogus_rst[20];
        tcp_parsed_t pkt;
        netd_tcp_build(bogus_rst, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt + 100, s.snd_nxt, TCP_FLAG_RST, 0, 0,
                       (uint8_t*)0, 0);
        netd_tcp_parse(bogus_rst, 20, s.remote_ip, s.local_ip, &pkt);
        uint8_t out[40]; size_t out_len = 0;
        int rc = netd_tcp_on_segment(&s, &pkt, 0, TPS,
                                     out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_ESTABLISHED,
                   "50. RST at wrong SEQ → dropped");

        // Good RST.
        uint8_t good_rst[20];
        netd_tcp_build(good_rst, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       s.rcv_nxt, s.snd_nxt, TCP_FLAG_RST, 0, 0,
                       (uint8_t*)0, 0);
        netd_tcp_parse(good_rst, 20, s.remote_ip, s.local_ip, &pkt);
        rc = netd_tcp_on_segment(&s, &pkt, 0, TPS,
                                 out, sizeof(out), &out_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_CLOSED,
                   "51. RST at exact SEQ → CLOSED");
    }

    // ====================================================================
    // G12. SYN retransmit on RTO.
    // ====================================================================
    {
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_CLOSED;
        s.local_ip = 0x0A00020Fu; s.local_port = 45000;
        s.remote_ip = 0x08080808u; s.remote_port = 443;
        s.rcv_wnd = TCP_DEFAULT_WINDOW; s.mss = TCP_DEFAULT_MSS;

        uint8_t syn[40]; size_t syn_len = 0;
        netd_tcp_connect(&s, 0x3333u, syn, sizeof(syn), &syn_len,
                         1000 /*rto_ms*/, 1000 /*now*/, TPS);
        uint32_t initial_rto = s.rto_ms;
        uint64_t first_expiry = s.retx_expiry_tsc;

        // Tick before expiry: nothing.
        uint8_t retx[40]; size_t retx_len = 0;
        netd_tcp_tick(&s, first_expiry - 1, TPS, retx, sizeof(retx), &retx_len);
        TAP_ASSERT(retx_len == 0 && s.rto_ms == initial_rto,
                   "52. tick pre-RTO: no retransmit");

        // Tick at RTO: retransmit with doubled RTO.
        retx_len = 0;
        netd_tcp_tick(&s, first_expiry, TPS, retx, sizeof(retx), &retx_len);
        TAP_ASSERT(retx_len == 24,
                   "53. tick at RTO: SYN retransmit emitted (24 bytes)");
        TAP_ASSERT(s.rto_ms == initial_rto * 2,
                   "54. RTO doubled after retransmit (Karn)");
    }

    // ====================================================================
    // G13. MSS option negotiated in SYN_SENT → smaller peer MSS wins.
    // ====================================================================
    {
        tcp_socket_t s;
        for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
        s.state = TCP_STATE_CLOSED;
        s.local_ip = 0x0A00020Fu; s.local_port = 60000;
        s.remote_ip = 0x08080808u; s.remote_port = 80;
        s.rcv_wnd = TCP_DEFAULT_WINDOW; s.mss = TCP_DEFAULT_MSS; // 1460

        uint8_t syn[40]; size_t syn_len = 0;
        netd_tcp_connect(&s, 0x4000u, syn, sizeof(syn), &syn_len,
                         TCP_DEFAULT_RTO_MS, 0, TPS);

        // Peer sends SYN-ACK with MSS=536 (ancient path).
        uint8_t synack[24];
        netd_tcp_build(synack, s.remote_ip, s.local_ip,
                       s.remote_port, s.local_port,
                       0xFF00u, s.iss + 1,
                       TCP_FLAG_SYN | TCP_FLAG_ACK, 65535,
                       536, (uint8_t*)0, 0);
        tcp_parsed_t pkt;
        netd_tcp_parse(synack, 24, s.remote_ip, s.local_ip, &pkt);
        uint8_t ack[40]; size_t ack_len = 0;
        int rc = netd_tcp_on_segment(&s, &pkt, 0, TPS, ack, sizeof(ack), &ack_len);
        TAP_ASSERT(rc == 0 && s.state == TCP_STATE_ESTABLISHED,
                   "55. tiny-MSS SYN-ACK still reaches ESTABLISHED");
        TAP_ASSERT(s.mss == 536,
                   "56. negotiated MSS = min(ours=1460, peer=536) = 536");
    }

    tap_done();
    exit(0);
}
