// user/netd_ipv4.c — Phase 22 Stage B: IPv4 (RFC 791, no TX fragmentation).
//
// Scope:
//   - Header build (v4, IHL=5, no options, TTL=64 default, identification
//     monotonic caller-provided).
//   - Header parse: validates version/IHL/length/checksum; rejects fragments.
//   - Internet checksum (RFC 1071) used by IP/ICMP/TCP/UDP.
//   - Pseudo-header builder used by TCP/UDP checksum computation.
//   - Dotted-quad string parse (for /etc/net/static.conf + manual IP entry).
//
// Not-yet:
//   - TX fragmentation — senders (TCP MSS, UDP max 1472) ensure frames fit
//     in the 1500-byte MTU.
//   - IP options on RX — rejected as malformed.
//   - Multicast / broadcast routing — netd accepts broadcast only for DHCP.

#include "netd.h"

// RFC 1071 one's-complement Internet checksum.
//
// The standard computation:
//   sum = 0
//   for each 16-bit word in buf: sum += word
//   while sum >> 16: sum = (sum & 0xffff) + (sum >> 16)
//   return ~sum
//
// Caller passes `initial` as a 32-bit accumulator to chain pseudo-headers
// (TCP/UDP: call first on the pseudo-header with initial=0, get back the
// folded value, pass that as `initial` on the segment body).
uint16_t netd_inet_checksum(const uint8_t *buf, size_t len, uint32_t initial) {
    uint32_t sum = initial;
    // Even pairs. If the initial is a folded 16-bit value, feeding it as
    // a uint32_t accumulator is safe — we fold at the end.
    while (len >= 2) {
        sum += ((uint32_t)buf[0] << 8) | (uint32_t)buf[1];
        buf += 2;
        len -= 2;
    }
    // Trailing odd byte: high-order.
    if (len == 1) {
        sum += ((uint32_t)buf[0] << 8);
    }
    // Fold carries.
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    uint16_t csum = (uint16_t)(~sum & 0xFFFFu);
    // Return in BIG-ENDIAN (ready to write directly to the header byte).
    return netd_htons(csum);
}

// Build a 20-byte IPv4 header (v4, IHL=5) at out_hdr[0..19].
size_t netd_ipv4_build_header(uint8_t *out_hdr,
                              uint32_t src, uint32_t dst,
                              uint8_t proto,
                              uint16_t id,
                              uint16_t payload_len,
                              uint8_t ttl) {
    uint16_t total_len = (uint16_t)IPV4_HDR_LEN_MIN + payload_len;
    out_hdr[0] = (uint8_t)((IPV4_VERSION << 4) | 5);   // version(4) | IHL(5)
    out_hdr[1] = 0;                                    // TOS / DSCP
    netd_write_be16(&out_hdr[2], total_len);
    netd_write_be16(&out_hdr[4], id);
    netd_write_be16(&out_hdr[6], 0);                   // Flags + frag_offset
    out_hdr[8]  = ttl;
    out_hdr[9]  = proto;
    netd_write_be16(&out_hdr[10], 0);                  // Zero checksum first
    netd_write_be32(&out_hdr[12], src);
    netd_write_be32(&out_hdr[16], dst);
    // Compute + patch checksum.
    uint16_t csum_be = netd_inet_checksum(out_hdr, IPV4_HDR_LEN_MIN, 0);
    out_hdr[10] = (uint8_t)(netd_ntohs(csum_be) >> 8);
    out_hdr[11] = (uint8_t)(netd_ntohs(csum_be) & 0xFFu);
    return IPV4_HDR_LEN_MIN;
}

int netd_ipv4_parse(const uint8_t *buf, size_t buf_len, ipv4_parsed_t *out) {
    if (!buf || !out) return -1;
    if (buf_len < IPV4_HDR_LEN_MIN) return -1;

    uint8_t vhl = buf[0];
    uint8_t version = (uint8_t)(vhl >> 4);
    uint8_t ihl     = (uint8_t)(vhl & 0x0Fu);
    if (version != IPV4_VERSION) return -2;
    if (ihl != 5) return -2;                 // Reject options (MVP scope).

    uint16_t total_len   = netd_read_be16(&buf[2]);
    uint16_t id          = netd_read_be16(&buf[4]);
    uint16_t flags_frag  = netd_read_be16(&buf[6]);
    uint8_t  ttl         = buf[8];
    uint8_t  proto       = buf[9];
    uint32_t src         = netd_read_be32(&buf[12]);
    uint32_t dst         = netd_read_be32(&buf[16]);

    if (total_len < IPV4_HDR_LEN_MIN) return -3;
    if (total_len > buf_len) return -3;

    // RFC 791 fragmentation check. MF bit is bit 13 (0x2000). The low 13
    // bits are the fragment offset. Anything nonzero means a fragment.
    if ((flags_frag & 0x2000u) || (flags_frag & 0x1FFFu)) {
        return -4;  // MVP drops all fragments.
    }

    // Verify header checksum. RFC 1071: sum must fold to 0xFFFF across the
    // header when the checksum field is included in the sum as-stored.
    uint16_t csum_be = netd_inet_checksum(buf, IPV4_HDR_LEN_MIN, 0);
    if (csum_be != 0) return -5;

    out->src         = src;
    out->dst         = dst;
    out->proto       = proto;
    out->ttl         = ttl;
    out->total_len   = total_len;
    out->id          = id;
    out->flags_frag  = flags_frag;
    out->payload     = buf + IPV4_HDR_LEN_MIN;
    out->payload_len = (size_t)total_len - IPV4_HDR_LEN_MIN;
    return 0;
}

size_t netd_ipv4_build_pseudo_header(uint8_t *out,
                                     uint32_t src, uint32_t dst,
                                     uint8_t proto,
                                     uint16_t l4_len) {
    netd_write_be32(&out[0], src);
    netd_write_be32(&out[4], dst);
    out[8] = 0;
    out[9] = proto;
    netd_write_be16(&out[10], l4_len);
    return 12;
}

// Parse "A.B.C.D" into a 32-bit host-byte-order uint. Strict decimal-only;
// rejects any whitespace, hex, partial form, or out-of-range octet.
int netd_ipv4_parse_dotted(const char *str, uint32_t *out) {
    if (!str || !out) return -1;
    uint32_t ip = 0;
    int octet = 0;
    int has_digit = 0;
    int octet_idx = 0;
    const char *p = str;

    while (1) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            octet = octet * 10 + (c - '0');
            if (octet > 255) return -1;
            has_digit = 1;
            p++;
        } else if (c == '.' || c == '\0') {
            if (!has_digit) return -1;
            if (octet_idx >= 4) return -1;
            ip = (ip << 8) | (uint32_t)(octet & 0xFF);
            octet_idx++;
            octet = 0;
            has_digit = 0;
            if (c == '\0') break;
            p++;
        } else {
            return -1;
        }
    }
    if (octet_idx != 4) return -1;
    *out = ip;
    return 0;
}
