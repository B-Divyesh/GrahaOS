// user/netd_icmp.c — Phase 22 Stage B: ICMP (RFC 792) echo + reply.
//
// Scope is intentionally small — netd only needs:
//   - ECHO REQUEST handler that emits an ECHO REPLY
//   - ECHO REQUEST builder for /bin/ping (Stage D)
//   - Destination-unreachable emission is deferred until the TX-error path
//     lands (a full L4 error surface needs TCP/UDP in place first).
//
// Checksum semantics: covers the ICMP header + data (no pseudo-header —
// that's TCP/UDP). Caller is responsible for dropping ICMP packets where
// the IPv4 src is 0.0.0.0 (protected by netd main loop).

#include "netd.h"

size_t netd_icmp_build_echo(uint8_t *out,
                            uint8_t type, uint8_t code,
                            uint16_t id, uint16_t seq,
                            const uint8_t *payload, size_t payload_len) {
    out[0] = type;
    out[1] = code;
    netd_write_be16(&out[2], 0);        // Zero checksum before compute.
    netd_write_be16(&out[4], id);
    netd_write_be16(&out[6], seq);
    for (size_t i = 0; i < payload_len; i++) out[8 + i] = payload[i];

    uint16_t csum_be = netd_inet_checksum(out, 8 + payload_len, 0);
    out[2] = (uint8_t)(netd_ntohs(csum_be) >> 8);
    out[3] = (uint8_t)(netd_ntohs(csum_be) & 0xFFu);
    return 8 + payload_len;
}

int netd_icmp_parse_echo(const uint8_t *buf, size_t buf_len,
                         uint8_t *out_type, uint8_t *out_code,
                         uint16_t *out_id, uint16_t *out_seq,
                         const uint8_t **out_payload,
                         size_t *out_payload_len) {
    if (!buf || buf_len < 8) return -1;
    if (!out_type || !out_code || !out_id || !out_seq ||
        !out_payload || !out_payload_len) return -1;

    // Validate checksum: sum across the whole ICMP PDU must fold to 0xFFFF.
    uint16_t csum_be = netd_inet_checksum(buf, buf_len, 0);
    if (csum_be != 0) return -2;

    *out_type        = buf[0];
    *out_code        = buf[1];
    *out_id          = netd_read_be16(&buf[4]);
    *out_seq         = netd_read_be16(&buf[6]);
    *out_payload     = buf + 8;
    *out_payload_len = buf_len - 8;
    return 0;
}

// Given an already-parsed incoming IPv4 datagram whose proto is ICMP,
// decide whether to reply. On ECHO REQUEST addressed to us, compose the
// complete reply datagram (IPv4 + ICMP) into `reply_buf`.
int netd_icmp_handle_incoming(const ipv4_parsed_t *ip,
                              uint32_t my_ip,
                              uint8_t *reply_buf,
                              size_t reply_buf_cap,
                              size_t *reply_len) {
    if (!ip || !reply_buf || !reply_len) return -1;
    *reply_len = 0;

    if (ip->proto != IPPROTO_ICMP) return 0;
    if (my_ip != 0 && ip->dst != my_ip) return 0;

    if (ip->payload_len < 8) return -1;
    uint8_t  type, code;
    uint16_t id, seq;
    const uint8_t *echo_payload = (const uint8_t *)0;
    size_t   echo_payload_len = 0;
    int rc = netd_icmp_parse_echo(ip->payload, ip->payload_len,
                                  &type, &code, &id, &seq,
                                  &echo_payload, &echo_payload_len);
    if (rc < 0) return rc;

    if (type != ICMP_TYPE_ECHO_REQUEST || code != 0) return 0;

    // Reply size: 20 (IPv4 header) + 8 (ICMP header) + payload.
    size_t icmp_len = 8 + echo_payload_len;
    size_t total    = IPV4_HDR_LEN_MIN + icmp_len;
    if (reply_buf_cap < total) return -3;

    // Build the IPv4 header first so the ICMP build can follow immediately.
    // Destination = requester (ip->src); Source = us (my_ip) or the
    // address they asked about (if my_ip == 0, accept responsibility for
    // replying as ip->dst — useful in diagnostic configurations).
    uint32_t reply_src = (my_ip != 0) ? my_ip : ip->dst;
    uint32_t reply_dst = ip->src;
    // Simple monotonic IP.id: use the incoming id + 1. RFC is lax here.
    uint16_t reply_id  = (uint16_t)(ip->id + 1);
    netd_ipv4_build_header(reply_buf, reply_src, reply_dst,
                           IPPROTO_ICMP, reply_id,
                           (uint16_t)icmp_len, IPV4_DEFAULT_TTL);

    // ICMP: ECHO REPLY carries the same id/seq/payload.
    netd_icmp_build_echo(reply_buf + IPV4_HDR_LEN_MIN,
                         ICMP_TYPE_ECHO_REPLY, 0,
                         id, seq, echo_payload, echo_payload_len);

    *reply_len = total;
    return 0;
}
