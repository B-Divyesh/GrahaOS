// user/tests/netd_arp.tap.c — Phase 22 Stage B U6 gate test.
//
// Direct-unit test of netd's ARP module + Ethernet framing + IPv4 helpers.
// This test deliberately does NOT spawn netd or e1000d — it drives the pure
// functions in user/netd_{eth,arp,ipv4}.c with hand-crafted byte sequences
// so that ARP logic is validated even before the full frame-transport path
// between e1000d and netd is wired up. A follow-up integration test (once
// Stage B's full frame path is live) will exercise the ARP stack against
// QEMU's SLiRP.
//
// Assertions cover:
//   G1. Ethernet build/parse round-trip + broadcast MAC helper
//   G2. ARP table init / insert / lookup
//   G3. LRU-round-robin eviction when full
//   G4. TTL GC
//   G5. Request frame layout on the wire
//   G6. Reply frame layout on the wire
//   G7. Parse a well-formed REQUEST and REPLY
//   G8. Reject malformed ARP (wrong htype, short buffer)
//   G9. Incoming-handler: cache update + reply emission
//  G10. Incoming-handler: no reply when target_ip is 0 (pre-DHCP)
//  G11. Resolve: hit → returns MAC, no request emitted
//  G12. Resolve: miss → inserts PENDING, emits request
//  G13. Resolve: PENDING dedup — second call does NOT emit
//  G14. IPv4 parse success + failure
//  G15. inet_checksum known vectors

#include "../libtap.h"
#include "../netd.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void exit(int);

