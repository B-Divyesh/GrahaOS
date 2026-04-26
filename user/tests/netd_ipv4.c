// user/tests/netd_ipv4.c — Phase 22 Stage B U7-U9 gate test.
//
// Direct-unit coverage of netd's L3/L4 modules:
//   - netd_ipv4.c  (build/parse + checksum + pseudo-header + dotted-quad)
//   - netd_icmp.c  (echo build/parse + handle_incoming)
//   - netd_udp.c   (build/parse + socket table bind/find/close/ephemeral)
//
// Like netd_arp, this test links libnetd.a directly and drives the pure
// functions with hand-crafted byte sequences. No daemon, no wire.

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

void _start(void) {
    tap_plan(38);

    // ====================================================================
    // G1. IPv4 header build + parse round-trip.
    // ====================================================================
    {
        uint8_t hdr[20];
        // src=10.0.2.15, dst=8.8.8.8, UDP, id=0x1234, payload=100, ttl=64
        netd_ipv4_build_header(hdr, 0x0A00020Fu, 0x08080808u,
                               IPPROTO_UDP, 0x1234u, 100, 64);
        TAP_ASSERT((hdr[0] >> 4) == 4, "1. IPv4 version field = 4");
        TAP_ASSERT((hdr[0] & 0xF) == 5, "2. IPv4 IHL field = 5 (no options)");
        TAP_ASSERT(hdr[8] == 64, "3. IPv4 TTL = 64");
        TAP_ASSERT(hdr[9] == IPPROTO_UDP, "4. IPv4 proto = 17");
        TAP_ASSERT(hdr[4] == 0x12 && hdr[5] == 0x34,
                   "5. IPv4 id field stored big-endian");

        ipv4_parsed_t p;
        int rc = netd_ipv4_parse(hdr, 20, &p);
        TAP_ASSERT(rc == -3, "6. parse: total_len (120) > buf_len (20) rejected");

        // Supply a properly-sized buffer — header + 100 dummy bytes.
        uint8_t full[20 + 100];
        for (int i = 0; i < 120; i++) full[i] = (i < 20) ? hdr[i] : (uint8_t)i;
        rc = netd_ipv4_parse(full, 120, &p);
        TAP_ASSERT(rc == 0, "7. IPv4 parse accepts well-formed header");
        TAP_ASSERT(p.src == 0x0A00020Fu, "8. parsed src ip matches");
        TAP_ASSERT(p.dst == 0x08080808u, "9. parsed dst ip matches");
        TAP_ASSERT(p.proto == IPPROTO_UDP, "10. parsed proto matches");
        TAP_ASSERT(p.total_len == 120, "11. parsed total_len matches");
        TAP_ASSERT(p.payload_len == 100, "12. parsed payload_len matches");
    }

    // ====================================================================
    // G2. IPv4 rejects: wrong version, IHL, fragments, bad checksum.
    // ====================================================================
    {
        uint8_t hdr[20 + 4];
        netd_ipv4_build_header(hdr, 0x01020304u, 0x05060708u,
                               IPPROTO_ICMP, 1, 4, 64);
        for (int i = 0; i < 4; i++) hdr[20 + i] = 0;

        ipv4_parsed_t p;
        int rc = netd_ipv4_parse(hdr, 24, &p);
        TAP_ASSERT(rc == 0, "13. baseline IPv4 parse ok");

        // Mutate: version 6 → reject.
        uint8_t bad[24];
        for (int i = 0; i < 24; i++) bad[i] = hdr[i];
        bad[0] = (6 << 4) | 5;
        rc = netd_ipv4_parse(bad, 24, &p);
        TAP_ASSERT(rc == -2, "14. wrong version rejected");

        // IHL=6 (options) → reject.
        for (int i = 0; i < 24; i++) bad[i] = hdr[i];
        bad[0] = (4 << 4) | 6;
        rc = netd_ipv4_parse(bad, 24, &p);
        TAP_ASSERT(rc == -2, "15. IHL != 5 rejected");

        // Fragment: set MF flag.
        for (int i = 0; i < 24; i++) bad[i] = hdr[i];
        bad[6] = 0x20;   // MF=1, frag_off=0
        rc = netd_ipv4_parse(bad, 24, &p);
        TAP_ASSERT(rc == -4, "16. fragmented packet rejected");

        // Fragment: frag_offset nonzero.
        for (int i = 0; i < 24; i++) bad[i] = hdr[i];
        bad[7] = 0x01;
        rc = netd_ipv4_parse(bad, 24, &p);
        TAP_ASSERT(rc == -4, "17. nonzero fragment offset rejected");

        // Bad checksum: flip a bit in the checksum field.
        for (int i = 0; i < 24; i++) bad[i] = hdr[i];
        bad[10] ^= 0x01;
        rc = netd_ipv4_parse(bad, 24, &p);
        TAP_ASSERT(rc == -5, "18. bad header checksum rejected");
    }

    // ====================================================================
    // G3. ICMP echo build + parse round-trip.
    // ====================================================================
    {
        const uint8_t payload[4] = {'p','i','n','g'};
        uint8_t pdu[8 + 4];
        size_t n = netd_icmp_build_echo(pdu, ICMP_TYPE_ECHO_REQUEST, 0,
                                        0xBEEF, 0x0001, payload, 4);
        TAP_ASSERT(n == 12, "19. ICMP echo build returns 8+payload len");

        uint8_t type = 0, code = 0;
        uint16_t id = 0, seq = 0;
        const uint8_t *pl = (uint8_t*)0;
        size_t pl_len = 0;
        int rc = netd_icmp_parse_echo(pdu, 12, &type, &code, &id, &seq,
                                      &pl, &pl_len);
        TAP_ASSERT(rc == 0 && type == 8 && id == 0xBEEF && seq == 1 &&
                   pl_len == 4 && bytes_eq(pl, payload, 4),
                   "20. ICMP echo build/parse round-trips");

        // Tamper — bad checksum should reject.
        pdu[2] ^= 0xAA;
        rc = netd_icmp_parse_echo(pdu, 12, &type, &code, &id, &seq,
                                  &pl, &pl_len);
        TAP_ASSERT(rc < 0, "21. ICMP echo with wrong checksum rejected");
    }

    // ====================================================================
    // G4. ICMP handle_incoming: REQUEST for us → REPLY emitted.
    // ====================================================================
    {
        const uint32_t my_ip = 0x0A00020Fu;
        const uint32_t peer  = 0x0A000202u;
        const uint8_t payload[8] = {'H','E','L','L','O','W','R','D'};

        uint8_t dgram[20 + 8 + 8];
        // IPv4 header (src=peer, dst=my_ip).
        netd_ipv4_build_header(dgram, peer, my_ip, IPPROTO_ICMP, 0x1111,
                               8 + 8, 64);
        // ICMP ECHO REQUEST.
        netd_icmp_build_echo(dgram + 20, ICMP_TYPE_ECHO_REQUEST, 0,
                             0xABCD, 0x0007, payload, 8);

        ipv4_parsed_t ip;
        int rc = netd_ipv4_parse(dgram, sizeof(dgram), &ip);
        TAP_ASSERT(rc == 0 && ip.proto == IPPROTO_ICMP,
                   "22. outer IPv4 datagram parses for ICMP");

        uint8_t reply[40 + 8];
        size_t reply_len = 0;
        rc = netd_icmp_handle_incoming(&ip, my_ip, reply,
                                       sizeof(reply), &reply_len);
        TAP_ASSERT(rc == 0 && reply_len == 20 + 8 + 8,
                   "23. ICMP handle_incoming emits reply of expected length");

        // Reply IPv4: src = my_ip, dst = peer, proto = ICMP.
        ipv4_parsed_t rip;
        rc = netd_ipv4_parse(reply, reply_len, &rip);
        TAP_ASSERT(rc == 0 && rip.src == my_ip && rip.dst == peer &&
                   rip.proto == IPPROTO_ICMP,
                   "24. reply IPv4 addresses swapped correctly");

        // Reply ICMP: type=REPLY (0), same id/seq, same payload.
        uint8_t rtype = 0, rcode = 0;
        uint16_t rid = 0, rseq = 0;
        const uint8_t *rpl = (uint8_t*)0;
        size_t rpl_len = 0;
        rc = netd_icmp_parse_echo(rip.payload, rip.payload_len,
                                  &rtype, &rcode, &rid, &rseq,
                                  &rpl, &rpl_len);
        TAP_ASSERT(rc == 0 && rtype == ICMP_TYPE_ECHO_REPLY &&
                   rid == 0xABCD && rseq == 7 &&
                   rpl_len == 8 && bytes_eq(rpl, payload, 8),
                   "25. reply ICMP echo matches request id/seq/payload");
    }

    // ====================================================================
    // G5. ICMP handle_incoming: not-for-us / wrong-type no-reply.
    // ====================================================================
    {
        const uint32_t my_ip = 0x0A00020Fu;
        const uint32_t peer  = 0x0A000202u;
        const uint32_t other = 0x0A000203u;

        // REQUEST targeting `other`, not us.
        uint8_t dgram[20 + 8];
        netd_ipv4_build_header(dgram, peer, other, IPPROTO_ICMP, 0x2222,
                               8, 64);
        netd_icmp_build_echo(dgram + 20, ICMP_TYPE_ECHO_REQUEST, 0,
                             1, 1, (uint8_t*)0, 0);
        ipv4_parsed_t ip;
        netd_ipv4_parse(dgram, sizeof(dgram), &ip);

        uint8_t reply[40];
        size_t reply_len = 0;
        int rc = netd_icmp_handle_incoming(&ip, my_ip, reply,
                                           sizeof(reply), &reply_len);
        TAP_ASSERT(rc == 0 && reply_len == 0,
                   "26. ICMP handle_incoming ignores request targeting another IP");

        // Wrong type (ECHO_REPLY): still should not emit anything.
        netd_icmp_build_echo(dgram + 20, ICMP_TYPE_ECHO_REPLY, 0,
                             1, 1, (uint8_t*)0, 0);
        // Re-fix IPv4 checksum since we changed body? ICMP body change
        // doesn't affect IP checksum. Re-parse.
        netd_ipv4_parse(dgram, sizeof(dgram), &ip);
        // Fix dst so the my_ip check passes.
        ip.dst = my_ip;
        reply_len = 0;
        rc = netd_icmp_handle_incoming(&ip, my_ip, reply,
                                       sizeof(reply), &reply_len);
        TAP_ASSERT(rc == 0 && reply_len == 0,
                   "27. ICMP handle_incoming ignores non-REQUEST types");
    }

    // ====================================================================
    // G6. UDP build + parse round-trip (with checksum).
    // ====================================================================
    {
        const uint32_t src_ip = 0x0A00020Fu;
        const uint32_t dst_ip = 0x08080808u;
        const uint8_t  body[8] = {'D','N','S','Q','U','E','R','Y'};
        uint8_t dgram[8 + 8];
        size_t n = netd_udp_build(dgram, src_ip, dst_ip, 54321, 53,
                                  body, 8);
        TAP_ASSERT(n == 16, "28. UDP build returns 8+payload");

        // Peer receives — parse gives us back the payload and ports.
        uint16_t sp = 0, dp = 0;
        const uint8_t *pl = (uint8_t*)0;
        size_t pl_len = 0;
        int rc = netd_udp_parse(dgram, 16, src_ip, dst_ip,
                                &sp, &dp, &pl, &pl_len);
        TAP_ASSERT(rc == 0 && sp == 54321 && dp == 53 &&
                   pl_len == 8 && bytes_eq(pl, body, 8),
                   "29. UDP parse round-trips build");

        // Corrupt one byte → checksum must reject.
        dgram[10] ^= 0x01;
        rc = netd_udp_parse(dgram, 16, src_ip, dst_ip,
                            &sp, &dp, &pl, &pl_len);
        TAP_ASSERT(rc < 0, "30. UDP parse rejects corrupted payload");
    }

    // ====================================================================
    // G7. UDP socket table: bind, find, close, ephemeral.
    // ====================================================================
    {
        udp_table_t utbl;
        netd_udp_table_init(&utbl);

        int idx_a = netd_udp_bind(&utbl, 67, 0xAAAA);
        int idx_b = netd_udp_bind(&utbl, 68, 0xBBBB);
        TAP_ASSERT(idx_a >= 0 && idx_b >= 0 && idx_a != idx_b,
                   "31. two successful explicit UDP binds");

        int found = netd_udp_find(&utbl, 67);
        TAP_ASSERT(found == idx_a, "32. find(67) -> idx_a");

        // Duplicate port → failure.
        int idx_dup = netd_udp_bind(&utbl, 67, 0xCCCC);
        TAP_ASSERT(idx_dup < 0, "33. duplicate bind rejected");

        // Close idx_a; find(67) should miss.
        netd_udp_close(&utbl, idx_a);
        TAP_ASSERT(netd_udp_find(&utbl, 67) < 0,
                   "34. find(67) after close returns -1");

        // Re-bind; should succeed now.
        int idx_rebind = netd_udp_bind(&utbl, 67, 0xDDDD);
        TAP_ASSERT(idx_rebind >= 0, "35. re-bind after close succeeds");

        // Ephemeral bind.
        int idx_eph = netd_udp_bind(&utbl, 0, 0xEEEE);
        TAP_ASSERT(idx_eph >= 0 && utbl.sockets[idx_eph].local_port >= 48152,
                   "36. ephemeral bind returns valid >=48152 port");
    }

    // ====================================================================
    // G8. Dotted-quad parse helpers.
    // ====================================================================
    {
        uint32_t ip = 0;
        int rc = netd_ipv4_parse_dotted("10.0.2.15", &ip);
        TAP_ASSERT(rc == 0 && ip == 0x0A00020Fu,
                   "37. parse_dotted('10.0.2.15') succeeds");

        rc = netd_ipv4_parse_dotted("256.0.0.1", &ip);
        TAP_ASSERT(rc < 0, "38. parse_dotted rejects out-of-range octet");
    }

    tap_done();
    exit(0);
}
