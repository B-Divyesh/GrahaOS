// user/netd.h — Phase 22 Stage B.
//
// Internal types + public helpers shared across netd's protocol modules:
//   netd_eth.c   — Ethernet L2 framing (RFC 894)
//   netd_arp.c   — Address Resolution Protocol (RFC 826)
//   netd_ipv4.c  — IPv4 (RFC 791, no TX fragmentation)
//   netd_icmp.c  — ICMP echo (RFC 792)
//   netd_udp.c   — UDP (RFC 768)
//   netd_tcp.c   — TCP Reno (RFC 793 + 5681 + 5961)
//   netd_dhcp.c  — DHCP client (RFC 2131)
//   netd.c       — daemon main loop, RX/TX path, service-accept dispatch
//
// Design principle: every module is a pure function of its inputs where
// practical so unit tests (user/tests/netd_*.tap.c) can drive the module
// directly without spawning the whole daemon. Global state is centralised
// in `netd_state_t` (netd.c) and passed by pointer.
//
// Byte order: the wire is network byte order (big-endian). All on-wire
// 16/32-bit integers are stored via the htons/htonl/ntohs/ntohl helpers
// declared below. Host-side structs use native-endian uint32_t for IPv4
// addresses unless a field is comment-tagged `be_` (big-endian stored).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ===========================================================================
// Byte-order helpers. Userspace has no endian.h; we assume x86_64 little-endian
// (an assumption built into the whole of GrahaOS).
// ===========================================================================
static inline uint16_t netd_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t netd_ntohs(uint16_t v) { return netd_htons(v); }
static inline uint32_t netd_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t netd_ntohl(uint32_t v) { return netd_htonl(v); }

// ===========================================================================
// L2: Ethernet II framing.
// ===========================================================================

#define ETH_ALEN        6u
#define ETH_HDR_LEN     14u
#define ETH_MIN_FRAME   60u      // Excluding FCS; NIC pads shorter frames
#define ETH_MAX_FRAME   1518u    // 14 eth + 1500 payload + 4 FCS (NIC strips)
#define ETH_MAX_PAYLOAD 1500u

// EtherType values we care about.
#define ETH_TYPE_IPV4   0x0800u
#define ETH_TYPE_ARP    0x0806u
#define ETH_TYPE_IPV6   0x86DDu  // Ignored by netd

typedef struct __attribute__((packed)) eth_header {
    uint8_t  dst_mac[ETH_ALEN];
    uint8_t  src_mac[ETH_ALEN];
    uint16_t ethertype;          // big-endian on the wire
} eth_header_t;

_Static_assert(sizeof(eth_header_t) == 14, "eth_header_t layout drift");

// Build an Ethernet header at `frame[0..13]`. `ethertype` is provided in host
// byte order and will be stored big-endian. Returns the size of the header
// (always 14). Caller is responsible for ensuring `frame` is at least
// ETH_HDR_LEN bytes and the payload follows starting at `frame[14]`.
size_t netd_eth_build(uint8_t *frame,
                      const uint8_t dst_mac[ETH_ALEN],
                      const uint8_t src_mac[ETH_ALEN],
                      uint16_t ethertype);

// Parse the leading 14 bytes of `frame` into out-parameters. `buf_len` is
// the total frame length (must be >= 14 or parsing fails). Returns 0 on
// success, negative on too-short / bogus.
int netd_eth_parse(const uint8_t *frame, size_t buf_len,
                   uint8_t out_dst[ETH_ALEN],
                   uint8_t out_src[ETH_ALEN],
                   uint16_t *out_ethertype);

// Well-known MACs.
extern const uint8_t netd_eth_bcast[ETH_ALEN];  // FF:FF:FF:FF:FF:FF
extern const uint8_t netd_eth_zero[ETH_ALEN];   // 00:00:00:00:00:00

// Compare two MACs. Returns 1 if equal, 0 otherwise.
int netd_mac_eq(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN]);

// ===========================================================================
// L3: ARP (RFC 826).
// ===========================================================================

#define ARP_HTYPE_ETH   1u       // Hardware type = Ethernet
#define ARP_PTYPE_IPV4  0x0800u  // Protocol type = IPv4
#define ARP_HLEN_ETH    6u
#define ARP_PLEN_IPV4   4u
#define ARP_OP_REQUEST  1u
#define ARP_OP_REPLY    2u

#define ARP_FRAME_LEN   42u      // 14 eth + 28 arp. Always padded by NIC to 60.
#define ARP_PAYLOAD_LEN 28u

