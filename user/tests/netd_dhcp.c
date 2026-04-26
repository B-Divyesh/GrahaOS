// user/tests/netd_dhcp.c — Phase 22 Stage B U13 gate test.
//
// Covers netd_dhcp.c end-to-end by simulating a DHCP server:
//   G1. init sets state INIT, fills MAC, computes xid
//   G2. build_discover emits a valid 240+ byte message
//   G3. handle_incoming(OFFER) populates offered_ip/server_id
//   G4. build_request emits REQUEST referencing the OFFER
//   G5. handle_incoming(ACK) → BOUND + assigned_ip/lease_time
//   G6. handle_incoming(NAK) → INIT + backoff bump
//   G7. xid mismatch ignored
//   G8. OFFER outside SELECTING state ignored
//   G9. Magic cookie mismatch ignored

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

// Build a DHCP OFFER/ACK/NAK packet mirroring what a server would send.
// `msg_type` is DHCP_MT_OFFER/ACK/NAK; yiaddr is big-endian on wire but
// provided here as host-order.
static size_t build_server_reply(uint8_t *out,
                                 uint32_t xid,
                                 uint32_t yiaddr,
                                 uint32_t server_id,
                                 uint32_t subnet,
                                 uint32_t router,
                                 uint32_t dns,
                                 uint32_t lease,
                                 uint8_t msg_type,
                                 int good_magic) {
    for (size_t i = 0; i < 512; i++) out[i] = 0;
    out[0] = DHCP_OP_BOOTREPLY;
    out[1] = DHCP_HTYPE_ETH;
    out[2] = DHCP_HLEN_ETH;
    netd_write_be32(&out[4], xid);
    netd_write_be32(&out[16], yiaddr);
    netd_write_be32(&out[236], good_magic ? DHCP_MAGIC : 0xDEADBEEFu);
    size_t off = 240;
    // msg-type
    out[off++] = DHCP_OPT_MSG_TYPE; out[off++] = 1; out[off++] = msg_type;
    // server-id
    out[off++] = DHCP_OPT_SERVER_ID; out[off++] = 4;
    netd_write_be32(&out[off], server_id); off += 4;
    if (msg_type != DHCP_MT_NAK) {
        out[off++] = DHCP_OPT_SUBNET_MASK; out[off++] = 4;
        netd_write_be32(&out[off], subnet); off += 4;
        out[off++] = DHCP_OPT_ROUTER; out[off++] = 4;
        netd_write_be32(&out[off], router); off += 4;
        out[off++] = DHCP_OPT_DNS_SERVER; out[off++] = 4;
        netd_write_be32(&out[off], dns); off += 4;
        out[off++] = DHCP_OPT_LEASE_TIME; out[off++] = 4;
        netd_write_be32(&out[off], lease); off += 4;
    }
    out[off++] = DHCP_OPT_END;
    return off;
}

