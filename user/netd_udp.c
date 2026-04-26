// user/netd_udp.c — Phase 22 Stage B: UDP (RFC 768) + socket table.
//
// UDP checksum: covers the pseudo-header + UDP header + payload. RFC 768
// permits checksum=0 as "disabled" (IPv4 only); we accept incoming 0 but
// always compute on send so receivers can rely on it.
//
// Socket table: 16 fixed slots, bind-by-local-port. Ephemeral allocation
// scans the 48152-65535 range starting from a running cursor so repeated
// bindings don't repeatedly probe the same ports.

#include "netd.h"

// =========================================================================
// Wire helpers.
// =========================================================================
size_t netd_udp_build(uint8_t *out,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, size_t payload_len) {
    size_t total = UDP_HDR_LEN + payload_len;
    netd_write_be16(&out[0], src_port);
    netd_write_be16(&out[2], dst_port);
    netd_write_be16(&out[4], (uint16_t)total);
    netd_write_be16(&out[6], 0);                    // checksum placeholder
    for (size_t i = 0; i < payload_len; i++) out[UDP_HDR_LEN + i] = payload[i];

    // Pseudo-header then segment body.
    uint8_t pseudo[12];
    netd_ipv4_build_pseudo_header(pseudo, src_ip, dst_ip, IPPROTO_UDP,
                                  (uint16_t)total);
    uint32_t sum = 0;
    uint16_t p_sum = netd_inet_checksum(pseudo, 12, 0);
    sum += netd_ntohs(p_sum);
    uint16_t s_sum = netd_inet_checksum(out, total, 0);
    sum += netd_ntohs(s_sum);
    // The two ~sums were already inverted; combining requires re-inversion.
    // Simpler path: recompute over concat (pseudo || segment) directly.
    (void)sum;
    uint8_t scratch[12 + UDP_HDR_LEN + 1500];
    if (total > 1500) return 0;  // defensive
    for (size_t i = 0; i < 12; i++) scratch[i] = pseudo[i];
    for (size_t i = 0; i < total; i++) scratch[12 + i] = out[i];
    uint16_t csum_be = netd_inet_checksum(scratch, 12 + total, 0);
    // RFC 768: a transmitted checksum of 0 means "no checksum"; if the
    // computed value is zero, send 0xFFFF instead so the receiver doesn't
    // skip the verification.
    if (csum_be == 0) csum_be = 0xFFFFu;
    out[6] = (uint8_t)(netd_ntohs(csum_be) >> 8);
    out[7] = (uint8_t)(netd_ntohs(csum_be) & 0xFFu);
    return total;
}

int netd_udp_parse(const uint8_t *buf, size_t buf_len,
                   uint32_t src_ip, uint32_t dst_ip,
                   uint16_t *out_src_port, uint16_t *out_dst_port,
                   const uint8_t **out_payload, size_t *out_payload_len) {
    if (!buf || !out_src_port || !out_dst_port || !out_payload ||
        !out_payload_len) return -1;
    if (buf_len < UDP_HDR_LEN) return -1;

    uint16_t src_port = netd_read_be16(&buf[0]);
    uint16_t dst_port = netd_read_be16(&buf[2]);
    uint16_t length   = netd_read_be16(&buf[4]);
    uint16_t checksum = netd_read_be16(&buf[6]);

    if (length < UDP_HDR_LEN) return -2;
    if (length > buf_len) return -2;

    // Checksum verification (checksum==0 means disabled).
    if (checksum != 0) {
        // Scratch: pseudo-header + UDP segment. Max UDP = 1472 (IP-mtu-20).
        if (length > 1500) return -3;
        uint8_t scratch[12 + 1500];
        netd_ipv4_build_pseudo_header(scratch, src_ip, dst_ip,
                                      IPPROTO_UDP, length);
        for (size_t i = 0; i < length; i++) scratch[12 + i] = buf[i];
        uint16_t csum = netd_inet_checksum(scratch, 12 + length, 0);
        if (csum != 0) return -4;
    }

    *out_src_port     = src_port;
    *out_dst_port     = dst_port;
    *out_payload      = buf + UDP_HDR_LEN;
    *out_payload_len  = length - UDP_HDR_LEN;
    return 0;
}

// =========================================================================
// Socket table.
// =========================================================================
void netd_udp_table_init(udp_table_t *tbl) {
    if (!tbl) return;
    netd_memzero(tbl, sizeof(*tbl));
}

// Port-in-use check. Returns 1 if another BOUND socket has `port`.
static int udp_port_in_use(const udp_table_t *tbl, uint16_t port) {
    for (uint32_t i = 0; i < UDP_MAX_SOCKETS; i++) {
        const udp_socket_t *s = &tbl->sockets[i];
        if (s->state == UDP_STATE_BOUND && s->local_port == port) return 1;
    }
    return 0;
}

// Ephemeral cursor. Starts at 48152 (Linux default lower bound) and wraps
// through 65535. Static scope — the same cursor is shared across all
// tables in the process, acceptable for MVP where netd has one table.
static uint16_t s_udp_ephemeral_cursor = 48152u;

int netd_udp_bind(udp_table_t *tbl, uint16_t port, uint32_t owner_cookie) {
    if (!tbl) return -1;
    if (port == 0) {
        // Ephemeral.
        for (uint32_t attempts = 0; attempts < 17384; attempts++) {
            uint16_t candidate = s_udp_ephemeral_cursor++;
            if (s_udp_ephemeral_cursor == 0) s_udp_ephemeral_cursor = 48152u;
            if (candidate < 48152u) { s_udp_ephemeral_cursor = 48153u; continue; }
            if (!udp_port_in_use(tbl, candidate)) {
                port = candidate;
                break;
            }
        }
        if (port == 0) return -2;   // -EAGAIN analogue
    } else if (udp_port_in_use(tbl, port)) {
        return -3;   // -EADDRINUSE analogue
    }

    for (uint32_t i = 0; i < UDP_MAX_SOCKETS; i++) {
        udp_socket_t *s = &tbl->sockets[i];
        if (s->state == UDP_STATE_FREE) {
            s->state        = UDP_STATE_BOUND;
            s->local_port   = port;
            s->owner_cookie = owner_cookie;
            return (int)i;
        }
    }
    return -4;  // -ENOMEM analogue (table full)
}

void netd_udp_close(udp_table_t *tbl, int idx) {
    if (!tbl || idx < 0 || (uint32_t)idx >= UDP_MAX_SOCKETS) return;
    netd_memzero(&tbl->sockets[idx], sizeof(tbl->sockets[idx]));
}

int netd_udp_find(const udp_table_t *tbl, uint16_t local_port) {
    if (!tbl) return -1;
    for (uint32_t i = 0; i < UDP_MAX_SOCKETS; i++) {
        const udp_socket_t *s = &tbl->sockets[i];
        if (s->state == UDP_STATE_BOUND && s->local_port == local_port) {
            return (int)i;
        }
    }
    return -1;
}