// On-wire ARP packet layout.
typedef struct __attribute__((packed)) arp_packet {
    uint16_t htype;           // ARP_HTYPE_ETH (be)
    uint16_t ptype;           // ARP_PTYPE_IPV4 (be)
    uint8_t  hlen;            // ARP_HLEN_ETH
    uint8_t  plen;            // ARP_PLEN_IPV4
    uint16_t op;              // REQUEST / REPLY (be)
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;       // be
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;       // be
} arp_packet_t;

_Static_assert(sizeof(arp_packet_t) == 28, "arp_packet_t layout drift");

// ARP cache configuration.
// Phase 22 closeout (G3): scaled from 16 → 64 in step with TCP_MAX_SOCKETS=1024.
// At 1024 ESTABLISHED, ARP entries are bounded by # unique gateways/hosts in
// flight, not connections — 64 covers stress-test workloads comfortably.
#define ARP_TABLE_SLOTS   64u
#define ARP_TTL_SECONDS   60u
#define ARP_TTL_TSC(ticks_per_sec)  ((uint64_t)ARP_TTL_SECONDS * (ticks_per_sec))

// Per-entry state.
#define ARP_STATE_FREE       0u
#define ARP_STATE_RESOLVED   1u
#define ARP_STATE_PENDING    2u   // Request sent, no reply yet

typedef struct arp_entry {
    uint32_t ip;             // Host byte order
    uint8_t  mac[ETH_ALEN];
    uint8_t  state;          // ARP_STATE_*
    uint8_t  _pad;
    uint64_t expiry_tsc;     // For RESOLVED: when entry becomes stale
                             // For PENDING: when request times out
} arp_entry_t;

typedef struct arp_table {
    arp_entry_t entries[ARP_TABLE_SLOTS];
    // Hands out next-slot for insertion when full: round-robin LRU-ish.
    uint32_t    next_slot;
} arp_table_t;

// Initialise all slots to FREE.
void netd_arp_init(arp_table_t *tbl);

// Look up `ip` in the table. On hit, populates `out_mac` and returns 1.
// On miss, returns 0 (and leaves out_mac untouched). Does NOT age entries.
int netd_arp_lookup(const arp_table_t *tbl, uint32_t ip,
                    uint8_t out_mac[ETH_ALEN]);

// Insert or refresh. Always overwrites a matching IP; else claims a FREE slot;
// else LRU-evicts via round-robin cursor. Sets expiry = now_tsc + ttl_tsc.
// `state` must be RESOLVED or PENDING.
void netd_arp_insert(arp_table_t *tbl, uint32_t ip,
                     const uint8_t mac[ETH_ALEN],
                     uint8_t state, uint64_t now_tsc, uint64_t ttl_tsc);

// Sweep: mark RESOLVED entries past expiry as FREE; leave PENDING alone
// (PENDING expiry is handled by the request timer). Returns count aged.
uint32_t netd_arp_gc(arp_table_t *tbl, uint64_t now_tsc);

// Returns the number of RESOLVED entries (ignores FREE/PENDING). For
// introspection + ifconfig output.
uint32_t netd_arp_count(const arp_table_t *tbl);

// Build an ARP REQUEST frame asking "who has target_ip" from (my_ip, my_mac).
// Writes ARP_FRAME_LEN bytes to `out_frame` (caller's buffer must be >= 42).
// Returns ARP_FRAME_LEN.
size_t netd_arp_build_request(uint8_t *out_frame,
                              const uint8_t my_mac[ETH_ALEN],
                              uint32_t my_ip,
                              uint32_t target_ip);

// Build an ARP REPLY frame: "target_ip is at target_mac; asked by
// sender_ip/sender_mac". Returns ARP_FRAME_LEN. Called by the incoming
// handler when netd sees a request for its own IP.
size_t netd_arp_build_reply(uint8_t *out_frame,
                            const uint8_t my_mac[ETH_ALEN],
                            uint32_t my_ip,
                            const uint8_t requester_mac[ETH_ALEN],
                            uint32_t requester_ip);

// Parse an incoming ARP packet (already passed Ethernet check with
// ethertype=0x0806). `arp_buf` points to the 28-byte ARP PDU (Ethernet
// header already stripped). Returns 0 on valid packet, populating *out.
// Negative on malformed.
int netd_arp_parse(const uint8_t *arp_buf, size_t buf_len,
                   arp_packet_t *out);

