// user/netd_arp.c — Phase 22 Stage B: ARP (RFC 826) implementation.
//
// A tiny 16-entry table backed by per-slot state (FREE/RESOLVED/PENDING).
// Lookup is O(n) linear scan — with n=16 this is ~80 ns in the worst case
// on the target hardware, vastly lower than the frame rate budget. No
// hashing because the cost would exceed the scan. LRU eviction via
// round-robin cursor when the table is full.
//
// PENDING entries dedupe concurrent ARP requests: netd_arp_resolve returns
// without emitting a new request if a PENDING entry exists and hasn't yet
// expired. The caller (netd main loop) is responsible for re-calling
// netd_arp_resolve periodically — on reply, the entry flips to RESOLVED;
// on timeout, to FREE and a fresh request cycle may begin.

#include "netd.h"

void netd_arp_init(arp_table_t *tbl) {
    if (!tbl) return;
    netd_memzero(tbl, sizeof(*tbl));
    // Zeroed state == ARP_STATE_FREE by construction.
    tbl->next_slot = 0;
}

int netd_arp_lookup(const arp_table_t *tbl, uint32_t ip,
                    uint8_t out_mac[ETH_ALEN]) {
    if (!tbl) return 0;
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        const arp_entry_t *e = &tbl->entries[i];
        if (e->state == ARP_STATE_RESOLVED && e->ip == ip) {
            if (out_mac) {
                for (int k = 0; k < ETH_ALEN; k++) out_mac[k] = e->mac[k];
            }
            return 1;
        }
    }
    return 0;
}

// Find the best slot to claim for `ip`:
//   (1) An existing RESOLVED/PENDING entry for the same IP (refresh).
//   (2) The first FREE slot.
//   (3) Round-robin via next_slot if table is full.
static uint32_t netd_arp_choose_slot(arp_table_t *tbl, uint32_t ip) {
    // Pass 1: matching IP wins (even if PENDING — a reply always updates).
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        const arp_entry_t *e = &tbl->entries[i];
        if (e->state != ARP_STATE_FREE && e->ip == ip) return i;
    }
    // Pass 2: first FREE slot.
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        if (tbl->entries[i].state == ARP_STATE_FREE) return i;
    }
    // Pass 3: round-robin eviction.
    uint32_t slot = tbl->next_slot % ARP_TABLE_SLOTS;
    tbl->next_slot = (slot + 1) % ARP_TABLE_SLOTS;
    return slot;
}

void netd_arp_insert(arp_table_t *tbl, uint32_t ip,
                     const uint8_t mac[ETH_ALEN],
                     uint8_t state, uint64_t now_tsc, uint64_t ttl_tsc) {
    if (!tbl || !mac) return;
    if (state != ARP_STATE_RESOLVED && state != ARP_STATE_PENDING) return;

    uint32_t slot = netd_arp_choose_slot(tbl, ip);
    arp_entry_t *e = &tbl->entries[slot];
    e->ip    = ip;
    e->state = state;
    e->_pad  = 0;
    for (int k = 0; k < ETH_ALEN; k++) e->mac[k] = mac[k];
    e->expiry_tsc = now_tsc + ttl_tsc;
}

uint32_t netd_arp_gc(arp_table_t *tbl, uint64_t now_tsc) {
    if (!tbl) return 0;
    uint32_t aged = 0;
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        arp_entry_t *e = &tbl->entries[i];
        if (e->state == ARP_STATE_RESOLVED && now_tsc >= e->expiry_tsc) {
            netd_memzero(e, sizeof(*e));
            aged++;
        }
    }
    return aged;
}