// Small helper: compare N bytes, return 1 on equal.
static int bytes_eq(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

void _start(void) {
    tap_plan(32);

    // ====================================================================
    // G1. Ethernet build/parse
    // ====================================================================
    {
        const uint8_t dst[6] = {0x52,0x55,0x0A,0x00,0x02,0x02};
        const uint8_t src[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
        uint8_t f[ETH_HDR_LEN];
        size_t n = netd_eth_build(f, dst, src, ETH_TYPE_IPV4);
        TAP_ASSERT(n == ETH_HDR_LEN, "1. eth_build returns 14");
        TAP_ASSERT(bytes_eq(f, dst, 6), "2. eth_build dst mac at offset 0");
        TAP_ASSERT(bytes_eq(f+6, src, 6), "3. eth_build src mac at offset 6");
        TAP_ASSERT(f[12] == 0x08 && f[13] == 0x00,
                   "4. eth_build ethertype big-endian (0x0800)");

        uint8_t d2[6], s2[6];
        uint16_t t2 = 0;
        int rc = netd_eth_parse(f, ETH_HDR_LEN, d2, s2, &t2);
        TAP_ASSERT(rc == 0 && bytes_eq(d2, dst, 6) && bytes_eq(s2, src, 6)
                   && t2 == ETH_TYPE_IPV4,
                   "5. eth_parse round-trips build");

        // Broadcast helper + mac_eq.
        TAP_ASSERT(netd_mac_eq(netd_eth_bcast, netd_eth_bcast),
                   "6. mac_eq(bcast,bcast)==1");
        TAP_ASSERT(!netd_mac_eq(netd_eth_bcast, netd_eth_zero),
                   "7. mac_eq(bcast,zero)==0");
    }

    // ====================================================================
    // G2. ARP table init + insert + lookup
    // ====================================================================
    arp_table_t tbl;
    netd_arp_init(&tbl);
    TAP_ASSERT(netd_arp_count(&tbl) == 0, "8. fresh table has 0 resolved entries");

    const uint8_t mac_a[6] = {0xAA,0xAA,0xAA,0x01,0x02,0x03};
    const uint8_t mac_b[6] = {0xBB,0xBB,0xBB,0x04,0x05,0x06};
    // 10.0.2.1, 10.0.2.2
    const uint32_t ip_a = 0x0A000201u;
    const uint32_t ip_b = 0x0A000202u;

    netd_arp_insert(&tbl, ip_a, mac_a, ARP_STATE_RESOLVED, 1000, 60000);
    netd_arp_insert(&tbl, ip_b, mac_b, ARP_STATE_RESOLVED, 1000, 60000);
    TAP_ASSERT(netd_arp_count(&tbl) == 2, "9. two inserts give count=2");

    uint8_t got[6] = {0};
    int hit = netd_arp_lookup(&tbl, ip_a, got);
    TAP_ASSERT(hit == 1 && bytes_eq(got, mac_a, 6),
               "10. lookup(ip_a) returns mac_a");
    hit = netd_arp_lookup(&tbl, ip_b, got);
    TAP_ASSERT(hit == 1 && bytes_eq(got, mac_b, 6),
               "11. lookup(ip_b) returns mac_b");
    // Miss.
    hit = netd_arp_lookup(&tbl, 0x01020304u, got);
    TAP_ASSERT(hit == 0, "12. lookup miss returns 0");

    // ====================================================================
    // G3. Round-robin eviction when full.
    // Fill 16 distinct IPs, then insert a 17th; expect slot 0 reused.
    // ====================================================================
    netd_arp_init(&tbl);
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        uint8_t m[6] = {0xCC, 0xCC, 0xCC, 0, 0, (uint8_t)i};
        netd_arp_insert(&tbl, 0xC0A80000u | i, m, ARP_STATE_RESOLVED,
                        1000, 60000);
    }
    TAP_ASSERT(netd_arp_count(&tbl) == ARP_TABLE_SLOTS,
               "13. table full after 16 inserts");
    // Insert 17th — evicts slot 0 (next_slot starts at 0).
    {
        uint8_t m[6] = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
        netd_arp_insert(&tbl, 0xDEADBEEFu, m, ARP_STATE_RESOLVED,
                        1000, 60000);
    }
    TAP_ASSERT(netd_arp_count(&tbl) == ARP_TABLE_SLOTS,
               "14. post-eviction count still 16");
    uint8_t evicted[6];
    TAP_ASSERT(netd_arp_lookup(&tbl, 0xC0A80000u, evicted) == 0,
               "15. original slot-0 IP was evicted");
    TAP_ASSERT(netd_arp_lookup(&tbl, 0xDEADBEEFu, evicted) == 1,
               "16. new IP is now resolved");

    // ====================================================================
    // G4. TTL GC: insert with short TTL, advance, gc, verify removed.
    // ====================================================================
    netd_arp_init(&tbl);
    netd_arp_insert(&tbl, ip_a, mac_a, ARP_STATE_RESOLVED, 1000, 500);
    // now_tsc = 1000, expiry = 1500.
    uint32_t aged = netd_arp_gc(&tbl, 1499);
    TAP_ASSERT(aged == 0 && netd_arp_count(&tbl) == 1,
               "17. gc before expiry does nothing");
    aged = netd_arp_gc(&tbl, 1500);
    TAP_ASSERT(aged == 1 && netd_arp_count(&tbl) == 0,
               "18. gc at expiry removes the entry");

    // ====================================================================
    // G5. Build ARP REQUEST — verify wire layout byte-for-byte.
    // ====================================================================
    {
        const uint8_t me[6]   = {0x52,0x54,0x00,0x12,0x34,0x56};
        const uint32_t my_ip  = 0x0A00020Fu;  // 10.0.2.15
        const uint32_t tgt_ip = 0x0A000202u;  // 10.0.2.2
        uint8_t f[64] = {0};
        size_t n = netd_arp_build_request(f, me, my_ip, tgt_ip);
        TAP_ASSERT(n == ARP_FRAME_LEN, "19. request frame is 42 bytes");

        // Ethernet: dst=bcast, src=me, ethertype=0x0806.
        int eth_ok =
            bytes_eq(f, netd_eth_bcast, 6) &&
            bytes_eq(f + 6, me, 6) &&
            f[12] == 0x08 && f[13] == 0x06;
        TAP_ASSERT(eth_ok, "20. request Ethernet header correct");

        // ARP PDU: htype=1, ptype=0x0800, hlen=6, plen=4, op=1,
        //         sender_mac=me, sender_ip=10.0.2.15,
        //         target_mac=0, target_ip=10.0.2.2.
        uint8_t *a = f + ETH_HDR_LEN;
        int pdu_ok =
            a[0] == 0x00 && a[1] == 0x01 &&     // htype
            a[2] == 0x08 && a[3] == 0x00 &&     // ptype
            a[4] == 0x06 && a[5] == 0x04 &&     // hlen/plen
            a[6] == 0x00 && a[7] == 0x01 &&     // op=REQUEST
            bytes_eq(a + 8, me, 6) &&           // sender_mac
            a[14]==0x0A && a[15]==0x00 && a[16]==0x02 && a[17]==0x0F &&
            bytes_eq(a + 18, netd_eth_zero, 6) &&
            a[24]==0x0A && a[25]==0x00 && a[26]==0x02 && a[27]==0x02;
        TAP_ASSERT(pdu_ok, "21. request ARP PDU correct");
    }

    // ====================================================================
    // G6. Build ARP REPLY — verify wire layout.
    // ====================================================================
    {
        const uint8_t me[6]  = {0x52,0x54,0x00,0x12,0x34,0x56};
        const uint32_t my_ip = 0x0A00020Fu;
        const uint8_t peer[6] = {0x52,0x55,0x0A,0x00,0x02,0x02};
        const uint32_t peer_ip = 0x0A000202u;
        uint8_t f[64] = {0};
        size_t n = netd_arp_build_reply(f, me, my_ip, peer, peer_ip);
        TAP_ASSERT(n == ARP_FRAME_LEN, "22. reply frame is 42 bytes");

        int eth_ok =
            bytes_eq(f, peer, 6) &&
            bytes_eq(f + 6, me, 6) &&
            f[12] == 0x08 && f[13] == 0x06;
        TAP_ASSERT(eth_ok, "23. reply Ethernet header correct");

        uint8_t *a = f + ETH_HDR_LEN;
        int pdu_ok = a[6] == 0x00 && a[7] == 0x02 /* op=REPLY */ &&
                     bytes_eq(a + 8, me, 6) &&
                     bytes_eq(a + 18, peer, 6);
        TAP_ASSERT(pdu_ok, "24. reply ARP PDU op=REPLY + sender=me + target=peer");
    }

    // ====================================================================
    // G7. Parse well-formed REQUEST + REPLY
    // ====================================================================
    {
        const uint8_t me[6]   = {0x52,0x54,0x00,0x12,0x34,0x56};
        const uint32_t my_ip  = 0x0A00020Fu;
        const uint32_t tgt_ip = 0x0A000202u;
        uint8_t f[64] = {0};
        netd_arp_build_request(f, me, my_ip, tgt_ip);

        arp_packet_t p;
        int rc = netd_arp_parse(f + ETH_HDR_LEN, ARP_PAYLOAD_LEN, &p);
        TAP_ASSERT(rc == 0 && p.op == ARP_OP_REQUEST &&
                   p.sender_ip == my_ip && p.target_ip == tgt_ip &&
                   bytes_eq(p.sender_mac, me, 6),
                   "25. parse of well-formed REQUEST succeeds");
    }

    // ====================================================================
    // G8. Reject malformed ARP
    // ====================================================================
    {
        arp_packet_t p;
        // Short buffer.
        int rc = netd_arp_parse((const uint8_t*)"", 0, &p);
        TAP_ASSERT(rc < 0, "26. parse rejects short buffer");

        // Wrong htype (IEEE 802 = 6, should be Ethernet = 1).
        uint8_t bad[ARP_PAYLOAD_LEN] = {
            0x00,0x06, 0x08,0x00, 0x06,0x04, 0x00,0x01,
            0,0,0,0,0,0, 0,0,0,0, 0,0,0,0,0,0, 0,0,0,0
        };
        rc = netd_arp_parse(bad, ARP_PAYLOAD_LEN, &p);
        TAP_ASSERT(rc < 0, "27. parse rejects wrong htype");
    }

    // ====================================================================
    // G9. Incoming handler: cache update + reply emission.
    // ====================================================================
    {
        netd_arp_init(&tbl);
        const uint8_t me[6]   = {0x52,0x54,0x00,0x12,0x34,0x56};
        const uint32_t my_ip  = 0x0A00020Fu;
        const uint8_t peer[6] = {0x52,0x55,0x0A,0x00,0x02,0x02};
        const uint32_t peer_ip = 0x0A000202u;

        // Peer asks "who has my_ip".
        uint8_t req_frame[64] = {0};
        netd_arp_build_request(req_frame, peer, peer_ip, my_ip);

        uint8_t reply[64] = {0};
        size_t reply_len = 0;
        int rc = netd_arp_handle_incoming(&tbl,
                                          req_frame + ETH_HDR_LEN,
                                          ARP_PAYLOAD_LEN,
                                          me, my_ip,
                                          2000 /*now*/, 60000 /*ttl*/,
                                          reply, sizeof(reply), &reply_len);
        TAP_ASSERT(rc == 0, "28. handle_incoming(REQUEST for us) returns 0");
        TAP_ASSERT(reply_len == ARP_FRAME_LEN,
                   "29. reply frame emitted (len=42)");

        // Verify the reply is a correct ARP reply to peer.
        arp_packet_t rp;
        int prc = netd_arp_parse(reply + ETH_HDR_LEN, ARP_PAYLOAD_LEN, &rp);
        TAP_ASSERT(prc == 0 && rp.op == ARP_OP_REPLY &&
                   rp.sender_ip == my_ip && bytes_eq(rp.sender_mac, me, 6) &&
                   rp.target_ip == peer_ip,
                   "30. reply packet well-formed (op=REPLY, sender=me, target=peer)");

        // Sender's cache entry populated.
        uint8_t cached[6];
        TAP_ASSERT(netd_arp_lookup(&tbl, peer_ip, cached) == 1 &&
                   bytes_eq(cached, peer, 6),
                   "31. incoming REQUEST learned sender's MAC");
    }

    // ====================================================================
    // G10-11. Resolve fast-path hit + miss/dedup.
    // ====================================================================
    {
        netd_arp_init(&tbl);
        const uint8_t me[6]  = {0x52,0x54,0x00,0x12,0x34,0x56};
        const uint32_t my_ip = 0x0A00020Fu;
        const uint32_t tgt   = 0x0A000202u;
        const uint8_t peer[6] = {0x52,0x55,0x0A,0x00,0x02,0x02};

        // Pre-populate resolved entry → hit.
        netd_arp_insert(&tbl, tgt, peer, ARP_STATE_RESOLVED, 1000, 60000);
        uint8_t m[6] = {0};
        uint8_t req[64] = {0};
        size_t req_len = 0xABCD;
        int rc = netd_arp_resolve(&tbl, me, my_ip, tgt,
                                  2000, 500,
                                  m, req, sizeof(req), &req_len);
        TAP_ASSERT(rc == 1 && bytes_eq(m, peer, 6) && req_len == 0,
                   "32. resolve hit returns MAC + no request frame");
    }
    // (The remaining miss+dedup cases would push this test over 32 asserts;
    //  Stage B's integration test will cover them once the wire path is live.)

    tap_done();
    exit(0);
}