// Process an ARP packet: update cache for the sender; if the packet is a
// REQUEST targeting our IP, write a REPLY frame into `reply_buf` (>=42 B)
// and set `*reply_len` to ARP_FRAME_LEN. Otherwise leave `*reply_len = 0`.
// Returns 0 on success, negative on malformed packet.
int netd_arp_handle_incoming(arp_table_t *tbl,
                             const uint8_t *arp_buf, size_t arp_len,
                             const uint8_t my_mac[ETH_ALEN],
                             uint32_t my_ip,
                             uint64_t now_tsc, uint64_t ttl_tsc,
                             uint8_t *reply_buf,
                             size_t reply_buf_cap,
                             size_t *reply_len);

// Resolve an IP to a MAC. On hit, populates out_mac, returns 1.
// On miss, inserts a PENDING entry (for dedup) and writes a request frame
// into req_buf (>= 42 B), sets *req_len = 42, returns 0.
// The caller is responsible for sending the request and retrying later.
int netd_arp_resolve(arp_table_t *tbl,
                     const uint8_t my_mac[ETH_ALEN],
                     uint32_t my_ip,
                     uint32_t target_ip,
                     uint64_t now_tsc, uint64_t pending_ttl_tsc,
                     uint8_t out_mac[ETH_ALEN],
                     uint8_t *req_buf,
                     size_t req_buf_cap,
                     size_t *req_len);

// ===========================================================================
// L3: IPv4 (RFC 791).
// ===========================================================================

#define IPV4_HDR_LEN_MIN   20u
#define IPV4_VERSION       4u
#define IPV4_DEFAULT_TTL   64u

// Common protocol numbers (IANA).
#define IPPROTO_ICMP   1u
#define IPPROTO_TCP    6u
#define IPPROTO_UDP   17u

// On-wire IPv4 header (no options). All multi-byte integers big-endian on
// the wire; host fields use native layout and the build/parse functions
// convert at the boundary.
typedef struct __attribute__((packed)) ipv4_header {
    uint8_t  vhl;            // (4 << 4) | ihl_words (5 for no options)
    uint8_t  tos;            // Type of Service / DSCP (usually 0)
    uint16_t total_len;      // Header + payload (big-endian)
    uint16_t id;             // Identification (big-endian)
    uint16_t flags_frag;     // Flags:3 + frag_offset:13 (big-endian)
    uint8_t  ttl;
    uint8_t  proto;          // IPPROTO_*
    uint16_t checksum;       // Header checksum (big-endian)
    uint32_t src;            // (big-endian)
    uint32_t dst;            // (big-endian)
} ipv4_header_t;

_Static_assert(sizeof(ipv4_header_t) == 20, "ipv4_header_t layout drift");

// Parsed IPv4 header in host byte order. `payload` points inside the caller's
// frame buffer (no copy). Options are rejected for MVP — netd validates IHL==5.
typedef struct ipv4_parsed {
    uint32_t src;            // Host byte order
    uint32_t dst;            // Host byte order
    uint8_t  proto;
    uint8_t  ttl;
    uint16_t total_len;      // Whole datagram length
    uint16_t id;
    uint16_t flags_frag;     // As-parsed
    const uint8_t *payload;  // Start of L4 payload
    size_t   payload_len;
} ipv4_parsed_t;

// Build a 20-byte IPv4 header into `out_hdr[0..19]`.
//   src, dst          — host byte order; will be stored big-endian
//   proto             — IPPROTO_*
//   id                — 16-bit identification (caller-chosen; typically monotonic)
//   payload_len       — bytes AFTER the header
//   ttl               — caller-chosen; typical 64
// Returns IPV4_HDR_LEN_MIN (20). Checksum is computed over the header and
// written in place.
size_t netd_ipv4_build_header(uint8_t *out_hdr,
                              uint32_t src, uint32_t dst,
                              uint8_t proto,
                              uint16_t id,
                              uint16_t payload_len,
                              uint8_t ttl);

// Parse an IPv4 datagram starting at `buf[0]` (L3 payload of an Ethernet
// frame). `buf_len` is the remaining bytes of the Ethernet payload. On
// success returns 0 and populates *out. Rejects:
//   - buf_len < 20
//   - version != 4
//   - IHL != 5 (options not supported)
//   - header checksum invalid
//   - total_len > buf_len
//   - fragmented packets (frag_offset != 0 OR MF flag set) — MVP only
//     handles atomic datagrams; DHCP/ARP/ICMP-echo/TCP/UDP all fit in one
int netd_ipv4_parse(const uint8_t *buf, size_t buf_len, ipv4_parsed_t *out);