uint32_t netd_arp_count(const arp_table_t *tbl) {
    if (!tbl) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        if (tbl->entries[i].state == ARP_STATE_RESOLVED) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Frame builders.
// ---------------------------------------------------------------------------

// Fill bytes 14..41 (the ARP PDU) of `frame` with op/sender/target. The
// Ethernet header (bytes 0..13) is filled separately via netd_eth_build.
static void netd_arp_fill_pdu(uint8_t *frame,
                              uint16_t op,
                              const uint8_t sender_mac[ETH_ALEN],
                              uint32_t sender_ip,
                              const uint8_t target_mac[ETH_ALEN],
                              uint32_t target_ip) {
    uint8_t *a = frame + ETH_HDR_LEN;
    netd_write_be16(a + 0, ARP_HTYPE_ETH);
    netd_write_be16(a + 2, ARP_PTYPE_IPV4);
    a[4] = ARP_HLEN_ETH;
    a[5] = ARP_PLEN_IPV4;
    netd_write_be16(a + 6, op);
    for (int k = 0; k < ETH_ALEN; k++) a[8 + k]  = sender_mac[k];
    netd_write_be32(a + 14, sender_ip);
    for (int k = 0; k < ETH_ALEN; k++) a[18 + k] = target_mac[k];
    netd_write_be32(a + 24, target_ip);
}

size_t netd_arp_build_request(uint8_t *out_frame,
                              const uint8_t my_mac[ETH_ALEN],
                              uint32_t my_ip,
                              uint32_t target_ip) {
    // Broadcast Ethernet destination; target MAC in ARP PDU is all-zero
    // per RFC 826 ("requester does not yet know the hardware address").
    netd_eth_build(out_frame, netd_eth_bcast, my_mac, ETH_TYPE_ARP);
    netd_arp_fill_pdu(out_frame, ARP_OP_REQUEST, my_mac, my_ip,
                      netd_eth_zero, target_ip);
    return ARP_FRAME_LEN;
}

size_t netd_arp_build_reply(uint8_t *out_frame,
                            const uint8_t my_mac[ETH_ALEN],
                            uint32_t my_ip,
                            const uint8_t requester_mac[ETH_ALEN],
                            uint32_t requester_ip) {
    // Unicast reply back to the requester.
    netd_eth_build(out_frame, requester_mac, my_mac, ETH_TYPE_ARP);
    netd_arp_fill_pdu(out_frame, ARP_OP_REPLY, my_mac, my_ip,
                      requester_mac, requester_ip);
    return ARP_FRAME_LEN;
}

// ---------------------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------------------
int netd_arp_parse(const uint8_t *arp_buf, size_t buf_len,
                   arp_packet_t *out) {
    if (!arp_buf || !out) return -1;
    if (buf_len < ARP_PAYLOAD_LEN) return -1;

    out->htype      = netd_read_be16(arp_buf + 0);
    out->ptype      = netd_read_be16(arp_buf + 2);
    out->hlen       = arp_buf[4];
    out->plen       = arp_buf[5];
    out->op         = netd_read_be16(arp_buf + 6);
    for (int k = 0; k < ETH_ALEN; k++) out->sender_mac[k] = arp_buf[8 + k];
    out->sender_ip  = netd_read_be32(arp_buf + 14);
    for (int k = 0; k < ETH_ALEN; k++) out->target_mac[k] = arp_buf[18 + k];
    out->target_ip  = netd_read_be32(arp_buf + 24);

    // Validate content. Reject anything that isn't Ethernet/IPv4 ARP.
    if (out->htype != ARP_HTYPE_ETH) return -2;
    if (out->ptype != ARP_PTYPE_IPV4) return -2;
    if (out->hlen != ARP_HLEN_ETH) return -2;
    if (out->plen != ARP_PLEN_IPV4) return -2;
    if (out->op != ARP_OP_REQUEST && out->op != ARP_OP_REPLY) return -2;
    return 0;
}

// ---------------------------------------------------------------------------
// Glue: incoming handler.
// ---------------------------------------------------------------------------
int netd_arp_handle_incoming(arp_table_t *tbl,
                             const uint8_t *arp_buf, size_t arp_len,
                             const uint8_t my_mac[ETH_ALEN],
                             uint32_t my_ip,
                             uint64_t now_tsc, uint64_t ttl_tsc,
                             uint8_t *reply_buf,
                             size_t reply_buf_cap,
                             size_t *reply_len) {
    if (reply_len) *reply_len = 0;

    arp_packet_t pkt;
    int rc = netd_arp_parse(arp_buf, arp_len, &pkt);
    if (rc < 0) return rc;

    // RFC 826 §3: Update sender cache entry unconditionally, even for
    // REQUESTs targeting someone else — the sender-IP mapping is useful.
    // This also catches gratuitous ARPs.
    netd_arp_insert(tbl, pkt.sender_ip, pkt.sender_mac,
                    ARP_STATE_RESOLVED, now_tsc, ttl_tsc);

    // If it's a REQUEST for our IP (my_ip != 0 to avoid replying before
    // DHCP completes), craft a REPLY.
    if (pkt.op == ARP_OP_REQUEST && my_ip != 0 && pkt.target_ip == my_ip &&
        reply_buf && reply_buf_cap >= ARP_FRAME_LEN && reply_len) {
        *reply_len = netd_arp_build_reply(reply_buf, my_mac, my_ip,
                                          pkt.sender_mac, pkt.sender_ip);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Outbound resolution.
// ---------------------------------------------------------------------------
int netd_arp_resolve(arp_table_t *tbl,
                     const uint8_t my_mac[ETH_ALEN],
                     uint32_t my_ip,
                     uint32_t target_ip,
                     uint64_t now_tsc, uint64_t pending_ttl_tsc,
                     uint8_t out_mac[ETH_ALEN],
                     uint8_t *req_buf,
                     size_t req_buf_cap,
                     size_t *req_len) {
    if (req_len) *req_len = 0;

    // Fast path: cached RESOLVED entry.
    if (netd_arp_lookup(tbl, target_ip, out_mac)) return 1;

    // Dedup: if a PENDING entry exists and hasn't expired, don't emit a
    // duplicate request — the caller should just wait a bit longer.
    for (uint32_t i = 0; i < ARP_TABLE_SLOTS; i++) {
        const arp_entry_t *e = &tbl->entries[i];
        if (e->state == ARP_STATE_PENDING && e->ip == target_ip &&
            now_tsc < e->expiry_tsc) {
            return 0;  // Pending — caller keeps waiting.
        }
    }

    // Insert PENDING, emit request frame.
    netd_arp_insert(tbl, target_ip, netd_eth_zero, ARP_STATE_PENDING,
                    now_tsc, pending_ttl_tsc);

    if (req_buf && req_buf_cap >= ARP_FRAME_LEN && req_len) {
        *req_len = netd_arp_build_request(req_buf, my_mac, my_ip, target_ip);
    }
    return 0;
}
