// user/netd_dns.c — Phase 22 Stage C.
//
// Pure wire-level helpers for DNS query construction + response parsing
// (RFC 1035). State (xid tracking, timeout, retry) lives in netd.c; these
// functions are side-effect-free so the TAP test can drive them with
// fixture packets.
//
// Message format (all big-endian on the wire):
//
//     +-------+-------+-------+-------+-------+-------+
//     |       xid     |     flags     |
//     +-------+-------+-------+-------+
//     |    qdcount    |    ancount    |
//     +-------+-------+-------+-------+
//     |    nscount    |    arcount    |
//     +-------+-------+-------+-------+
//     | qname (length-prefixed labels, terminated by 0) |
//     +-------+-------+-------+-------+
//     |    qtype      |    qclass     |
//     +-------+-------+-------+-------+
//     | answer RR 1 / 2 / ...           |
//
// Each answer RR is:
//     name (either labels or 2-byte pointer 0xC0XX) +
//     type (2) + class (2) + ttl (4) + rdlen (2) + rdata (rdlen)
//
// We only parse type=A (1) class=IN (1); other types are skipped cleanly.

#include "netd.h"

// Encode a hostname into DNS label format. Returns bytes written on success
// (always name_len + 2 for a.b.c style: each dot becomes a length prefix
// plus one extra byte for the final NUL terminator) or negative on error.
static int dns_encode_name(uint8_t *out, size_t cap,
                           const char *name, size_t name_len) {
    if (name_len == 0 || name_len > DNS_MAX_NAME_BYTES - 2) return -1;
    size_t w = 0;
    size_t label_start = 0;
    for (size_t i = 0; i <= name_len; i++) {
        if (i == name_len || name[i] == '.') {
            size_t label_len = i - label_start;
            if (label_len == 0 || label_len > 63) return -1;
            if (w + 1 + label_len + 1 > cap) return -1;
            out[w++] = (uint8_t)label_len;
            for (size_t j = 0; j < label_len; j++) {
                out[w++] = (uint8_t)name[label_start + j];
            }
            label_start = i + 1;
        }
    }
    // Terminator.
    if (w >= cap) return -1;
    out[w++] = 0u;
    return (int)w;
}

int netd_dns_build_query(uint8_t *out, size_t cap,
                         uint16_t xid,
                         const char *hostname, size_t hostname_len) {
    if (!out || !hostname) return -1;
    if (cap < 12 + 6) return -1;   // header + at least 1-byte name + qtype/qclass

    // Header: xid | flags=RD | qdcount=1 | ancount=0 | nscount=0 | arcount=0
    netd_write_be16(out + 0, xid);
    netd_write_be16(out + 2, DNS_FLAG_RD);
    netd_write_be16(out + 4, 1);       // qdcount
    netd_write_be16(out + 6, 0);       // ancount
    netd_write_be16(out + 8, 0);       // nscount
    netd_write_be16(out + 10, 0);      // arcount

    int nw = dns_encode_name(out + 12, cap - 12, hostname, hostname_len);
    if (nw < 0) return -1;
    size_t w = 12 + (size_t)nw;

    if (w + 4 > cap) return -1;
    netd_write_be16(out + w, DNS_QTYPE_A);      w += 2;
    netd_write_be16(out + w, DNS_QCLASS_IN);    w += 2;

    return (int)w;
}

// Walk a DNS name starting at `offset`, honouring compression pointers
// (0xC0XX). Returns the offset of the byte just past the name on success,
// or -1 on malformed. Does NOT reconstruct the string.
static int dns_skip_name(const uint8_t *buf, size_t len, size_t offset) {
    // Guard against pointer loops: cap the number of hops.
    int hops = 0;
    int cur = (int)offset;
    int final_offset = -1;
    while (cur >= 0 && (size_t)cur < len) {
        uint8_t b = buf[cur];
        if (b == 0) {
            if (final_offset < 0) final_offset = cur + 1;
            return final_offset;
        }
        if ((b & 0xC0u) == 0xC0u) {
            if ((size_t)cur + 1 >= len) return -1;
            if (final_offset < 0) final_offset = cur + 2;
            uint16_t ptr = ((uint16_t)(b & 0x3Fu) << 8) | buf[cur + 1];
            cur = (int)ptr;
            if (++hops > 16) return -1;
            continue;
        }
        if ((b & 0xC0u) != 0) return -1;   // reserved bits
        size_t label_len = b;
        if ((size_t)cur + 1 + label_len >= len) return -1;
        cur += 1 + (int)label_len;
    }
    return -1;
}

int netd_dns_parse_response(const uint8_t *buf, size_t len,
                            uint16_t expected_xid,
                            dns_query_result_t *result) {
    if (!buf || !result) return -1;
    if (len < 12) return -1;

    uint16_t xid     = netd_read_be16(buf + 0);
    uint16_t flags   = netd_read_be16(buf + 2);
    uint16_t qdcount = netd_read_be16(buf + 4);
    uint16_t ancount = netd_read_be16(buf + 6);
    // We ignore nscount/arcount.

    if (xid != expected_xid) return -1;
    if ((flags & DNS_FLAG_QR) == 0) return -1;

    uint16_t rcode = flags & DNS_FLAG_RCODE_MASK;
    result->xid         = xid;
    result->rcode       = rcode;
    result->ip_count    = 0;
    result->ttl_seconds = 0;
    result->_pad        = 0;

    // Skip the question section: qdcount × (name + 4 bytes qtype/qclass).
    size_t off = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        int after = dns_skip_name(buf, len, off);
        if (after < 0) return -1;
        off = (size_t)after + 4;
        if (off > len) return -1;
    }

    // NXDOMAIN / SERVFAIL / no-answers → return 0 with ip_count=0; rcode
    // already captured above.
    if (rcode != DNS_RCODE_NOERROR || ancount == 0) {
        return 0;
    }

    // Walk the answer section.
    for (uint16_t i = 0; i < ancount; i++) {
        int after = dns_skip_name(buf, len, off);
        if (after < 0) return -1;
        off = (size_t)after;
        if (off + 10 > len) return -1;
        uint16_t rr_type  = netd_read_be16(buf + off); off += 2;
        uint16_t rr_class = netd_read_be16(buf + off); off += 2;
        uint32_t rr_ttl   = ((uint32_t)buf[off]     << 24) |
                            ((uint32_t)buf[off + 1] << 16) |
                            ((uint32_t)buf[off + 2] << 8)  |
                            ((uint32_t)buf[off + 3]);
        off += 4;
        uint16_t rdlen    = netd_read_be16(buf + off); off += 2;
        if (off + rdlen > len) return -1;

        if (rr_type == DNS_QTYPE_A && rr_class == DNS_QCLASS_IN &&
            rdlen == 4 && result->ip_count < DNS_MAX_ANSWERS) {
            uint32_t a = ((uint32_t)buf[off]     << 24) |
                         ((uint32_t)buf[off + 1] << 16) |
                         ((uint32_t)buf[off + 2] << 8)  |
                         ((uint32_t)buf[off + 3]);
            result->ips[result->ip_count++] = a;
            if (result->ttl_seconds == 0) result->ttl_seconds = rr_ttl;
        }
        off += rdlen;
    }
    return 0;
}