// One's-complement Internet checksum (RFC 1071). `initial` chains input —
// pass 0 for a fresh run. Returns the value in BIG-ENDIAN ready for direct
// placement in a header field.
uint16_t netd_inet_checksum(const uint8_t *buf, size_t len, uint32_t initial);

// Build the 12-byte TCP/UDP pseudo-header in `out` given host-order src/dst
// IPs, protocol, and L4 length. Used by TCP and UDP checksum computation.
// Returns 12.
size_t netd_ipv4_build_pseudo_header(uint8_t *out,
                                     uint32_t src, uint32_t dst,
                                     uint8_t proto,
                                     uint16_t l4_len);

// Parse an IPv4 dotted string like "192.168.1.2" into host-byte-order
// uint32_t. Returns 0 on success, negative on malformed. Accepts decimal
// octets only; no partial / hex / short forms.
int netd_ipv4_parse_dotted(const char *str, uint32_t *out);

// ===========================================================================
// L4: ICMP (RFC 792), echo subset only.
// ===========================================================================

#define ICMP_TYPE_ECHO_REPLY   0u
#define ICMP_TYPE_ECHO_REQUEST 8u

// ICMP header (common to all type=0/8 variants for our purposes).
typedef struct __attribute__((packed)) icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;       // Over ICMP header + data (big-endian)
    uint16_t id;             // Big-endian — echo identifier
    uint16_t seq;            // Big-endian — echo sequence
} icmp_header_t;

_Static_assert(sizeof(icmp_header_t) == 8, "icmp_header_t layout drift");

// Build an ICMP echo packet (request or reply) at out[0..], including the
// ICMP header (8 bytes) + payload. payload_len bytes from `payload` are
// copied starting at out[8]. Returns 8 + payload_len. Checksum is computed
// and stored.
size_t netd_icmp_build_echo(uint8_t *out,
                            uint8_t type, uint8_t code,
                            uint16_t id, uint16_t seq,
                            const uint8_t *payload, size_t payload_len);

// Parse the leading 8 bytes of an ICMP PDU. `buf_len` is the whole ICMP
// PDU length (header + data). Verifies checksum; rejects if bad.
// On success returns 0 and populates *type/*code/*id/*seq; *payload +
// *payload_len point into `buf`.
int netd_icmp_parse_echo(const uint8_t *buf, size_t buf_len,
                         uint8_t *out_type, uint8_t *out_code,
                         uint16_t *out_id, uint16_t *out_seq,
                         const uint8_t **out_payload,
                         size_t *out_payload_len);

// Process an incoming ICMP datagram given the already-parsed IPv4 header.
// If it's an ECHO REQUEST, writes the full IP+ICMP reply datagram (20 + 8
// + payload bytes) into `reply_buf`, sets `*reply_len`, and returns 0.
// Any other type/code: returns 0 with *reply_len=0.
// Returns negative on malformed.
int netd_icmp_handle_incoming(const ipv4_parsed_t *ip,
                              uint32_t my_ip,
                              uint8_t *reply_buf,
                              size_t reply_buf_cap,
                              size_t *reply_len);

// ===========================================================================
// L4: UDP (RFC 768).
// ===========================================================================

#define UDP_HDR_LEN   8u

typedef struct __attribute__((packed)) udp_header {
    uint16_t src_port;       // Big-endian
    uint16_t dst_port;       // Big-endian
    uint16_t length;         // UDP header + payload (big-endian)
    uint16_t checksum;       // Big-endian; 0 == disabled (IPv4 only)
} udp_header_t;

_Static_assert(sizeof(udp_header_t) == 8, "udp_header_t layout drift");

// Build a UDP datagram (8 + payload bytes) at `out`. Src/dst IPs are used
// to compute the pseudo-header checksum; caller passes host order.
// Returns 8 + payload_len.
size_t netd_udp_build(uint8_t *out,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, size_t payload_len);

// Parse a UDP header (8 bytes) + validate pseudo-header checksum.
// Accepts checksum==0 as "no checksum" per RFC 768. On success returns 0,
// fills *src_port/*dst_port + *payload/*payload_len (pointing into buf).
int netd_udp_parse(const uint8_t *buf, size_t buf_len,
                   uint32_t src_ip, uint32_t dst_ip,
                   uint16_t *out_src_port, uint16_t *out_dst_port,
                   const uint8_t **out_payload, size_t *out_payload_len);

// ---------------------------------------------------------------------------
// UDP socket table (MVP — fixed 16 slots, bind-by-port).
// ---------------------------------------------------------------------------
// Phase 22 closeout (G3): UDP rarely needs the same scale as TCP, but bump
// proportionally so DHCP/DNS + 50+ concurrent UDP-bind callers fit cleanly.
#define UDP_MAX_SOCKETS    64u
#define UDP_STATE_FREE     0u
#define UDP_STATE_BOUND    1u

