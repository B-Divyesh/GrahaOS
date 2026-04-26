// user/netd.c — Phase 22 Stage C daemon.
//
// Clean-room userspace TCP/IP stack. Stage A established the rendezvous with
// e1000d over /sys/net/rawframe + published /sys/net/service. Stage B added
// the protocol-module library (ARP / IPv4 / ICMP / UDP / TCP-Reno / DHCP)
// plus the VMO-shared substrate and the rawframe ANNOUNCE handshake. Stage C
// wires the live stack:
//
//   1. RX path: poll rawframe rd_resp for RX_NOTIFY; on each notify, copy
//      the frame out of the shared rx ring and run it through
//      netd_eth_parse → ARP / IPv4 → ICMP / UDP (DHCP, DNS) / TCP (Stage D).
//
//   2. TX path: allocate the next tx-ring slot, fill with Ethernet+IPv4+L4,
//      send rawframe_slot_msg_t{op=TX_REQ, slot, len} on wr_req. e1000d
//      reads from the shared tx ring and DMAs out.
//
//   3. DHCP: on link-up (from rawframe ANNOUNCE.link_up), send DISCOVER;
//      on OFFER, send REQUEST; on ACK, populate state.ip/gw/dns/netmask +
//      flip stack_running=1.
//
//   4. Per-client service: libnet_service_accept fills a slot table. Each
//      slot's rd_req is polled non-blocking per tick. Ops dispatch via a
//      switch; replies go out on the slot's wr_resp with the same seq.
//
//   5. DNS: pending-query table (16 slots). net_dns_resolve arrives on the
//      service channel; netd builds a DNS query, puts it on the wire
//      (UDP to learned resolver), records (xid, client_idx, seq, deadline).
//      On matching UDP reply, parse and respond to the client.
//
// Design principle: every protocol helper is pure (in libnetd.a). Stateful
// logic lives in this file.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "syscalls.h"
#include "libnet/libnet.h"
#include "libnet/libnet_msg.h"
#include "libnet/rawframe.h"
#include "netd.h"

extern int printf(const char *fmt, ...);

// =====================================================================
// rdtsc — used for DHCP xid, pending-DNS deadlines, TCP seq seeding,
// and general "now" timestamps. QEMU invariant TSC at ~2.4 GHz — we
// bake TICKS_PER_SEC as the default but callers that need wall-clock
// should not rely on precision better than ±20 %.
// =====================================================================
#define NETD_TICKS_PER_SEC   2400000000ull

static uint64_t netd_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Basic xorshift PRNG for initial sequence numbers, DNS transaction ids,
// etc. Seeded from rdtsc on first use.
static uint64_t s_rng_state = 0;
static uint32_t netd_rand32(void) {
    if (s_rng_state == 0) s_rng_state = netd_rdtsc();
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 7;
    s_rng_state ^= s_rng_state << 17;
    return (uint32_t)s_rng_state;
}

// =====================================================================
// Daemon-wide state.
// =====================================================================
typedef struct netd_state {
    // Link-level facts learned from e1000d's rawframe ANNOUNCE.
    uint8_t  mac[6];
    uint8_t  _pad0[2];
    uint8_t  link_up;
    uint8_t  stack_running;     // 1 once DHCP bound.
    uint8_t  _pad1[6];

    uint64_t rx_ring_va;        // VA of mapped rx ring
    uint64_t tx_ring_va;
    uint32_t slot_count;
    uint32_t slot_size;
    uint32_t tx_cursor;         // Next tx slot to use (round-robin)
    uint32_t _pad2;

    // Rawframe peer — only one peer today (the single NIC), so we hold a
    // single pair of handles.
    cap_token_u_t rawframe_wr_req;
    cap_token_u_t rawframe_rd_resp;

    // L3/L4 configuration. All host byte order.
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_resolver;

    // Counters exposed via net_query.
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;

    // Protocol module state.
    arp_table_t    arp;
    udp_table_t    udp;
    tcp_table_t    tcp;
    dhcp_client_t  dhcp;

    // Daemon PID (filled from syscall_getpid for net_query).
    int32_t  pid;
    uint32_t _pad3;
} netd_state_t;

static netd_state_t g_net;

// =====================================================================
// Client slot table. /sys/net/service accepts up to NETD_MAX_CLIENTS
// concurrent connections; each slot holds the per-connection channels +
// a tiny state machine for out-of-band readers.
// =====================================================================
#define NETD_MAX_CLIENTS  16u

typedef struct netd_client {
    uint8_t        in_use;
    uint8_t        _pad[3];
    int32_t        connector_pid;
    uint32_t       connection_id;
    cap_token_u_t  rd_req;
    cap_token_u_t  wr_resp;
} netd_client_t;

static netd_client_t g_clients[NETD_MAX_CLIENTS];

// =====================================================================
// DNS pending-query table. MVP: up to 16 in-flight queries. On each UDP
// reply received on the assigned src_port, we scan the table for a match.
// =====================================================================
#define NETD_MAX_PENDING_DNS  16u

typedef struct pending_dns {
    uint8_t   in_use;
    uint8_t   _pad;
    uint16_t  xid;              // DNS transaction id
    uint16_t  src_port;         // UDP ephemeral src port we used
    uint16_t  _pad2;
    uint32_t  client_idx;       // Slot index into g_clients
    uint32_t  req_seq;          // Echo on resp
    uint64_t  deadline_tsc;
} pending_dns_t;

static pending_dns_t g_dns[NETD_MAX_PENDING_DNS];

// =====================================================================
// ICMP pending-echo table (Stage C). ping sends icmp_echo_req → we emit
// ICMP ECHO REQUEST on the wire and record (id, seq, client_idx, deadline);
// on matching ICMP ECHO REPLY we compute rtt and reply to the client.
// =====================================================================
#define NETD_MAX_PENDING_ICMP  16u

typedef struct pending_icmp {
    uint8_t   in_use;
    uint8_t   payload_len;
    uint16_t  id;
    uint16_t  seq;
    uint16_t  _pad;
    uint32_t  client_idx;
    uint32_t  req_seq;
    uint32_t  dst_ip;
    uint64_t  start_tsc;
    uint64_t  deadline_tsc;
    uint8_t   payload[64];
} pending_icmp_t;

static pending_icmp_t g_icmps[NETD_MAX_PENDING_ICMP];

// =====================================================================
// TCP daemon state.
//
// The `netd_tcp.c` module owns the wire state machine + per-socket
// sequence tracking; netd.c glues it to the service channel and gives
// each socket an rx ring so libhttp / aitest / grahai can pull bytes
// via libnet_tcp_recv. A single client owns one TCP socket at a time
// — the `client_idx` field on the conn tracks ownership.
//
// NETD_TCP_RX_RING_BYTES trades memory for smoother HTTP response
// streaming. 2 KiB per socket × 16 sockets = 32 KiB BSS; adequate for
// MVP HTTP responses (small JSON bodies, chunked transfers drained
// by libhttp within a few ticks).
// =====================================================================
#define NETD_TCP_RX_RING_BYTES   2048u
#define NETD_TCP_EPHEMERAL_MIN   32768u
#define NETD_TCP_EPHEMERAL_SPAN  32768u

typedef struct netd_tcp_conn {
    uint8_t  in_use;
    uint8_t  peer_fin;         // Peer has sent FIN; no more bytes coming
    uint8_t  close_sent;       // We called netd_tcp_close (sent our FIN)
    uint8_t  _pad;
    uint32_t client_idx;       // Owner slot in g_clients (0xFFFFFFFFu = none)
    uint32_t cookie;           // What the client sees (stable)
    uint16_t rx_head;
    uint16_t rx_tail;
    uint8_t  rx_buf[NETD_TCP_RX_RING_BYTES];
} netd_tcp_conn_t;

static netd_tcp_conn_t g_tcp_conn[TCP_MAX_SOCKETS];

// Pending TCP opens: client fired TCP_OPEN_REQ; netd replies once
// ESTABLISHED (or timeout). Matched by (client_idx, socket_idx).
// Phase 22 closeout (G3): scaled from 16 → 1024 to match TCP_MAX_SOCKETS.
// Per-table cost: sizeof(pending_tcp_open_t) ≈ 32 B × 1024 = 32 KiB.
#define NETD_MAX_PENDING_TCP_OPEN   1024u

typedef struct pending_tcp_open {
    uint8_t   in_use;
    uint8_t   _pad[3];
    uint32_t  client_idx;
    uint32_t  socket_idx;
    uint32_t  req_seq;
    uint64_t  deadline_tsc;
} pending_tcp_open_t;

static pending_tcp_open_t g_tcp_opens[NETD_MAX_PENDING_TCP_OPEN];

// Pending TCP recvs: client fired TCP_RECV_REQ and asked netd to block.
// Phase 22 closeout (G3): scaled from 16 → 1024 to match TCP_MAX_SOCKETS.
// Per-table cost: sizeof(pending_tcp_recv_t) ≈ 32 B × 1024 = 32 KiB.
#define NETD_MAX_PENDING_TCP_RECV   1024u