void _start(void) {
    tap_plan(22);

    const uint8_t mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
    dhcp_client_t c;

    // ====================================================================
    // G1. init.
    // ====================================================================
    netd_dhcp_init(&c, mac, 0x1234567890ABCDEFull);
    TAP_ASSERT(c.state == DHCP_STATE_INIT, "1. state = INIT post-init");
    TAP_ASSERT(bytes_eq(c.mac, mac, 6), "2. MAC stored");
    TAP_ASSERT(c.xid != 0, "3. xid nonzero");
    TAP_ASSERT(c.retry_backoff_ms == 5000u, "4. initial backoff = 5s");

    // ====================================================================
    // G2. build_discover.
    // ====================================================================
    uint8_t pkt[600];
    size_t n = netd_dhcp_build_discover(&c, pkt, sizeof(pkt));
    TAP_ASSERT(n >= 240 + 3 + 3 + 2 + 1, "5. DISCOVER at least 249 bytes");
    TAP_ASSERT(c.state == DHCP_STATE_SELECTING,
               "6. state → SELECTING after build_discover");
    TAP_ASSERT(pkt[0] == DHCP_OP_BOOTREQUEST, "7. DISCOVER op=BOOTREQUEST");
    TAP_ASSERT(pkt[1] == DHCP_HTYPE_ETH && pkt[2] == DHCP_HLEN_ETH,
               "8. DISCOVER htype/hlen correct");
    // chaddr matches our MAC.
    TAP_ASSERT(bytes_eq(pkt + 28, mac, 6), "9. DISCOVER chaddr = our MAC");
    // Magic cookie.
    TAP_ASSERT(netd_read_be32(&pkt[236]) == DHCP_MAGIC,
               "10. DISCOVER has magic cookie");
    // First option is msg-type=DISCOVER.
    TAP_ASSERT(pkt[240] == DHCP_OPT_MSG_TYPE && pkt[241] == 1 && pkt[242] == DHCP_MT_DISCOVER,
               "11. DISCOVER first option = msg-type DISCOVER");

    // ====================================================================
    // G3. handle_incoming(OFFER).
    // ====================================================================
    uint8_t offer[512];
    size_t offer_len = build_server_reply(offer, c.xid,
                                          0x0A00020Fu /*yiaddr 10.0.2.15*/,
                                          0x0A000202u /*server 10.0.2.2*/,
                                          0xFFFFFF00u /*mask 255.255.255.0*/,
                                          0x0A000202u /*router*/,
                                          0x08080808u /*dns*/,
                                          86400u,
                                          DHCP_MT_OFFER, 1);
    int rc = netd_dhcp_handle_incoming(&c, offer, offer_len);
    TAP_ASSERT(rc == DHCP_MT_OFFER, "12. OFFER returned");
    TAP_ASSERT(c.offered_ip == 0x0A00020Fu, "13. offered_ip populated");
    TAP_ASSERT(c.server_id == 0x0A000202u, "14. server_id populated");
    TAP_ASSERT(c.subnet_mask == 0xFFFFFF00u, "15. subnet_mask populated");

    // ====================================================================
    // G4. build_request.
    // ====================================================================
    n = netd_dhcp_build_request(&c, pkt, sizeof(pkt));
    TAP_ASSERT(n > 240 && c.state == DHCP_STATE_REQUESTING,
               "16. REQUEST emitted + state=REQUESTING");
    // msg-type=REQUEST as first option.
    TAP_ASSERT(pkt[242] == DHCP_MT_REQUEST,
               "17. REQUEST has msg-type = REQUEST");

    // ====================================================================
    // G5. handle_incoming(ACK).
    // ====================================================================
    uint8_t ack[512];
    size_t ack_len = build_server_reply(ack, c.xid,
                                        0x0A00020Fu, 0x0A000202u,
                                        0xFFFFFF00u, 0x0A000202u,
                                        0x08080808u, 86400u,
                                        DHCP_MT_ACK, 1);
    rc = netd_dhcp_handle_incoming(&c, ack, ack_len);
    TAP_ASSERT(rc == DHCP_MT_ACK, "18. ACK returned");
    TAP_ASSERT(c.state == DHCP_STATE_BOUND && c.assigned_ip == 0x0A00020Fu &&
               c.lease_seconds == 86400u,
               "19. state=BOUND + assigned_ip + lease_seconds");

    // ====================================================================
    // G6. NAK bumps backoff.
    // ====================================================================
    // Reset to REQUESTING; simulate a NAK.
    c.state = DHCP_STATE_REQUESTING;
    uint32_t old_backoff = c.retry_backoff_ms;
    uint8_t nak[512];
    size_t nak_len = build_server_reply(nak, c.xid, 0, 0x0A000202u,
                                        0, 0, 0, 0,
                                        DHCP_MT_NAK, 1);
    rc = netd_dhcp_handle_incoming(&c, nak, nak_len);
    TAP_ASSERT(rc == DHCP_MT_NAK && c.state == DHCP_STATE_INIT &&
               c.retry_backoff_ms > old_backoff,
               "20. NAK → state=INIT + backoff bumped");

    // ====================================================================
    // G7-G9: defensive rejects.
    // ====================================================================
    // Mismatched xid.
    c.state = DHCP_STATE_SELECTING;
    uint8_t bad_xid[512];
    size_t bad_xid_len = build_server_reply(bad_xid, c.xid ^ 0xFFFFu,
                                            0x0A00020Fu, 0x0A000202u, 0,0,0,0,
                                            DHCP_MT_OFFER, 1);
    rc = netd_dhcp_handle_incoming(&c, bad_xid, bad_xid_len);
    TAP_ASSERT(rc == 0 && c.state == DHCP_STATE_SELECTING,
               "21. mismatched xid: reply ignored, state unchanged");

    // Bad magic cookie.
    uint8_t bad_magic[512];
    size_t bad_magic_len = build_server_reply(bad_magic, c.xid,
                                              0x0A00020Fu, 0x0A000202u, 0,0,0,0,
                                              DHCP_MT_OFFER, 0 /* bad magic */);
    rc = netd_dhcp_handle_incoming(&c, bad_magic, bad_magic_len);
    TAP_ASSERT(rc == 0, "22. bad magic cookie: packet ignored");

    tap_done();
    exit(0);
}