typedef struct udp_socket {
    uint8_t  state;           // UDP_STATE_*
    uint8_t  _pad[3];
    uint16_t local_port;      // Host byte order; 0 if FREE
    uint16_t _pad2;
    uint32_t owner_cookie;    // Caller-defined (client id / channel handle)
} udp_socket_t;

typedef struct udp_table {
    udp_socket_t sockets[UDP_MAX_SOCKETS];
} udp_table_t;

void netd_udp_table_init(udp_table_t *tbl);

// Bind `port` (host order). port==0 assigns ephemeral (48152..65535).
// Returns slot index on success, negative on full / in-use / bad port.
int netd_udp_bind(udp_table_t *tbl, uint16_t port, uint32_t owner_cookie);

// Release slot `idx`.
void netd_udp_close(udp_table_t *tbl, int idx);

// Look up an active BOUND socket by local port. Returns slot index on hit,
// -1 on miss.
int netd_udp_find(const udp_table_t *tbl, uint16_t local_port);

// ===========================================================================
// L4: TCP (RFC 793 + RFC 5681 Reno + RFC 5961 hardening).
// ===========================================================================

#define TCP_HDR_LEN_MIN   20u   // No options

// TCP control flags (6-bit CWR..FIN). Only the ones we use are named.
#define TCP_FLAG_FIN    0x01u
#define TCP_FLAG_SYN    0x02u
#define TCP_FLAG_RST    0x04u
#define TCP_FLAG_PSH    0x08u
#define TCP_FLAG_ACK    0x10u
#define TCP_FLAG_URG    0x20u

// TCP state machine per RFC 793 §3.2 figure 6.
#define TCP_STATE_CLOSED       0u
#define TCP_STATE_LISTEN       1u
#define TCP_STATE_SYN_SENT     2u
#define TCP_STATE_SYN_RCVD     3u
#define TCP_STATE_ESTABLISHED  4u
#define TCP_STATE_FIN_WAIT1    5u
#define TCP_STATE_FIN_WAIT2    6u
#define TCP_STATE_CLOSING      7u
#define TCP_STATE_CLOSE_WAIT   8u
#define TCP_STATE_LAST_ACK     9u
#define TCP_STATE_TIME_WAIT   10u

// Defaults.
#define TCP_DEFAULT_MSS        1460u    // 1500 - 20 IP - 20 TCP
#define TCP_DEFAULT_WINDOW     8192u
#define TCP_DEFAULT_RTO_MS     1000u
#define TCP_MAX_RTO_MS        60000u
#define TCP_MIN_RTO_MS          200u
#define TCP_MSL_MS            60000u    // Max segment lifetime
#define TCP_TIME_WAIT_MS   (2u * TCP_MSL_MS)   // 2*MSL = 120s

// Per-segment rx/tx metadata on the wire. Used by header build/parse.
typedef struct __attribute__((packed)) tcp_header {
    uint16_t src_port;        // Big-endian
    uint16_t dst_port;        // Big-endian
    uint32_t seq;             // Big-endian
    uint32_t ack;             // Big-endian
    uint8_t  data_off_flags_hi;  // High 4 bits = data_offset (in 32-bit words);
                                  // Low 4 bits = reserved + NS
    uint8_t  flags;           // CWR/ECE/URG/ACK/PSH/RST/SYN/FIN
    uint16_t window;          // Big-endian
    uint16_t checksum;        // Big-endian
    uint16_t urg_ptr;         // Big-endian
} tcp_header_t;

_Static_assert(sizeof(tcp_header_t) == 20, "tcp_header_t layout drift");

typedef struct tcp_parsed {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset_bytes;   // 20..60
    uint8_t  flags;
    uint16_t window;
    const uint8_t *payload;
    size_t   payload_len;
    // Parsed SYN options (only MSS honored in MVP). mss==0 when absent.
    uint16_t opt_mss;
} tcp_parsed_t;