typedef struct pending_tcp_recv {
    uint8_t   in_use;
    uint8_t   _pad[3];
    uint32_t  client_idx;
    uint32_t  socket_idx;
    uint32_t  req_seq;
    uint16_t  max_bytes;
    uint16_t  _pad2;
    uint64_t  deadline_tsc;
} pending_tcp_recv_t;

static pending_tcp_recv_t g_tcp_recvs[NETD_MAX_PENDING_TCP_RECV];

// Monotonically-increasing cookie allocator. 0 is reserved ("no cookie").
static uint32_t g_tcp_cookie_seed = 0x1000u;

// ---------------------------------------------------------------------
// Forward declarations. (Keeps the file readable top-down.)
// ---------------------------------------------------------------------
static int  tx_raw_frame(const uint8_t *frame, size_t len);
static int  tx_ipv4_datagram(uint32_t dst_ip, uint8_t proto,
                             const uint8_t *payload, size_t payload_len);

static void rx_dispatch_frame(const uint8_t *frame, size_t len);
static void rx_arp(const uint8_t *arp_pdu, size_t len,
                   const uint8_t src_mac[6]);
static void rx_ipv4(const uint8_t *ipv4_pdu, size_t len);
static void rx_icmp(const ipv4_parsed_t *ip);
static void rx_udp(const ipv4_parsed_t *ip);
static void rx_tcp(const ipv4_parsed_t *ip);

static void tcp_tick(uint64_t now_tsc);
static void tcp_on_socket_state_change(int socket_idx);
static void tcp_satisfy_pending_recv(int socket_idx);
static uint16_t tcp_ring_bytes(const netd_tcp_conn_t *c);
static uint16_t tcp_ring_push(netd_tcp_conn_t *c,
                              const uint8_t *src, size_t len);
static uint16_t tcp_ring_pop(netd_tcp_conn_t *c,
                             uint8_t *dst, size_t cap);

static void dhcp_kickoff(void);
static void dhcp_on_udp(uint16_t src_port, uint16_t dst_port,
                        const uint8_t *payload, size_t len);
static void dns_on_udp(uint16_t src_port, uint16_t dst_port,
                       const uint8_t *payload, size_t len);

static int  find_free_client_slot(void);
static void client_release(netd_client_t *c);
static void clients_dispatch_tick(void);
static int  client_handle_message(netd_client_t *c, const chan_msg_user_t *m);
static void client_send_error(netd_client_t *c, uint16_t resp_op,
                              uint32_t seq, int32_t status);

static void dns_tick(uint64_t now_tsc);
static void icmp_tick(uint64_t now_tsc);

// =====================================================================
// TX path.
// =====================================================================
static uint32_t tx_alloc_slot(void) {
    uint32_t slot = g_net.tx_cursor;
    g_net.tx_cursor = (g_net.tx_cursor + 1u) % g_net.slot_count;
    return slot;
}

static int tx_raw_frame(const uint8_t *frame, size_t len) {
    if (len == 0 || len > g_net.slot_size) return -5;
    if (!g_net.rawframe_wr_req.raw || !g_net.tx_ring_va) return -32 /* -ESHUTDOWN */;

    uint32_t slot = tx_alloc_slot();
    uint8_t *dst = (uint8_t *)(uintptr_t)(g_net.tx_ring_va +
                                         (uint64_t)slot * g_net.slot_size);
    memcpy(dst, frame, len);

    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(rawframe_slot_msg_t);
    m.header.nhandles   = 0;
    // The rawframe channel's type is grahaos.net.frame.v1 — it's stamped on
    // the per-connection channel by the kernel when the publisher declared
    // the payload_type_hash. The kernel verifies inline_len is consistent
    // with the declared size but does not cross-check this field at
    // chan_send time — still, set it for clarity.
    m.header.type_hash = 0;  // kernel infers from channel

    rawframe_slot_msg_t *rm = (rawframe_slot_msg_t *)m.inline_payload;
    rm->op     = RAWFRAME_OP_TX_REQ;
    rm->_pad[0] = rm->_pad[1] = rm->_pad[2] = 0;
    rm->slot   = slot;
    rm->length = (uint32_t)len;

    long rc = syscall_chan_send(g_net.rawframe_wr_req, &m,
                                200000000ull /* 200 ms */);
    if (rc < 0) return (int)rc;

    g_net.tx_packets++;
    g_net.tx_bytes += len;
    return 0;
}

static int tx_ipv4_datagram(uint32_t dst_ip, uint8_t proto,
                            const uint8_t *payload, size_t payload_len) {
    if (payload_len > ETH_MAX_PAYLOAD - IPV4_HDR_LEN_MIN) return -5;

    uint8_t scratch[ETH_MAX_FRAME];
    if (14 + IPV4_HDR_LEN_MIN + payload_len > sizeof(scratch)) return -5;

    // Route: on-link vs gateway.
    uint32_t next_hop = dst_ip;
    if (g_net.gateway && (dst_ip & g_net.netmask) != (g_net.ip & g_net.netmask)) {
        next_hop = g_net.gateway;
    }
    if (next_hop == 0xFFFFFFFFu) {
        // Directed broadcast: just send to FF:FF:FF:FF:FF:FF.
    }

    uint8_t dst_mac[6];
    uint8_t is_bcast = (next_hop == 0xFFFFFFFFu) ? 1u : 0u;
    if (is_bcast) {
        memcpy(dst_mac, netd_eth_bcast, 6);
    } else {
        uint8_t req_buf[ARP_FRAME_LEN];
        size_t  req_len = 0;
        int arc = netd_arp_resolve(&g_net.arp, g_net.mac, g_net.ip, next_hop,
                                   netd_rdtsc(),
                                   (uint64_t)ARP_TTL_SECONDS * NETD_TICKS_PER_SEC,
                                   dst_mac, req_buf, sizeof(req_buf), &req_len);
        if (arc == 0) {
            // Need resolution. Send the ARP request; drop this packet — the
            // caller can retry once cache is populated. For DHCP this is
            // fine: DISCOVER is broadcast (no ARP).
            if (req_len > 0) (void)tx_raw_frame(req_buf, req_len);  // ARP request (broadcast)
            return -11 /* -EAGAIN */;
        }
        if (arc < 0) return arc;
    }

    uint8_t *p = scratch;
    p += netd_eth_build(p, dst_mac, g_net.mac, ETH_TYPE_IPV4);
    p += netd_ipv4_build_header(p, g_net.ip, dst_ip, proto,
                                (uint16_t)netd_rand32(),
                                (uint16_t)payload_len, IPV4_DEFAULT_TTL);
    memcpy(p, payload, payload_len);
    p += payload_len;
    return tx_raw_frame(scratch, (size_t)(p - scratch));
}

// =====================================================================
// RX dispatch.
// =====================================================================
static void rx_dispatch_frame(const uint8_t *frame, size_t len) {
    uint8_t dst[6], src[6];
    uint16_t ethertype = 0;
    int rc = netd_eth_parse(frame, len, dst, src, &ethertype);
    if (rc < 0) return;

    // Drop frames not for us unless broadcast/multicast we care about.
    if (!netd_mac_eq(dst, g_net.mac) &&
        !netd_mac_eq(dst, netd_eth_bcast)) {
        return;
    }

    g_net.rx_packets++;
    g_net.rx_bytes += len;

    if (ethertype == ETH_TYPE_ARP) {
        rx_arp(frame + 14, len - 14, src);
    } else if (ethertype == ETH_TYPE_IPV4) {
        rx_ipv4(frame + 14, len - 14);
    }
    // Other ethertypes (IPv6, LLDP, ...) dropped silently.
}

static void rx_arp(const uint8_t *arp_pdu, size_t len, const uint8_t src_mac[6]) {
    (void)src_mac;
    uint8_t reply_buf[ARP_FRAME_LEN];
    size_t  reply_len = 0;
    int rc = netd_arp_handle_incoming(&g_net.arp, arp_pdu, len,
                                      g_net.mac, g_net.ip,
                                      netd_rdtsc(),
                                      (uint64_t)ARP_TTL_SECONDS *
                                          NETD_TICKS_PER_SEC,
                                      reply_buf, sizeof(reply_buf), &reply_len);
    if (rc == 0 && reply_len > 0) {
        (void)tx_raw_frame(reply_buf, reply_len);
    }
}

static void rx_ipv4(const uint8_t *ipv4_pdu, size_t len) {
    ipv4_parsed_t ip;
    if (netd_ipv4_parse(ipv4_pdu, len, &ip) != 0) return;
    if (ip.dst != g_net.ip && ip.dst != 0xFFFFFFFFu) {
        // Accept only unicast-to-us or broadcast.
        return;
    }
    if (ip.proto == IPPROTO_ICMP)      rx_icmp(&ip);
    else if (ip.proto == IPPROTO_UDP)  rx_udp(&ip);
    else if (ip.proto == IPPROTO_TCP)  rx_tcp(&ip);
}

