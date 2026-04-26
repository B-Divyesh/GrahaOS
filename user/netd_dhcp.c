// user/netd_dhcp.c — Phase 22 Stage B: DHCP client (RFC 2131).
//
// Happy-path DHCP: INIT → DISCOVER → SELECTING (OFFER) → REQUEST →
// REQUESTING (ACK) → BOUND. Retry policy (DISCOVER timeout / NAK):
// exponential backoff 5 s → 10 s → 20 s → 30 s (capped). After 3 failed
// cycles a static-IP fallback may be tried (not implemented here — caller
// looks at state == DHCP_STATE_FAILED and reads /etc/net/static.conf).
//
// On-wire packet layout (RFC 2131 Figure 1):
//   Offset  Size   Field
//     0      1     op               (BOOTREQUEST=1 / BOOTREPLY=2)
//     1      1     htype            (1 = Ethernet)
//     2      1     hlen             (6 for Ethernet MAC)
//     3      1     hops             (0 for client)
//     4      4     xid              (transaction id, big-endian)
//     8      2     secs             (0; since DHCPDISCOVER)
//    10      2     flags            (0x8000 = broadcast)
//    12      4     ciaddr           (0 except in RENEWING)
//    16      4     yiaddr           (filled by server)
//    20      4     siaddr           (next server IP)
//    24      4     giaddr           (relay agent; 0 for us)
//    28     16     chaddr           (client MAC, zero-padded to 16)
//    44     64     sname            (server hostname; 0)
//   108    128     file             (boot file; 0)
//   236      4     magic            (0x63825363 big-endian)
//   240      *     options          (TLV + END=255)
//
// Minimum packet size is 240 + options. A DISCOVER carries msg-type +
// parameter-request-list + end, so ~250 bytes total. Well under the 1472
// UDP+IPv4 payload limit.

#include "netd.h"

#define DHCP_PKT_FIXED_LEN   240u
#define DHCP_MAX_PAYLOAD     576u    // Typical minimum; enough for our options

static void dhcp_reset_retry(dhcp_client_t *c) {
    c->retry_count      = 0;
    c->retry_backoff_ms = 5000u;    // Initial retry: 5 s (spec D13)
    c->next_retry_tsc   = 0;
}

void netd_dhcp_init(dhcp_client_t *c, const uint8_t my_mac[6], uint64_t now_tsc) {
    if (!c) return;
    netd_memzero(c, sizeof(*c));
    c->state = DHCP_STATE_INIT;
    // Bleak but sufficient: xid = rdtsc low 32 mixed with MAC bytes.
    uint32_t h = (uint32_t)(now_tsc ^ (now_tsc >> 32));
    for (int i = 0; i < 6; i++) h = (h * 0x1000193u) ^ my_mac[i];
    c->xid = h;
    for (int i = 0; i < 6; i++) c->mac[i] = my_mac[i];
    dhcp_reset_retry(c);
}

// ---------------------------------------------------------------------------
// Packet builders.
// ---------------------------------------------------------------------------

// Fill the common 240-byte DHCP header at `out[0..239]` for an outbound
// BOOTREQUEST. Options start at out[240]. Returns 240.
static size_t dhcp_fill_header(dhcp_client_t *c, uint8_t *out,
                               uint32_t ciaddr_hostbyte) {
    netd_memzero(out, DHCP_PKT_FIXED_LEN);
    out[0] = DHCP_OP_BOOTREQUEST;
    out[1] = DHCP_HTYPE_ETH;
    out[2] = DHCP_HLEN_ETH;
    out[3] = 0;   // hops
    netd_write_be32(&out[4], c->xid);
    netd_write_be16(&out[8], 0);     // secs
    netd_write_be16(&out[10], 0x8000u);  // flags.BROADCAST
    netd_write_be32(&out[12], ciaddr_hostbyte);  // ciaddr
    // yiaddr/siaddr/giaddr default to 0 via memzero.
    for (int i = 0; i < 6; i++) out[28 + i] = c->mac[i];   // chaddr
    // chaddr pad already zero.
    netd_write_be32(&out[236], DHCP_MAGIC);
    return DHCP_PKT_FIXED_LEN;
}

static size_t dhcp_append_opt(uint8_t *out, size_t off,
                              uint8_t code, const uint8_t *data, uint8_t len) {
    out[off++] = code;
    out[off++] = len;
    for (uint8_t i = 0; i < len; i++) out[off++] = data[i];
    return off;
}

size_t netd_dhcp_build_discover(dhcp_client_t *c,
                                uint8_t *out_payload, size_t cap) {
    if (!c || !out_payload || cap < DHCP_PKT_FIXED_LEN + 16) return 0;
    size_t off = dhcp_fill_header(c, out_payload, 0);

    // msg_type = DISCOVER
    uint8_t mt = DHCP_MT_DISCOVER;
    off = dhcp_append_opt(out_payload, off, DHCP_OPT_MSG_TYPE, &mt, 1);

    // Parameter request list — ask for subnet mask, router, DNS.
    uint8_t prl[3] = {DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS_SERVER};
    off = dhcp_append_opt(out_payload, off,
                          DHCP_OPT_PARAM_REQUEST_LIST, prl, 3);

    // End marker.
    out_payload[off++] = DHCP_OPT_END;

    c->state = DHCP_STATE_SELECTING;
    return off;
}