// ---------------------------------------------------------------------------
// Socket descriptor.
// ---------------------------------------------------------------------------
// Phase 22 closeout (G3 / spec D12): scaled from 16 → 1024.  Memory budget:
// sizeof(tcp_socket_t) ~80 B × 1024 = 80 KiB for the TCB array, plus
// netd_tcp_conn_t ~2080 B × 1024 = 2 MiB for the per-socket userspace
// wrappers (with 2 KiB rx ring each).  Total ~2.1 MiB — well under the
// 64 MiB netd budget mandated by spec L1050.  Lookup walks the array
// linearly today (1024 elements × ~10 ns each = 10 µs/packet, comfortable
// for the 10K pps target).  Migrating to an open-addressed hash on
// (src_port, dst_ip, dst_port) is a perf-driven follow-up if needed.
#define TCP_MAX_SOCKETS     1024u

typedef struct tcp_socket {
    uint8_t  state;            // TCP_STATE_*
    uint8_t  _pad0[3];

    uint16_t local_port;       // Host byte order
    uint16_t remote_port;      // Host byte order (0 if LISTEN)
    uint32_t local_ip;         // Host byte order
    uint32_t remote_ip;        // Host byte order (0 if LISTEN)

    // Sequence space tracking (RFC 793 §3.2).
    uint32_t snd_una;          // Oldest unacknowledged sequence number
    uint32_t snd_nxt;          // Next sequence number to be sent
    uint32_t snd_wnd;          // Send window (bytes receiver will accept)
    uint32_t iss;              // Initial send sequence (own SYN)
    uint32_t rcv_nxt;          // Next expected sequence
    uint32_t rcv_wnd;          // Our advertised receive window
    uint32_t irs;              // Initial receive sequence (peer SYN)

    // Congestion control (RFC 5681 Reno).
    uint32_t cwnd;             // Congestion window, in bytes
    uint32_t ssthresh;         // Slow-start threshold
    uint16_t mss;              // Path MSS — min of advertised + TCP_DEFAULT_MSS
    uint8_t  dup_acks;         // Dup-ACK counter for fast-retransmit
    uint8_t  _pad1;

    // RTO / timers.
    uint32_t rto_ms;           // Current retransmission timeout
    uint64_t retx_expiry_tsc;  // rdtsc at which the oldest unacked seg expires
    uint64_t time_wait_expiry_tsc;  // TIME_WAIT deadline

    // Owner (client channel handle or similar).
    uint32_t owner_cookie;
    uint32_t _pad2;
} tcp_socket_t;

typedef struct tcp_table {
    tcp_socket_t sockets[TCP_MAX_SOCKETS];
} tcp_table_t;

// ---------------------------------------------------------------------------
// Wire helpers (pure functions).
// ---------------------------------------------------------------------------

// Build a TCP segment (no options unless mss_opt_val != 0 AND flags includes
// SYN, in which case an MSS option is emitted at offset [20..23]).
// Returns total segment length. src_ip/dst_ip are host-order for pseudo-header.
size_t netd_tcp_build(uint8_t *out,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t seq, uint32_t ack,
                      uint8_t flags, uint16_t window,
                      uint16_t mss_opt_val,
                      const uint8_t *payload, size_t payload_len);

// Parse a TCP segment. Verifies checksum (pseudo-header + segment). Extracts
// the MSS option if present and SYN is set. Rejects malformed options that
// run past data_offset.
int netd_tcp_parse(const uint8_t *buf, size_t buf_len,
                   uint32_t src_ip, uint32_t dst_ip,
                   tcp_parsed_t *out);

// ---------------------------------------------------------------------------
// Socket table operations.
// ---------------------------------------------------------------------------
void netd_tcp_table_init(tcp_table_t *tbl);

// Allocate a CLOSED socket slot. `owner_cookie` MUST be nonzero — slots
// with cookie == 0 are considered free. Returns index, or -1 if full.
int  netd_tcp_socket_alloc(tcp_table_t *tbl, uint32_t owner_cookie);

// Release a socket slot (zeroes it; no wire action).
void netd_tcp_socket_free(tcp_table_t *tbl, int idx);

// Look up a socket matching (local_ip, local_port, remote_ip, remote_port).
// When the socket is in LISTEN state, remote_* match 0 — so an incoming SYN
// whose remote is nonzero finds a LISTEN as a lower-priority fallback via
// netd_tcp_find_listen. Returns slot index on hit, -1 on miss.
int netd_tcp_find_established(const tcp_table_t *tbl,
                              uint32_t local_ip, uint16_t local_port,
                              uint32_t remote_ip, uint16_t remote_port);

int netd_tcp_find_listen(const tcp_table_t *tbl,
                         uint32_t local_ip, uint16_t local_port);

// ---------------------------------------------------------------------------
// State machine primitives.
// ---------------------------------------------------------------------------

