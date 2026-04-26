// user/netd_eth.c — Phase 22 Stage B: Ethernet L2 framing (RFC 894).

#include "netd.h"

// Well-known MAC addresses.
const uint8_t netd_eth_bcast[ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
const uint8_t netd_eth_zero[ETH_ALEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

int netd_mac_eq(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN]) {
    for (int i = 0; i < ETH_ALEN; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

size_t netd_eth_build(uint8_t *frame,
                      const uint8_t dst_mac[ETH_ALEN],
                      const uint8_t src_mac[ETH_ALEN],
                      uint16_t ethertype) {
    // Header layout: [dst_mac:6][src_mac:6][ethertype_be:2]
    for (int i = 0; i < ETH_ALEN; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < ETH_ALEN; i++) frame[6 + i] = src_mac[i];
    netd_write_be16(&frame[12], ethertype);
    return ETH_HDR_LEN;
}

int netd_eth_parse(const uint8_t *frame, size_t buf_len,
                   uint8_t out_dst[ETH_ALEN],
                   uint8_t out_src[ETH_ALEN],
                   uint16_t *out_ethertype) {
    if (buf_len < ETH_HDR_LEN) return -1;
    if (!frame || !out_dst || !out_src || !out_ethertype) return -1;
    for (int i = 0; i < ETH_ALEN; i++) out_dst[i] = frame[i];
    for (int i = 0; i < ETH_ALEN; i++) out_src[i] = frame[6 + i];
    *out_ethertype = netd_read_be16(&frame[12]);
    return 0;
}