size_t netd_dhcp_build_request(dhcp_client_t *c,
                               uint8_t *out_payload, size_t cap) {
    if (!c || !out_payload || cap < DHCP_PKT_FIXED_LEN + 24) return 0;
    if (c->state != DHCP_STATE_SELECTING) return 0;
    if (c->offered_ip == 0 || c->server_id == 0) return 0;
    size_t off = dhcp_fill_header(c, out_payload, 0);

    uint8_t mt = DHCP_MT_REQUEST;
    off = dhcp_append_opt(out_payload, off, DHCP_OPT_MSG_TYPE, &mt, 1);

    uint8_t req_ip[4];
    netd_write_be32(req_ip, c->offered_ip);
    off = dhcp_append_opt(out_payload, off, DHCP_OPT_REQUESTED_IP, req_ip, 4);

    uint8_t srv_id[4];
    netd_write_be32(srv_id, c->server_id);
    off = dhcp_append_opt(out_payload, off, DHCP_OPT_SERVER_ID, srv_id, 4);

    uint8_t prl[3] = {DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS_SERVER};
    off = dhcp_append_opt(out_payload, off,
                          DHCP_OPT_PARAM_REQUEST_LIST, prl, 3);

    out_payload[off++] = DHCP_OPT_END;

    c->state = DHCP_STATE_REQUESTING;
    return off;
}

// ---------------------------------------------------------------------------
// Parser. Walks the option block; consumes type, lease, server-id, netmask,
// router, dns. Returns the message type (OFFER/ACK/NAK) on success, 0 if
// xid doesn't match or packet is malformed.
// ---------------------------------------------------------------------------
int netd_dhcp_handle_incoming(dhcp_client_t *c,
                              const uint8_t *payload, size_t len) {
    if (!c || !payload || len < DHCP_PKT_FIXED_LEN) return 0;

    // Quick sanity: BOOTREPLY + magic.
    if (payload[0] != DHCP_OP_BOOTREPLY) return 0;
    uint32_t magic = netd_read_be32(&payload[236]);
    if (magic != DHCP_MAGIC) return 0;

    // Xid match.
    uint32_t xid = netd_read_be32(&payload[4]);
    if (xid != c->xid) return 0;

    // yiaddr — server's offered IP.
    uint32_t yiaddr = netd_read_be32(&payload[16]);

    // Walk options.
    size_t i = DHCP_PKT_FIXED_LEN;
    uint8_t msg_type = 0;
    uint32_t server_id = 0;
    uint32_t subnet = 0;
    uint32_t router = 0;
    uint32_t dns = 0;
    uint32_t lease = 0;

    while (i < len) {
        uint8_t code = payload[i++];
        if (code == DHCP_OPT_END) break;
        if (code == DHCP_OPT_PAD) continue;
        if (i >= len) return 0;
        uint8_t olen = payload[i++];
        if (i + olen > len) return 0;
        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (olen >= 1) msg_type = payload[i];
            break;
        case DHCP_OPT_SERVER_ID:
            if (olen == 4) server_id = netd_read_be32(&payload[i]);
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (olen == 4) subnet = netd_read_be32(&payload[i]);
            break;
        case DHCP_OPT_ROUTER:
            if (olen >= 4) router = netd_read_be32(&payload[i]);
            break;
        case DHCP_OPT_DNS_SERVER:
            if (olen >= 4) dns = netd_read_be32(&payload[i]);
            break;
        case DHCP_OPT_LEASE_TIME:
            if (olen == 4) lease = netd_read_be32(&payload[i]);
            break;
        default:
            break;
        }
        i += olen;
    }

    if (msg_type == 0) return 0;

    // State machine gate.
    if (msg_type == DHCP_MT_OFFER) {
        if (c->state != DHCP_STATE_SELECTING) return 0;
        c->offered_ip = yiaddr;
        c->server_id  = server_id;
        c->subnet_mask = subnet;
        c->router = router;
        c->dns = dns;
        return DHCP_MT_OFFER;
    }
    if (msg_type == DHCP_MT_ACK) {
        if (c->state != DHCP_STATE_REQUESTING) return 0;
        c->assigned_ip  = yiaddr;
        c->subnet_mask  = subnet;
        c->router       = router;
        c->dns          = dns;
        c->lease_seconds = lease;
        c->state        = DHCP_STATE_BOUND;
        dhcp_reset_retry(c);
        return DHCP_MT_ACK;
    }
    if (msg_type == DHCP_MT_NAK) {
        // Restart from scratch — back-off bump, state → INIT.
        c->state = DHCP_STATE_INIT;
        c->retry_count++;
        uint32_t next = c->retry_backoff_ms * 2;
        if (next > 30000u) next = 30000u;
        c->retry_backoff_ms = next;
        return DHCP_MT_NAK;
    }
    return 0;
}