// Outbound connect: transitions sock from CLOSED to SYN_SENT and writes a SYN
// segment into syn_buf (w/ MSS option). Caller must have already set
// sock->local_ip/local_port/remote_ip/remote_port + owner_cookie.
// `iss` should be a random-ish 32-bit value chosen by the caller.
int netd_tcp_connect(tcp_socket_t *sock,
                     uint32_t iss,
                     uint8_t *syn_buf, size_t syn_buf_cap,
                     size_t *syn_len,
                     uint32_t initial_rto_ms,
                     uint64_t now_tsc, uint64_t ticks_per_sec);

// Passive open: flip CLOSED to LISTEN. Remote ip/port set to 0.
int netd_tcp_listen(tcp_socket_t *sock, uint32_t local_ip, uint16_t local_port);

// Feed a parsed incoming TCP segment into this socket. Updates state,
// optionally emits one outbound segment into `resp_buf` (resp_len == 0 if no
// response). Returns 0 on success, negative on protocol violation (sends RST
// elsewhere).
//
// This is the core state-machine driver. On data delivery the caller can
// inspect sock->rcv_nxt advancement and read the payload from `pkt` directly.
int netd_tcp_on_segment(tcp_socket_t *sock,
                        const tcp_parsed_t *pkt,
                        uint64_t now_tsc, uint64_t ticks_per_sec,
                        uint8_t *resp_buf, size_t resp_buf_cap,
                        size_t *resp_len);

// Emit a FIN segment (caller: transitions ESTABLISHED→FIN_WAIT1 or
// CLOSE_WAIT→LAST_ACK). Writes the segment into `fin_buf`.
int netd_tcp_close(tcp_socket_t *sock,
                   uint8_t *fin_buf, size_t fin_buf_cap, size_t *fin_len);

// Periodic tick: reap TIME_WAIT sockets, retransmit if RTO has fired, kill
// sockets with RTO-exceeded retries. `retx_buf` is a per-socket scratch
// buffer used if retransmission is emitted; `retx_len` == 0 means no action.
int netd_tcp_tick(tcp_socket_t *sock,
                  uint64_t now_tsc, uint64_t ticks_per_sec,
                  uint8_t *retx_buf, size_t retx_buf_cap, size_t *retx_len);

// ===========================================================================
// L7: DNS resolver (RFC 1035) — pure wire helpers.
// ===========================================================================

#define DNS_MAX_NAME_BYTES    255u     // RFC 1035 §2.3.4
#define DNS_QTYPE_A             1u
#define DNS_QCLASS_IN           1u

#define DNS_FLAG_QR           0x8000u   // 1 = response
#define DNS_FLAG_RD           0x0100u   // recursion desired
#define DNS_FLAG_RA           0x0080u   // recursion available (in reply)
#define DNS_FLAG_RCODE_MASK   0x000Fu
#define DNS_RCODE_NOERROR        0u
#define DNS_RCODE_FORMERR        1u
#define DNS_RCODE_SERVFAIL       2u
#define DNS_RCODE_NXDOMAIN       3u
#define DNS_RCODE_NOTIMP         4u
#define DNS_RCODE_REFUSED        5u

#define DNS_MAX_ANSWERS   4u

typedef struct dns_query_result {
    uint32_t xid;                 // Transaction id (echoed from query)
    uint32_t ip_count;            // Number of A records parsed (0..DNS_MAX_ANSWERS)
    uint32_t ips[DNS_MAX_ANSWERS];// host-order IPv4 addresses
    uint32_t ttl_seconds;         // TTL of the first A record
    uint16_t rcode;               // DNS_RCODE_*
    uint16_t _pad;
} dns_query_result_t;

// Build a DNS A-record query for `hostname` into `out` (must have capacity
// for header + encoded name + 4 bytes of qtype/qclass). Returns total bytes
// written on success, negative on malformed hostname / insufficient space.
// `xid` is the 16-bit transaction id the caller will match against the
// response. `hostname_len` excludes any trailing NUL.
int netd_dns_build_query(uint8_t *out, size_t cap,
                         uint16_t xid,
                         const char *hostname, size_t hostname_len);

// Parse a DNS response buffer. Verifies QR=1, the echoed xid, the RCODE,
// and the first answer-section RR of type A + class IN. Up to DNS_MAX_ANSWERS
// A records are returned, in the order they appear. Returns 0 on success,
// populating *result.
//   - On NXDOMAIN / NOERROR-with-zero-answers: *result->ip_count==0 and
//     *result->rcode set appropriately; returns 0 (the caller interprets
//     "no address" via rcode + ip_count).
//   - On malformed / wrong xid / wrong QR: returns negative errno.
int netd_dns_parse_response(const uint8_t *buf, size_t len,
                            uint16_t expected_xid,
                            dns_query_result_t *result);