static void rx_icmp(const ipv4_parsed_t *ip) {
    // First, check if this is a reply to an in-flight ping.
    uint8_t type = 0, code = 0;
    uint16_t id = 0, seq = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int pr = netd_icmp_parse_echo(ip->payload, ip->payload_len,
                                  &type, &code, &id, &seq,
                                  &payload, &payload_len);
    if (pr == 0 && type == ICMP_TYPE_ECHO_REPLY) {
        for (uint32_t i = 0; i < NETD_MAX_PENDING_ICMP; i++) {
            pending_icmp_t *pi = &g_icmps[i];
            if (!pi->in_use) continue;
            if (pi->dst_ip != ip->src) continue;
            if (pi->id != id || pi->seq != seq) continue;

            // Build the echo response to the user client.
            libnet_icmp_echo_resp_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.hdr.op  = LIBNET_OP_ICMP_ECHO_RESP;
            resp.hdr.seq = pi->req_seq;
            resp.status  = 0;
            resp.src_ip  = ip->src;
            resp.id      = id;
            resp.seq     = seq;
            uint64_t now = netd_rdtsc();
            uint64_t dt  = now - pi->start_tsc;
            resp.rtt_ns  = (dt * 1000000000ull) / NETD_TICKS_PER_SEC;
            resp.payload_len = pi->payload_len;
            if (pi->payload_len)
                memcpy(resp.payload, pi->payload, pi->payload_len);

            netd_client_t *c = &g_clients[pi->client_idx];
            if (c->in_use) {
                chan_msg_user_t m;
                memset(&m, 0, sizeof(m));
                m.header.inline_len = (uint16_t)sizeof(resp);
                memcpy(m.inline_payload, &resp, sizeof(resp));
                (void)syscall_chan_send(c->wr_resp, &m,
                                        200000000ull /* 200 ms */);
            }
            pi->in_use = 0;
            return;
        }
        // Unsolicited reply → drop.
        return;
    }

    // Otherwise: respond to ECHO REQUEST if dst=us.
    uint8_t reply_buf[ETH_MAX_PAYLOAD];   // IP+ICMP+payload
    size_t  reply_len = 0;
    int ir = netd_icmp_handle_incoming(ip, g_net.ip,
                                       reply_buf, sizeof(reply_buf),
                                       &reply_len);
    if (ir == 0 && reply_len > 0) {
        // reply_buf contains IPv4 + ICMP. Skip the IPv4 header and hand the
        // ICMP payload to tx_ipv4_datagram (which rebuilds the IPv4 header).
        if (reply_len > IPV4_HDR_LEN_MIN) {
            (void)tx_ipv4_datagram(ip->src, IPPROTO_ICMP,
                                   reply_buf + IPV4_HDR_LEN_MIN,
                                   reply_len - IPV4_HDR_LEN_MIN);
        }
    }
}

static void rx_udp(const ipv4_parsed_t *ip) {
    uint16_t src_port = 0, dst_port = 0;
    const uint8_t *payload = NULL;
    size_t plen = 0;
    int rc = netd_udp_parse(ip->payload, ip->payload_len,
                            ip->src, ip->dst,
                            &src_port, &dst_port, &payload, &plen);
    if (rc != 0) return;

    // DHCP client port = 68; server = 67.
    if (src_port == 67 && dst_port == 68) {
        dhcp_on_udp(src_port, dst_port, payload, plen);
        return;
    }
    // DNS replies come from src_port=53, dst_port=our ephemeral.
    if (src_port == 53) {
        dns_on_udp(src_port, dst_port, payload, plen);
        return;
    }
    // Other UDP: Stage D (user-bound sockets deliver into client channel).
}

// -- TCP ring helpers ----------------------------------------------------
static uint16_t tcp_ring_bytes(const netd_tcp_conn_t *c) {
    // head = write cursor, tail = read cursor. Count = (head - tail) mod N.
    uint16_t n = (uint16_t)(c->rx_head - c->rx_tail);
    return (uint16_t)(n % NETD_TCP_RX_RING_BYTES);
}

static uint16_t tcp_ring_push(netd_tcp_conn_t *c,
                              const uint8_t *src, size_t len) {
    // Drop overflow — HTTP MVP is best-effort. Record counter later if we
    // ever need congestion signalling through the ring.
    uint16_t avail =
        (uint16_t)(NETD_TCP_RX_RING_BYTES - 1u - tcp_ring_bytes(c));
    if (len > avail) len = avail;
    for (size_t i = 0; i < len; i++) {
        c->rx_buf[c->rx_head % NETD_TCP_RX_RING_BYTES] = src[i];
        c->rx_head = (uint16_t)((c->rx_head + 1u) % NETD_TCP_RX_RING_BYTES);
    }
    return (uint16_t)len;
}

static uint16_t tcp_ring_pop(netd_tcp_conn_t *c, uint8_t *dst, size_t cap) {
    uint16_t avail = tcp_ring_bytes(c);
    uint16_t n = (cap < avail) ? (uint16_t)cap : avail;
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = c->rx_buf[c->rx_tail % NETD_TCP_RX_RING_BYTES];
        c->rx_tail = (uint16_t)((c->rx_tail + 1u) % NETD_TCP_RX_RING_BYTES);
    }
    return n;
}

// Emit a TCP segment with the given flags + payload over IPv4. The helper
// fabricates a tiny scratch buffer holding the segment then hands it to
// tx_ipv4_datagram which prepends IP + Ethernet. On ARP miss the caller
// gets -EAGAIN propagated.
static int tcp_emit_segment(const tcp_socket_t *sock,
                            uint32_t seq, uint32_t ack,
                            uint8_t flags,
                            const uint8_t *payload, size_t payload_len) {
    uint8_t scratch[TCP_HDR_LEN_MIN + 4 + 512];
    if (payload_len > 512) return -5;
    size_t seg_len = netd_tcp_build(scratch,
                                    sock->local_ip, sock->remote_ip,
                                    sock->local_port, sock->remote_port,
                                    seq, ack,
                                    flags, (uint16_t)sock->rcv_wnd,
                                    0, payload, payload_len);
    if (seg_len == 0) return -5;
    return tx_ipv4_datagram(sock->remote_ip, IPPROTO_TCP, scratch, seg_len);
}

static void rx_tcp(const ipv4_parsed_t *ip) {
    tcp_parsed_t pkt;
    if (netd_tcp_parse(ip->payload, ip->payload_len, ip->src, ip->dst, &pkt) != 0) {
        return;
    }

    // Find an established / transitioning socket for this 4-tuple.
    int idx = netd_tcp_find_established(&g_net.tcp,
                                        ip->dst, pkt.dst_port,
                                        ip->src, pkt.src_port);
    // SYN_SENT: socket's remote_* match before ESTABLISHED too, so the
    // above lookup already covers it. No LISTEN state in Stage D client
    // flow.
    if (idx < 0) return;

    tcp_socket_t *sock = &g_net.tcp.sockets[idx];
    netd_tcp_conn_t *conn = &g_tcp_conn[idx];
    if (!conn->in_use) return;

    // Feed the parsed segment to the state machine.
    uint8_t resp_buf[TCP_HDR_LEN_MIN + 4];
    size_t  resp_len = 0;
    uint64_t now = netd_rdtsc();
    uint8_t prev_state = sock->state;
    int rc = netd_tcp_on_segment(sock, &pkt, now, NETD_TICKS_PER_SEC,
                                 resp_buf, sizeof(resp_buf), &resp_len);
    (void)rc;

    // If any control segment came back, emit it (ACK / SYN-ACK / FIN-ACK).
    if (resp_len > 0) {
        (void)tx_ipv4_datagram(sock->remote_ip, IPPROTO_TCP,
                               resp_buf, resp_len);
    }

    // Stash payload bytes in the per-socket rx ring. netd_tcp_on_segment
    // only advances rcv_nxt; buffering is our responsibility.
    if (pkt.payload_len > 0 && pkt.seq == sock->irs + 1u + /* dropped? */ 0) {
        // on_segment already bumped rcv_nxt; the segment was accepted iff
        // pkt.seq == prev rcv_nxt == sock->rcv_nxt - pkt.payload_len.
        if (sock->rcv_nxt == pkt.seq + (uint32_t)pkt.payload_len) {
            (void)tcp_ring_push(conn, pkt.payload, pkt.payload_len);
        }
    } else if (pkt.payload_len > 0 &&
               sock->rcv_nxt == pkt.seq + (uint32_t)pkt.payload_len) {
        (void)tcp_ring_push(conn, pkt.payload, pkt.payload_len);
    }

    if (pkt.flags & TCP_FLAG_FIN) {
        conn->peer_fin = 1;
    }

    // State-change fan-out: pending_open fires on ESTABLISHED, pending_recv
    // is satisfied whenever ring grows or peer_fin arrives.
    if (sock->state != prev_state) {
        tcp_on_socket_state_change(idx);
    }
    tcp_satisfy_pending_recv(idx);
}