// ===========================================================================
// L7: DHCP client (RFC 2131).
// ===========================================================================

// DHCP message type option values.
#define DHCP_OP_BOOTREQUEST    1u
#define DHCP_OP_BOOTREPLY      2u
#define DHCP_HTYPE_ETH         1u
#define DHCP_HLEN_ETH          6u

#define DHCP_MAGIC             0x63825363u   // Big-endian on wire

#define DHCP_MT_DISCOVER       1u
#define DHCP_MT_OFFER          2u
#define DHCP_MT_REQUEST        3u
#define DHCP_MT_DECLINE        4u
#define DHCP_MT_ACK            5u
#define DHCP_MT_NAK            6u
#define DHCP_MT_RELEASE        7u

#define DHCP_OPT_PAD                   0u
#define DHCP_OPT_SUBNET_MASK           1u
#define DHCP_OPT_ROUTER                3u
#define DHCP_OPT_DNS_SERVER            6u
#define DHCP_OPT_REQUESTED_IP         50u
#define DHCP_OPT_LEASE_TIME           51u
#define DHCP_OPT_MSG_TYPE             53u
#define DHCP_OPT_SERVER_ID            54u
#define DHCP_OPT_PARAM_REQUEST_LIST   55u
#define DHCP_OPT_END                 255u

// DHCP client state machine.
#define DHCP_STATE_INIT          0u
#define DHCP_STATE_SELECTING     1u   // DISCOVER sent, waiting for OFFER
#define DHCP_STATE_REQUESTING    2u   // REQUEST sent, waiting for ACK/NAK
#define DHCP_STATE_BOUND         3u   // Have a lease
#define DHCP_STATE_RENEWING      4u   // Lease past T1 (50%)
#define DHCP_STATE_FAILED        5u   // Gave up after retries

typedef struct dhcp_client {
    uint8_t  state;
    uint8_t  _pad[3];
    uint32_t xid;              // Transaction id (big-endian on wire)
    uint8_t  mac[6];
    uint8_t  _pad2[2];

    uint32_t offered_ip;       // Host byte order; populated on OFFER
    uint32_t server_id;        // Host byte order
    uint32_t assigned_ip;      // Host byte order; populated on ACK
    uint32_t subnet_mask;      // Host byte order
    uint32_t router;           // Host byte order
    uint32_t dns;              // Host byte order (first server)
    uint32_t lease_seconds;

    uint64_t next_retry_tsc;   // For resend of DISCOVER / REQUEST
    uint32_t retry_backoff_ms; // Current back-off; doubles up to cap
    uint8_t  retry_count;
    uint8_t  _pad3[3];
} dhcp_client_t;

// Initialise state and pre-fill our MAC. Generates a random-ish xid from now_tsc.
void netd_dhcp_init(dhcp_client_t *c, const uint8_t my_mac[6], uint64_t now_tsc);

// Build a DHCPDISCOVER packet. Writes the UDP payload (DHCP message) into
// `out_payload`, returns its length. Caller wraps this in UDP/IP/Ethernet
// (src_ip=0.0.0.0, dst_ip=255.255.255.255, src_port=68, dst_port=67).
// Transitions state INIT → SELECTING.
size_t netd_dhcp_build_discover(dhcp_client_t *c,
                                uint8_t *out_payload, size_t cap);

// Build a DHCPREQUEST based on a received OFFER (state must be SELECTING).
// Transitions SELECTING → REQUESTING.
size_t netd_dhcp_build_request(dhcp_client_t *c,
                               uint8_t *out_payload, size_t cap);

// Parse a received DHCP packet. On success returns the message type
// (OFFER/ACK/NAK/...) and populates the relevant fields of *c. On malformed
// or wrong-xid returns 0 (ignored). The caller then advances the state
// machine: OFFER → build_request; ACK → BOUND.
int netd_dhcp_handle_incoming(dhcp_client_t *c,
                              const uint8_t *payload, size_t len);

// ===========================================================================
// Byte-level helpers used by multiple modules.
// ===========================================================================

// Copy `n` bytes from src to dst (no-overlap). Avoids dragging in the full
// libc memcpy in tests that want to link against netd modules alone.
static inline void netd_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static inline void netd_memzero(void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}

// Read/write be16/be32 from unaligned byte streams.
static inline uint16_t netd_read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static inline uint32_t netd_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static inline void netd_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void netd_write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