// =====================================================================
// DHCP.
// =====================================================================
static void dhcp_kickoff(void) {
    if (g_net.dhcp.state == DHCP_STATE_INIT) {
        uint8_t payload[512];
        size_t  plen = netd_dhcp_build_discover(&g_net.dhcp, payload,
                                                sizeof(payload));
        if (plen == 0) return;

        // Build UDP packet manually (src_ip=0.0.0.0, dst_ip=255.255.255.255,
        // src_port=68, dst_port=67). Our tx_ipv4_datagram uses g_net.ip
        // which is still 0 at this point — and that's what we want.
        uint8_t udp_pkt[8 + 512];
        size_t  udp_len = netd_udp_build(udp_pkt,
                                         0 /*src_ip*/, 0xFFFFFFFFu /*dst_ip*/,
                                         68, 67,
                                         payload, plen);

        // Build IPv4 + Ethernet directly (tx_ipv4_datagram uses g_net.ip).
        uint8_t scratch[ETH_MAX_FRAME];
        uint8_t *p = scratch;
        p += netd_eth_build(p, netd_eth_bcast, g_net.mac, ETH_TYPE_IPV4);
        p += netd_ipv4_build_header(p, 0 /*src*/, 0xFFFFFFFFu /*dst*/,
                                    IPPROTO_UDP, (uint16_t)netd_rand32(),
                                    (uint16_t)udp_len, IPV4_DEFAULT_TTL);
        memcpy(p, udp_pkt, udp_len);
        p += udp_len;
        (void)tx_raw_frame(scratch, (size_t)(p - scratch));
        printf("[netd] DHCPDISCOVER sent\n");
    }
}

static void dhcp_send_request(void) {
    uint8_t payload[512];
    size_t  plen = netd_dhcp_build_request(&g_net.dhcp, payload, sizeof(payload));
    if (plen == 0) return;

    uint8_t udp_pkt[8 + 512];
    size_t  udp_len = netd_udp_build(udp_pkt,
                                     0 /*src*/, 0xFFFFFFFFu /*dst*/,
                                     68, 67,
                                     payload, plen);
    uint8_t scratch[ETH_MAX_FRAME];
    uint8_t *p = scratch;
    p += netd_eth_build(p, netd_eth_bcast, g_net.mac, ETH_TYPE_IPV4);
    p += netd_ipv4_build_header(p, 0, 0xFFFFFFFFu, IPPROTO_UDP,
                                (uint16_t)netd_rand32(),
                                (uint16_t)udp_len, IPV4_DEFAULT_TTL);
    memcpy(p, udp_pkt, udp_len);
    p += udp_len;
    (void)tx_raw_frame(scratch, (size_t)(p - scratch));
    printf("[netd] DHCPREQUEST sent for %u.%u.%u.%u\n",
           (g_net.dhcp.offered_ip >> 24) & 0xFF,
           (g_net.dhcp.offered_ip >> 16) & 0xFF,
           (g_net.dhcp.offered_ip >> 8) & 0xFF,
           g_net.dhcp.offered_ip & 0xFF);
}

static void dhcp_on_udp(uint16_t src_port, uint16_t dst_port,
                        const uint8_t *payload, size_t len) {
    (void)src_port; (void)dst_port;
    int mt = netd_dhcp_handle_incoming(&g_net.dhcp, payload, len);
    if (mt == DHCP_MT_OFFER) {
        dhcp_send_request();
    } else if (mt == DHCP_MT_ACK) {
        g_net.ip           = g_net.dhcp.assigned_ip;
        g_net.netmask      = g_net.dhcp.subnet_mask;
        g_net.gateway      = g_net.dhcp.router;
        g_net.dns_resolver = g_net.dhcp.dns;
        g_net.stack_running = 1;
        printf("[netd] DHCP bound: ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
               (g_net.ip >> 24) & 0xFF, (g_net.ip >> 16) & 0xFF,
               (g_net.ip >> 8) & 0xFF, g_net.ip & 0xFF,
               (g_net.gateway >> 24) & 0xFF, (g_net.gateway >> 16) & 0xFF,
               (g_net.gateway >> 8) & 0xFF, g_net.gateway & 0xFF,
               (g_net.dns_resolver >> 24) & 0xFF,
               (g_net.dns_resolver >> 16) & 0xFF,
               (g_net.dns_resolver >> 8) & 0xFF,
               g_net.dns_resolver & 0xFF);
    }
}

// =====================================================================
// DNS resolver — stateful wrapper over netd_dns_build_query + parse.
// =====================================================================
static int dns_submit(uint32_t client_idx, uint32_t req_seq,
                      const char *hostname, size_t hlen,
                      uint32_t timeout_ms) {
    if (!g_net.stack_running || g_net.dns_resolver == 0) {
        return -101 /* -ENETUNREACH */;
    }
    if (timeout_ms == 0) timeout_ms = 5000u;

    // Find a pending-DNS slot.
    pending_dns_t *slot = NULL;
    for (uint32_t i = 0; i < NETD_MAX_PENDING_DNS; i++) {
        if (!g_dns[i].in_use) { slot = &g_dns[i]; break; }
    }
    if (!slot) return -11 /* -EAGAIN */;

    uint16_t xid      = (uint16_t)(netd_rand32() | 1u);
    uint16_t src_port = (uint16_t)(49152u + (netd_rand32() % 16384u));

    uint8_t qbuf[300];
    int qlen = netd_dns_build_query(qbuf, sizeof(qbuf), xid, hostname, hlen);
    if (qlen < 0) return -5;

    // Wrap in UDP + IPv4.
    uint8_t udp_pkt[400];
    size_t  udp_len = netd_udp_build(udp_pkt,
                                     g_net.ip, g_net.dns_resolver,
                                     src_port, 53,
                                     qbuf, (size_t)qlen);

    int tc = tx_ipv4_datagram(g_net.dns_resolver, IPPROTO_UDP,
                              udp_pkt, udp_len);
    if (tc < 0 && tc != -11 /* EAGAIN — ARP resolve in flight */) {
        return tc;
    }

    slot->in_use       = 1;
    slot->xid          = xid;
    slot->src_port     = src_port;
    slot->client_idx   = client_idx;
    slot->req_seq      = req_seq;
    slot->deadline_tsc = netd_rdtsc() +
                         (uint64_t)timeout_ms * (NETD_TICKS_PER_SEC / 1000u);
    return 0;
}

static void dns_reply_to_client(pending_dns_t *slot,
                                const dns_query_result_t *result,
                                int32_t status) {
    netd_client_t *c = &g_clients[slot->client_idx];
    if (!c->in_use) { slot->in_use = 0; return; }

    libnet_dns_query_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_DNS_QUERY_RESP;
    resp.hdr.seq = slot->req_seq;
    resp.status  = status;
    if (result) {
        resp.answer_count = result->ip_count;
        for (uint32_t i = 0; i < result->ip_count && i < LIBNET_DNS_MAX_ANSWERS; i++) {
            resp.answers[i] = result->ips[i];
        }
        resp.ttl_seconds = result->ttl_seconds;
    }
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    slot->in_use = 0;
}

static void dns_on_udp(uint16_t src_port, uint16_t dst_port,
                       const uint8_t *payload, size_t len) {
    (void)src_port;
    // Find a pending slot matching dst_port (our assigned ephemeral).
    for (uint32_t i = 0; i < NETD_MAX_PENDING_DNS; i++) {
        pending_dns_t *slot = &g_dns[i];
        if (!slot->in_use) continue;
        if (slot->src_port != dst_port) continue;

        dns_query_result_t r;
        memset(&r, 0, sizeof(r));
        int pr = netd_dns_parse_response(payload, len, slot->xid, &r);
        if (pr < 0) continue;

        int32_t status = 0;
        if (r.rcode != DNS_RCODE_NOERROR || r.ip_count == 0) {
            status = -2 /* -ENOENT */;
        }
        dns_reply_to_client(slot, &r, status);
        return;
    }
}

static void dns_tick(uint64_t now_tsc) {
    for (uint32_t i = 0; i < NETD_MAX_PENDING_DNS; i++) {
        pending_dns_t *slot = &g_dns[i];
        if (!slot->in_use) continue;
        if ((int64_t)(now_tsc - slot->deadline_tsc) < 0) continue;
        dns_reply_to_client(slot, NULL, -110 /* -ETIMEDOUT */);
    }
}

static void icmp_tick(uint64_t now_tsc) {
    for (uint32_t i = 0; i < NETD_MAX_PENDING_ICMP; i++) {
        pending_icmp_t *pi = &g_icmps[i];
        if (!pi->in_use) continue;
        if ((int64_t)(now_tsc - pi->deadline_tsc) < 0) continue;

        libnet_icmp_echo_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.hdr.op  = LIBNET_OP_ICMP_ECHO_RESP;
        resp.hdr.seq = pi->req_seq;
        resp.status  = -110 /* -ETIMEDOUT */;
        resp.src_ip  = pi->dst_ip;
        resp.id      = pi->id;
        resp.seq     = pi->seq;
        resp.payload_len = pi->payload_len;
        if (pi->payload_len) memcpy(resp.payload, pi->payload, pi->payload_len);

        netd_client_t *c = &g_clients[pi->client_idx];
        if (c->in_use) {
            chan_msg_user_t m;
            memset(&m, 0, sizeof(m));
            m.header.inline_len = (uint16_t)sizeof(resp);
            memcpy(m.inline_payload, &resp, sizeof(resp));
            (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
        }
        pi->in_use = 0;
    }
}

// =====================================================================
// Service accept + per-client dispatch.
// =====================================================================
static int find_free_client_slot(void) {
    for (int i = 0; i < (int)NETD_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) return i;
    }
    return -1;
}

static void client_release(netd_client_t *c) {
    memset(c, 0, sizeof(*c));
}

static void client_send_error(netd_client_t *c, uint16_t resp_op,
                              uint32_t seq, int32_t status) {
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    uint16_t n = libnet_msg_build_err_response(m.inline_payload,
                                                CHAN_MSG_INLINE_MAX,
                                                resp_op, seq, status);
    if (n == 0) return;
    m.header.inline_len = n;
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void handle_hello(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_hello_req_t *req = (const libnet_hello_req_t *)in->inline_payload;
    libnet_hello_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_HELLO_RESP;
    resp.hdr.seq = req->hdr.seq;
    resp.status  = 0;
    resp.server_pid = g_net.pid;
    resp.server_tsc = netd_rdtsc();
    resp.protocol_version = 1u;

    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void handle_net_query(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_net_query_req_t *req =
        (const libnet_net_query_req_t *)in->inline_payload;

    libnet_net_query_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_NET_QUERY_RESP;
    resp.hdr.seq = req->hdr.seq;
    resp.status  = 0;
    resp.field   = req->field;
    resp.ip      = g_net.ip;
    resp.netmask = g_net.netmask;
    resp.gateway = g_net.gateway;
    resp.dns     = g_net.dns_resolver;
    memcpy(resp.mac, g_net.mac, 6);
    resp.link_up       = g_net.link_up;
    resp.stack_running = g_net.stack_running;
    resp.rx_packets    = g_net.rx_packets;
    resp.tx_packets    = g_net.tx_packets;
    resp.rx_bytes      = g_net.rx_bytes;
    resp.tx_bytes      = g_net.tx_bytes;
    resp.arp_entries_count = netd_arp_count(&g_net.arp);
    uint32_t tcp_count = 0, udp_count = 0;
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++)
        if (g_net.tcp.sockets[i].owner_cookie) tcp_count++;
    for (uint32_t i = 0; i < UDP_MAX_SOCKETS; i++)
        if (g_net.udp.sockets[i].state == UDP_STATE_BOUND) udp_count++;
    resp.tcp_sockets_count = tcp_count;
    resp.udp_sockets_count = udp_count;

    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void handle_dns_query(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_dns_query_req_t *req =
        (const libnet_dns_query_req_t *)in->inline_payload;
    uint32_t client_idx = (uint32_t)(c - g_clients);
    if (req->name_len == 0 || req->name_len > LIBNET_DNS_MAX_NAME) {
        client_send_error(c, LIBNET_OP_DNS_QUERY_RESP, req->hdr.seq, -5);
        return;
    }
    if (req->qtype != 1u) {
        client_send_error(c, LIBNET_OP_DNS_QUERY_RESP, req->hdr.seq, -6 /* -ENOSYS */);
        return;
    }
    int rc = dns_submit(client_idx, req->hdr.seq,
                        (const char *)req->name, req->name_len,
                        req->timeout_ms);
    if (rc < 0) {
        libnet_dns_query_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.hdr.op  = LIBNET_OP_DNS_QUERY_RESP;
        resp.hdr.seq = req->hdr.seq;
        resp.status  = rc;
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        m.header.inline_len = (uint16_t)sizeof(resp);
        memcpy(m.inline_payload, &resp, sizeof(resp));
        (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    }
    // Otherwise the reply will arrive asynchronously when the DNS UDP
    // response comes in (or dns_tick() times out).
}

static void handle_icmp_echo(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_icmp_echo_req_t *req =
        (const libnet_icmp_echo_req_t *)in->inline_payload;
    if (!g_net.stack_running) {
        client_send_error(c, LIBNET_OP_ICMP_ECHO_RESP, req->hdr.seq,
                          -101 /* -ENETUNREACH */);
        return;
    }
    if (req->payload_len > sizeof(req->payload)) {
        client_send_error(c, LIBNET_OP_ICMP_ECHO_RESP, req->hdr.seq, -5);
        return;
    }

    pending_icmp_t *pi = NULL;
    for (uint32_t i = 0; i < NETD_MAX_PENDING_ICMP; i++) {
        if (!g_icmps[i].in_use) { pi = &g_icmps[i]; break; }
    }
    if (!pi) {
        client_send_error(c, LIBNET_OP_ICMP_ECHO_RESP, req->hdr.seq, -11);
        return;
    }

    uint32_t timeout_ms = req->timeout_ms ? req->timeout_ms : 3000u;
    pi->in_use       = 1;
    pi->id           = req->id;
    pi->seq          = req->seq;
    pi->dst_ip       = req->dst_ip;
    pi->payload_len  = (uint8_t)req->payload_len;
    if (req->payload_len)
        memcpy(pi->payload, req->payload, req->payload_len);
    pi->client_idx   = (uint32_t)(c - g_clients);
    pi->req_seq      = req->hdr.seq;
    pi->start_tsc    = netd_rdtsc();
    pi->deadline_tsc = pi->start_tsc +
                       (uint64_t)timeout_ms * (NETD_TICKS_PER_SEC / 1000u);

    // Build ICMP ECHO REQUEST and send via IPv4.
    uint8_t icmp_pkt[8 + 64];
    size_t icmp_len = netd_icmp_build_echo(icmp_pkt,
                                           ICMP_TYPE_ECHO_REQUEST, 0,
                                           req->id, req->seq,
                                           req->payload, req->payload_len);
    int tc = tx_ipv4_datagram(req->dst_ip, IPPROTO_ICMP, icmp_pkt, icmp_len);
    if (tc < 0 && tc != -11) {
        pi->in_use = 0;
        client_send_error(c, LIBNET_OP_ICMP_ECHO_RESP, req->hdr.seq, tc);
    }
    // Else: reply (or timeout) will come asynchronously.
}

static void handle_udp_bind(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_udp_bind_req_t *req =
        (const libnet_udp_bind_req_t *)in->inline_payload;
    int idx = netd_udp_bind(&g_net.udp, req->local_port,
                            (uint32_t)(c - g_clients) + 1u);
    libnet_udp_bind_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_UDP_BIND_RESP;
    resp.hdr.seq = req->hdr.seq;
    if (idx < 0) {
        resp.status = idx;
    } else {
        resp.status     = 0;
        resp.cookie     = (uint32_t)idx + 1u;
        resp.local_port = g_net.udp.sockets[idx].local_port;
    }
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void handle_udp_sendto(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_udp_sendto_req_t *req =
        (const libnet_udp_sendto_req_t *)in->inline_payload;
    libnet_udp_sendto_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_UDP_SENDTO_RESP;
    resp.hdr.seq = req->hdr.seq;

    if (!g_net.stack_running) {
        resp.status = -101;
        goto send;
    }
    int idx = (int)req->cookie - 1;
    if (idx < 0 || idx >= (int)UDP_MAX_SOCKETS ||
        g_net.udp.sockets[idx].state != UDP_STATE_BOUND) {
        resp.status = -9 /* -EBADF */;
        goto send;
    }
    if (req->payload_len > sizeof(req->payload)) {
        resp.status = -5;
        goto send;
    }

    uint8_t udp_pkt[8 + 200];
    size_t udp_len = netd_udp_build(udp_pkt,
                                    g_net.ip, req->dst_ip,
                                    g_net.udp.sockets[idx].local_port,
                                    req->dst_port,
                                    req->payload, req->payload_len);
    int tc = tx_ipv4_datagram(req->dst_ip, IPPROTO_UDP, udp_pkt, udp_len);
    if (tc < 0 && tc != -11) {
        resp.status = tc;
    } else {
        resp.status     = 0;
        resp.bytes_sent = req->payload_len;
    }

send: {
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        m.header.inline_len = (uint16_t)sizeof(resp);
        memcpy(m.inline_payload, &resp, sizeof(resp));
        (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    }
}

// =====================================================================
// TCP — helpers for pending-op fanout + per-connection lifecycle.
// =====================================================================
static int find_socket_by_cookie(uint32_t cookie) {
    if (cookie == 0) return -1;
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (g_tcp_conn[i].in_use && g_tcp_conn[i].cookie == cookie) {
            return (int)i;
        }
    }
    return -1;
}

static void tcp_send_open_resp(int socket_idx, uint32_t client_idx,
                                uint32_t req_seq, int32_t status) {
    netd_client_t *c = &g_clients[client_idx];
    if (!c->in_use) return;

    libnet_tcp_open_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_TCP_OPEN_RESP;
    resp.hdr.seq = req_seq;
    resp.status  = status;
    if (status == 0 && socket_idx >= 0) {
        resp.socket_cookie = g_tcp_conn[socket_idx].cookie;
        resp.local_port    = g_net.tcp.sockets[socket_idx].local_port;
    }
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void tcp_send_recv_resp(uint32_t client_idx, uint32_t req_seq,
                                int32_t status,
                                const uint8_t *payload, uint16_t payload_len,
                                uint16_t flags) {
    netd_client_t *c = &g_clients[client_idx];
    if (!c->in_use) return;

    libnet_tcp_recv_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op      = LIBNET_OP_TCP_RECV_RESP;
    resp.hdr.seq     = req_seq;
    resp.status      = status;
    resp.payload_len = payload_len;
    resp.flags       = flags;
    if (payload_len && payload) memcpy(resp.payload, payload, payload_len);

    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(resp);
    memcpy(m.inline_payload, &resp, sizeof(resp));
    (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
}

static void tcp_on_socket_state_change(int socket_idx) {
    if (socket_idx < 0) return;
    tcp_socket_t *sock = &g_net.tcp.sockets[socket_idx];
    netd_tcp_conn_t *conn = &g_tcp_conn[socket_idx];

    // Fire any matching pending_open for this socket.
    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_OPEN; i++) {
        pending_tcp_open_t *po = &g_tcp_opens[i];
        if (!po->in_use) continue;
        if (po->socket_idx != (uint32_t)socket_idx) continue;
        if (sock->state == TCP_STATE_ESTABLISHED) {
            tcp_send_open_resp(socket_idx, po->client_idx, po->req_seq, 0);
            po->in_use = 0;
        } else if (sock->state == TCP_STATE_CLOSED) {
            tcp_send_open_resp(-1, po->client_idx, po->req_seq,
                               -111 /* -ECONNREFUSED */);
            po->in_use = 0;
            conn->in_use = 0;
            netd_tcp_socket_free(&g_net.tcp, socket_idx);
        }
    }
}

static void tcp_satisfy_pending_recv(int socket_idx) {
    if (socket_idx < 0) return;
    netd_tcp_conn_t *conn = &g_tcp_conn[socket_idx];

    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_RECV; i++) {
        pending_tcp_recv_t *pr = &g_tcp_recvs[i];
        if (!pr->in_use) continue;
        if (pr->socket_idx != (uint32_t)socket_idx) continue;

        uint16_t buffered = tcp_ring_bytes(conn);
        if (buffered == 0 && !conn->peer_fin) continue;

        uint16_t cap = pr->max_bytes ? pr->max_bytes : LIBNET_TCP_CHUNK_MAX;
        if (cap > LIBNET_TCP_CHUNK_MAX) cap = LIBNET_TCP_CHUNK_MAX;

        uint8_t buf[LIBNET_TCP_CHUNK_MAX];
        uint16_t n = tcp_ring_pop(conn, buf, cap);
        uint16_t flags = conn->peer_fin ? 1u : 0u;

        if (n == 0 && conn->peer_fin) {
            // No more data coming.
            tcp_send_recv_resp(pr->client_idx, pr->req_seq, -32 /*EPIPE*/,
                               NULL, 0, flags);
        } else {
            tcp_send_recv_resp(pr->client_idx, pr->req_seq, 0,
                               buf, n, flags);
        }
        pr->in_use = 0;
    }
}

static void handle_tcp_open(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_tcp_open_req_t *req =
        (const libnet_tcp_open_req_t *)in->inline_payload;

    if (!g_net.stack_running) {
        client_send_error(c, LIBNET_OP_TCP_OPEN_RESP, req->hdr.seq,
                          -101 /* -ENETUNREACH */);
        return;
    }
    if (req->dst_port == 0) {
        client_send_error(c, LIBNET_OP_TCP_OPEN_RESP, req->hdr.seq, -5);
        return;
    }

    uint32_t client_idx = (uint32_t)(c - g_clients);
    uint32_t cookie = g_tcp_cookie_seed++;
    if (cookie == 0) cookie = g_tcp_cookie_seed++;

    int idx = netd_tcp_socket_alloc(&g_net.tcp, cookie);
    if (idx < 0) {
        client_send_error(c, LIBNET_OP_TCP_OPEN_RESP, req->hdr.seq,
                          -11 /* -EAGAIN — table full */);
        return;
    }
    tcp_socket_t *sock = &g_net.tcp.sockets[idx];
    netd_tcp_conn_t *conn = &g_tcp_conn[idx];
    memset(conn, 0, sizeof(*conn));
    conn->in_use     = 1;
    conn->client_idx = client_idx;
    conn->cookie     = cookie;

    uint16_t local_port = (uint16_t)(NETD_TCP_EPHEMERAL_MIN +
                                     (netd_rand32() % NETD_TCP_EPHEMERAL_SPAN));
    sock->local_ip    = g_net.ip;
    sock->local_port  = local_port;
    sock->remote_ip   = req->dst_ip;
    sock->remote_port = req->dst_port;

    // Resolve ARP so we don't drop our own SYN. tx_ipv4_datagram handles
    // the ARP-miss path (returns -EAGAIN + emits request). We'll retransmit
    // via the TCP SYN timer on the next tick.
    uint8_t syn_buf[TCP_HDR_LEN_MIN + 4];
    size_t  syn_len = 0;
    int cr = netd_tcp_connect(sock, netd_rand32(),
                              syn_buf, sizeof(syn_buf), &syn_len,
                              TCP_DEFAULT_RTO_MS,
                              netd_rdtsc(), NETD_TICKS_PER_SEC);
    if (cr < 0) {
        netd_tcp_socket_free(&g_net.tcp, idx);
        conn->in_use = 0;
        client_send_error(c, LIBNET_OP_TCP_OPEN_RESP, req->hdr.seq, cr);
        return;
    }
    (void)tx_ipv4_datagram(sock->remote_ip, IPPROTO_TCP, syn_buf, syn_len);

    // Record pending open.
    pending_tcp_open_t *po = NULL;
    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_OPEN; i++) {
        if (!g_tcp_opens[i].in_use) { po = &g_tcp_opens[i]; break; }
    }
    if (!po) {
        // No room; fail the open synchronously.
        conn->in_use = 0;
        netd_tcp_socket_free(&g_net.tcp, idx);
        client_send_error(c, LIBNET_OP_TCP_OPEN_RESP, req->hdr.seq, -11);
        return;
    }
    uint32_t timeout_ms = req->timeout_ms ? req->timeout_ms : 10000u;
    po->in_use       = 1;
    po->client_idx   = client_idx;
    po->socket_idx   = (uint32_t)idx;
    po->req_seq      = req->hdr.seq;
    po->deadline_tsc = netd_rdtsc() +
                       (uint64_t)timeout_ms * (NETD_TICKS_PER_SEC / 1000u);
    // Reply arrives asynchronously in tcp_on_socket_state_change once
    // SYN-ACK lands.
}

static void handle_tcp_send(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_tcp_send_req_t *req =
        (const libnet_tcp_send_req_t *)in->inline_payload;
    libnet_tcp_send_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_TCP_SEND_RESP;
    resp.hdr.seq = req->hdr.seq;

    int idx = find_socket_by_cookie(req->socket_cookie);
    if (idx < 0) { resp.status = -9 /* EBADF */; goto send; }
    tcp_socket_t *sock = &g_net.tcp.sockets[idx];
    if (sock->state != TCP_STATE_ESTABLISHED &&
        sock->state != TCP_STATE_CLOSE_WAIT) {
        resp.status = -107 /* ENOTCONN */;
        goto send;
    }
    if (req->payload_len == 0 || req->payload_len > LIBNET_TCP_CHUNK_MAX) {
        resp.status = -5;
        goto send;
    }

    uint8_t flags = TCP_FLAG_ACK | TCP_FLAG_PSH;
    int tc = tcp_emit_segment(sock, sock->snd_nxt, sock->rcv_nxt, flags,
                              req->payload, req->payload_len);
    if (tc == -11 /* EAGAIN — ARP miss */) {
        // Bubble up so the caller can retry once the ARP cache populates.
        resp.status = -11;
        goto send;
    }
    if (tc < 0) { resp.status = tc; goto send; }

    sock->snd_nxt += (uint32_t)req->payload_len;
    resp.status     = 0;
    resp.bytes_sent = req->payload_len;

send: {
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        m.header.inline_len = (uint16_t)sizeof(resp);
        memcpy(m.inline_payload, &resp, sizeof(resp));
        (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    }
}

static void handle_tcp_recv(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_tcp_recv_req_t *req =
        (const libnet_tcp_recv_req_t *)in->inline_payload;

    int idx = find_socket_by_cookie(req->socket_cookie);
    if (idx < 0) {
        client_send_error(c, LIBNET_OP_TCP_RECV_RESP, req->hdr.seq, -9);
        return;
    }
    netd_tcp_conn_t *conn = &g_tcp_conn[idx];

    uint16_t cap = req->max_bytes ? req->max_bytes : LIBNET_TCP_CHUNK_MAX;
    if (cap > LIBNET_TCP_CHUNK_MAX) cap = LIBNET_TCP_CHUNK_MAX;
    uint16_t buffered = tcp_ring_bytes(conn);
    uint16_t flags = conn->peer_fin ? 1u : 0u;

    if (buffered > 0) {
        uint8_t buf[LIBNET_TCP_CHUNK_MAX];
        uint16_t n = tcp_ring_pop(conn, buf, cap);
        tcp_send_recv_resp((uint32_t)(c - g_clients), req->hdr.seq,
                           0, buf, n, flags);
        return;
    }
    if (conn->peer_fin) {
        tcp_send_recv_resp((uint32_t)(c - g_clients), req->hdr.seq,
                           -32 /* EPIPE */, NULL, 0, flags);
        return;
    }
    if (req->timeout_ms == 0) {
        tcp_send_recv_resp((uint32_t)(c - g_clients), req->hdr.seq,
                           -11 /* EAGAIN */, NULL, 0, flags);
        return;
    }

    pending_tcp_recv_t *pr = NULL;
    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_RECV; i++) {
        if (!g_tcp_recvs[i].in_use) { pr = &g_tcp_recvs[i]; break; }
    }
    if (!pr) {
        client_send_error(c, LIBNET_OP_TCP_RECV_RESP, req->hdr.seq, -11);
        return;
    }
    pr->in_use     = 1;
    pr->client_idx = (uint32_t)(c - g_clients);
    pr->socket_idx = (uint32_t)idx;
    pr->req_seq    = req->hdr.seq;
    pr->max_bytes  = cap;
    pr->deadline_tsc = netd_rdtsc() +
                       (uint64_t)req->timeout_ms *
                       (NETD_TICKS_PER_SEC / 1000u);
}

static void handle_tcp_close(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_tcp_close_req_t *req =
        (const libnet_tcp_close_req_t *)in->inline_payload;
    libnet_tcp_close_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_TCP_CLOSE_RESP;
    resp.hdr.seq = req->hdr.seq;

    int idx = find_socket_by_cookie(req->socket_cookie);
    if (idx < 0) { resp.status = -9; goto send; }
    tcp_socket_t *sock = &g_net.tcp.sockets[idx];
    netd_tcp_conn_t *conn = &g_tcp_conn[idx];

    uint8_t fin_buf[TCP_HDR_LEN_MIN + 4];
    size_t  fin_len = 0;
    int cr = netd_tcp_close(sock, fin_buf, sizeof(fin_buf), &fin_len);
    if (cr == 0 && fin_len > 0) {
        (void)tx_ipv4_datagram(sock->remote_ip, IPPROTO_TCP,
                               fin_buf, fin_len);
        conn->close_sent = 1;
    } else if (sock->state == TCP_STATE_CLOSED) {
        // Already closed; acceptable.
    }
    resp.status = 0;

send: {
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        m.header.inline_len = (uint16_t)sizeof(resp);
        memcpy(m.inline_payload, &resp, sizeof(resp));
        (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    }
}

static void handle_tcp_status(netd_client_t *c, const chan_msg_user_t *in) {
    const libnet_tcp_status_req_t *req =
        (const libnet_tcp_status_req_t *)in->inline_payload;
    libnet_tcp_status_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.op  = LIBNET_OP_TCP_STATUS_RESP;
    resp.hdr.seq = req->hdr.seq;

    int idx = find_socket_by_cookie(req->socket_cookie);
    if (idx < 0) { resp.status = -9; goto send; }
    tcp_socket_t *sock = &g_net.tcp.sockets[idx];
    netd_tcp_conn_t *conn = &g_tcp_conn[idx];
    resp.status      = 0;
    resp.state       = sock->state;
    resp.peer_fin    = conn->peer_fin;
    resp.rx_buffered = tcp_ring_bytes(conn);
    resp.tx_pending  = sock->snd_nxt - sock->snd_una;

send: {
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        m.header.inline_len = (uint16_t)sizeof(resp);
        memcpy(m.inline_payload, &resp, sizeof(resp));
        (void)syscall_chan_send(c->wr_resp, &m, 200000000ull);
    }
}

static void tcp_tick(uint64_t now_tsc) {
    // Per-socket timer tick: retransmit pending SYN / data, drive TIME_WAIT
    // expiry. Data retransmit buffer is tiny (24 B for the SYN option) —
    // data resends on snd_nxt advancement + netd_tcp_on_segment's ACK path
    // cover the rest for MVP.
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        netd_tcp_conn_t *conn = &g_tcp_conn[i];
        if (!conn->in_use) continue;
        tcp_socket_t *sock = &g_net.tcp.sockets[i];

        uint8_t prev_state = sock->state;
        uint8_t retx_buf[TCP_HDR_LEN_MIN + 4];
        size_t  retx_len = 0;
        (void)netd_tcp_tick(sock, now_tsc, NETD_TICKS_PER_SEC,
                             retx_buf, sizeof(retx_buf), &retx_len);
        if (retx_len > 0) {
            (void)tx_ipv4_datagram(sock->remote_ip, IPPROTO_TCP,
                                   retx_buf, retx_len);
        }
        if (sock->state != prev_state) {
            tcp_on_socket_state_change((int)i);
        }

        // Garbage-collect CLOSED sockets.
        if (sock->state == TCP_STATE_CLOSED && sock->owner_cookie != 0) {
            netd_tcp_socket_free(&g_net.tcp, (int)i);
            conn->in_use = 0;
        }
    }

    // Pending opens: fire -ETIMEDOUT on deadline.
    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_OPEN; i++) {
        pending_tcp_open_t *po = &g_tcp_opens[i];
        if (!po->in_use) continue;
        if ((int64_t)(now_tsc - po->deadline_tsc) < 0) continue;
        tcp_send_open_resp(-1, po->client_idx, po->req_seq,
                           -110 /* ETIMEDOUT */);
        // Release the half-open socket.
        netd_tcp_conn_t *conn = &g_tcp_conn[po->socket_idx];
        conn->in_use = 0;
        netd_tcp_socket_free(&g_net.tcp, (int)po->socket_idx);
        po->in_use = 0;
    }

    // Pending recvs: timeout.
    for (uint32_t i = 0; i < NETD_MAX_PENDING_TCP_RECV; i++) {
        pending_tcp_recv_t *pr = &g_tcp_recvs[i];
        if (!pr->in_use) continue;
        if ((int64_t)(now_tsc - pr->deadline_tsc) < 0) continue;
        tcp_send_recv_resp(pr->client_idx, pr->req_seq,
                           -110 /* ETIMEDOUT */, NULL, 0, 0);
        pr->in_use = 0;
    }
}

static int client_handle_message(netd_client_t *c, const chan_msg_user_t *m) {
    uint16_t op = 0;
    uint32_t seq = 0;
    if (libnet_msg_unpack_header(m, &op, &seq) < 0) return -5;
    switch (op) {
        case LIBNET_OP_HELLO_REQ:      handle_hello(c, m);       break;
        case LIBNET_OP_NET_QUERY_REQ:  handle_net_query(c, m);   break;
        case LIBNET_OP_DNS_QUERY_REQ:  handle_dns_query(c, m);   break;
        case LIBNET_OP_ICMP_ECHO_REQ:  handle_icmp_echo(c, m);   break;
        case LIBNET_OP_UDP_BIND_REQ:   handle_udp_bind(c, m);    break;
        case LIBNET_OP_UDP_SENDTO_REQ: handle_udp_sendto(c, m);  break;
        case LIBNET_OP_TCP_OPEN_REQ:   handle_tcp_open(c, m);    break;
        case LIBNET_OP_TCP_CLOSE_REQ:  handle_tcp_close(c, m);   break;
        case LIBNET_OP_TCP_SEND_REQ:   handle_tcp_send(c, m);    break;
        case LIBNET_OP_TCP_RECV_REQ:   handle_tcp_recv(c, m);    break;
        case LIBNET_OP_TCP_STATUS_REQ: handle_tcp_status(c, m);  break;
        default:
            client_send_error(c, op | LIBNET_OP_MASK_RESP, seq,
                              -6 /* -ENOSYS */);
            break;
    }
    return 0;
}

static void clients_dispatch_tick(void) {
    for (uint32_t i = 0; i < NETD_MAX_CLIENTS; i++) {
        netd_client_t *c = &g_clients[i];
        if (!c->in_use) continue;
        for (int drain = 0; drain < 8; drain++) {
            chan_msg_user_t m;
            memset(&m, 0, sizeof(m));
            long rc = syscall_chan_recv(c->rd_req, &m, 0 /*non-blocking*/);
            if (rc == -32 /*-EPIPE*/) {
                client_release(c);
                break;
            }
            if (rc < 0) break;   // EAGAIN / ETIMEDOUT
            (void)client_handle_message(c, &m);
        }
    }
}

// =====================================================================
// Rawframe RX poll — drain all pending RX_NOTIFYs in a tick.
// =====================================================================
static void rawframe_poll_rx(void) {
    for (int drain = 0; drain < 32; drain++) {
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        long rc = syscall_chan_recv(g_net.rawframe_rd_resp, &m, 0);
        if (rc < 0) break;
        if (m.header.inline_len < sizeof(rawframe_slot_msg_t)) continue;
        rawframe_slot_msg_t *sm = (rawframe_slot_msg_t *)m.inline_payload;
        if (sm->op == RAWFRAME_OP_RX_NOTIFY) {
            if (sm->slot >= g_net.slot_count) continue;
            if (sm->length == 0 || sm->length > g_net.slot_size) continue;
            uint8_t frame_copy[ETH_MAX_FRAME];
            size_t  flen = (sm->length > sizeof(frame_copy)) ?
                           sizeof(frame_copy) : sm->length;
            const uint8_t *src = (const uint8_t *)(uintptr_t)
                (g_net.rx_ring_va + (uint64_t)sm->slot * g_net.slot_size);
            memcpy(frame_copy, src, flen);
            rx_dispatch_frame(frame_copy, flen);
        } else if (sm->op == RAWFRAME_OP_LINK_UP) {
            g_net.link_up = 1;
            printf("[netd] rawframe: link UP\n");
            if (g_net.dhcp.state == DHCP_STATE_INIT) dhcp_kickoff();
        } else if (sm->op == RAWFRAME_OP_LINK_DOWN) {
            g_net.link_up = 0;
            g_net.stack_running = 0;
            printf("[netd] rawframe: link DOWN\n");
        }
    }
}

// =====================================================================
// Service accept drain.
// =====================================================================
static void service_accept_tick(libnet_service_ctx_t *svc) {
    for (int a = 0; a < 4; a++) {
        libnet_server_ctx_t srv;
        int rc = libnet_service_accept(svc, &srv, 0 /* non-blocking */);
        if (rc <= 0) break;
        int slot = find_free_client_slot();
        if (slot < 0) {
            printf("[netd] WARN: client table full, dropping connector_pid=%d\n",
                   srv.connector_pid);
            continue;
        }
        netd_client_t *c = &g_clients[slot];
        c->in_use        = 1;
        c->connector_pid = srv.connector_pid;
        c->connection_id = srv.connection_id;
        c->rd_req        = srv.rd_req;
        c->wr_resp       = srv.wr_resp;
        printf("[netd] client +%d: pid=%d conn=%u\n",
               slot, srv.connector_pid, (unsigned)srv.connection_id);
    }
}

// =====================================================================
// Entry.
// =====================================================================
static uint64_t fnv1a_hash64_local(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; s[i]; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

void _start(void) {
    printf("[netd] starting (Phase 22 Stage C)\n");

    memset(&g_net, 0, sizeof(g_net));
    g_net.pid = syscall_getpid();

    // 1. Connect to e1000d via /sys/net/rawframe.
    libnet_client_ctx_t rawframe;
    int rc = libnet_connect_service_with_retry("/sys/net/rawframe", 17,
                                                20000, &rawframe);
    if (rc < 0) {
        printf("[netd] FATAL: rawframe connect rc=%d\n", rc);
        syscall_exit(1);
    }
    g_net.rawframe_wr_req  = rawframe.wr_req;
    g_net.rawframe_rd_resp = rawframe.rd_resp;
    printf("[netd] connected to /sys/net/rawframe\n");

    // 2. Receive ANNOUNCE.
    {
        chan_msg_user_t am;
        memset(&am, 0, sizeof(am));
        long bytes = syscall_chan_recv(rawframe.rd_resp, &am, 5000000000ull);
        if (bytes < 0 || bytes < (long)sizeof(rawframe_announce_t) ||
            am.header.nhandles < 2) {
            printf("[netd] FATAL: ANNOUNCE recv rc=%ld\n", bytes);
            syscall_exit(4);
        }
        rawframe_announce_t *ab = (rawframe_announce_t *)am.inline_payload;
        if (ab->op != RAWFRAME_OP_ANNOUNCE) {
            printf("[netd] FATAL: first msg op=%u\n", (unsigned)ab->op);
            syscall_exit(4);
        }
        memcpy(g_net.mac, ab->mac, 6);
        g_net.link_up    = ab->link_up;
        g_net.slot_count = ab->slot_count;
        g_net.slot_size  = ab->slot_size;

        cap_token_u_t rx_vmo = { .raw = am.handles[0] };
        cap_token_u_t tx_vmo = { .raw = am.handles[1] };
        uint64_t bytes_per_ring = (uint64_t)g_net.slot_count * g_net.slot_size;
        long va = syscall_vmo_map(rx_vmo, 0, 0, bytes_per_ring,
                                   PROT_READ | PROT_WRITE);
        if (va <= 0) { printf("[netd] FATAL: vmo_map(rx)=%ld\n", va); syscall_exit(5); }
        g_net.rx_ring_va = (uint64_t)va;
        va = syscall_vmo_map(tx_vmo, 0, 0, bytes_per_ring,
                              PROT_READ | PROT_WRITE);
        if (va <= 0) { printf("[netd] FATAL: vmo_map(tx)=%ld\n", va); syscall_exit(5); }
        g_net.tx_ring_va = (uint64_t)va;

        printf("[netd] ANNOUNCE mac=%x:%x:%x:%x:%x:%x link=%u slots=%u size=%u\n",
               g_net.mac[0], g_net.mac[1], g_net.mac[2],
               g_net.mac[3], g_net.mac[4], g_net.mac[5],
               (unsigned)g_net.link_up, (unsigned)g_net.slot_count,
               (unsigned)g_net.slot_size);
    }

    // 3. Initialise protocol tables.
    netd_arp_init(&g_net.arp);
    netd_udp_table_init(&g_net.udp);
    netd_tcp_table_init(&g_net.tcp);
    netd_dhcp_init(&g_net.dhcp, g_net.mac, netd_rdtsc());
    memset(g_clients, 0, sizeof(g_clients));
    memset(g_dns, 0, sizeof(g_dns));
    memset(g_icmps, 0, sizeof(g_icmps));

    // 4. Publish /sys/net/service.
    libnet_service_ctx_t svc;
    uint64_t socket_hash = fnv1a_hash64_local("grahaos.net.socket.v1");
    rc = libnet_publish_service(&svc, "/sys/net/service", socket_hash);
    if (rc < 0) { printf("[netd] FATAL: publish rc=%d\n", rc); syscall_exit(2); }
    printf("[netd] /sys/net/service published\n");

    // 5. Kick off DHCP if link is already up (ANNOUNCE said so).
    if (g_net.link_up) dhcp_kickoff();

    // 6. Event loop. Each iteration: drain rawframe RX, drain accepts, poll
    //    each client, tick timers. Block briefly on rawframe rd_resp to
    //    save CPU when idle (short timeout so service accepts + timers
    //    still fire on time).
    printf("[netd] entering event loop\n");
    for (;;) {
        rawframe_poll_rx();
        service_accept_tick(&svc);
        clients_dispatch_tick();
        uint64_t now = netd_rdtsc();
        dns_tick(now);
        icmp_tick(now);
        tcp_tick(now);

        // Idle wait: blocking chan_recv on rawframe with a 25 ms budget.
        // This lets frames wake us immediately; in absence of frames we
        // re-enter the loop in ~25 ms and re-check accept + clients.
        chan_msg_user_t m;
        memset(&m, 0, sizeof(m));
        long rc2 = syscall_chan_recv(g_net.rawframe_rd_resp, &m,
                                     25000000ull /* 25 ms */);
        if (rc2 >= 0 && m.header.inline_len >= sizeof(rawframe_slot_msg_t)) {
            // Got something — handle it directly via the common path. We
            // "push back" by redispatching inline rather than re-recv'ing.
            rawframe_slot_msg_t *sm = (rawframe_slot_msg_t *)m.inline_payload;
            if (sm->op == RAWFRAME_OP_RX_NOTIFY &&
                sm->slot < g_net.slot_count &&
                sm->length > 0 && sm->length <= g_net.slot_size) {
                uint8_t frame_copy[ETH_MAX_FRAME];
                size_t  flen = (sm->length > sizeof(frame_copy)) ?
                               sizeof(frame_copy) : sm->length;
                const uint8_t *src = (const uint8_t *)(uintptr_t)
                    (g_net.rx_ring_va + (uint64_t)sm->slot * g_net.slot_size);
                memcpy(frame_copy, src, flen);
                rx_dispatch_frame(frame_copy, flen);
            } else if (sm->op == RAWFRAME_OP_LINK_UP) {
                g_net.link_up = 1;
                if (g_net.dhcp.state == DHCP_STATE_INIT) dhcp_kickoff();
            } else if (sm->op == RAWFRAME_OP_LINK_DOWN) {
                g_net.link_up = 0;
                g_net.stack_running = 0;
            }
        }
    }
}
