// user/libnet/libnet_msg.h — Phase 22 Stage C.
//
// On-wire schema for messages exchanged over `/sys/net/service`. Each
// per-connection channel pair (created by SYS_CHAN_CONNECT via the rawnet
// registry) carries `grahaos.net.socket.v1` messages. Both directions use
// the same compact op-code enum; the first byte of the inline payload
// identifies which struct follows, and the channel message header's
// `inline_len` carries the total body length.
//
// Layout of every message:
//
//     inline_payload[0]        = libnet_msg_op_t
//     inline_payload[1..3]     = reserved / padding (zero)
//     inline_payload[4..7]     = libnet_msg_seq_t   — per-request id the
//                                client chose; netd echoes it in the reply
//                                so parallel in-flight requests don't
//                                collide even when one takes longer.
//     inline_payload[8..]      = the request or response body for the op.
//
// Why not an enum class + tagged union? Because we want to keep the library
// usable from C userspace (no C++), avoid pulling in libc sizeof-dance, and
// let the wire schema be fully knowable from a single source file. Future
// Phase 23 additions (storaged) can model their own libstorage_msg.h after
// this file.
//
// All integer fields are **host byte order** (little-endian on x86_64).
// The /sys/net/service channel is *intra-host*; we only use network byte
// order where it bubbles up onto the physical wire inside netd.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "libnet.h"
#include "../syscalls.h"

// ---------------------------------------------------------------------------
// Op-codes. Kept in a compact uint16_t — gives us room to grow well past
// the Phase 22 set without touching alignment. Paired request/response op-
// codes share the low byte except for the high bit (bit 15 set = response),
// so `is_response(op) = (op & 0x8000u) != 0`. Convention is enforced below.
// ---------------------------------------------------------------------------
#define LIBNET_OP_MASK_RESP        0x8000u

// Ping-style liveness check. Reply echoes the same msg_seq + server_pid.
#define LIBNET_OP_HELLO_REQ        0x0001u
#define LIBNET_OP_HELLO_RESP       0x8001u

// Query network state (field selects which bundle is returned).
#define LIBNET_OP_NET_QUERY_REQ    0x0010u
#define LIBNET_OP_NET_QUERY_RESP   0x8010u

// ICMP echo. Fire-and-wait; the server drives ARP + IPv4 + ICMP framing.
#define LIBNET_OP_ICMP_ECHO_REQ    0x0020u
#define LIBNET_OP_ICMP_ECHO_RESP   0x8020u

// UDP bind + sendto. The server holds the socket state; the client just
// sees a uint32_t cookie. Sendto is single-shot; there is no async recv
// path in Stage C.
#define LIBNET_OP_UDP_BIND_REQ     0x0030u
#define LIBNET_OP_UDP_BIND_RESP    0x8030u
#define LIBNET_OP_UDP_SENDTO_REQ   0x0031u
#define LIBNET_OP_UDP_SENDTO_RESP  0x8031u

// DNS resolver (Stage C U16).
#define LIBNET_OP_DNS_QUERY_REQ    0x0040u
#define LIBNET_OP_DNS_QUERY_RESP   0x8040u

// TCP open — Stage C stubbed; Stage D wires the full path. The server does
// not reply synchronously: TCP_OPEN_RESP is emitted once the three-way
// handshake completes (ESTABLISHED) or the connect fails / times out.
// Subsequent data uses TCP_SEND/TCP_RECV keyed on `socket_cookie`.
#define LIBNET_OP_TCP_OPEN_REQ     0x0050u
#define LIBNET_OP_TCP_OPEN_RESP    0x8050u
#define LIBNET_OP_TCP_CLOSE_REQ    0x0051u
#define LIBNET_OP_TCP_CLOSE_RESP   0x8051u
#define LIBNET_OP_TCP_SEND_REQ     0x0052u
#define LIBNET_OP_TCP_SEND_RESP    0x8052u
#define LIBNET_OP_TCP_RECV_REQ     0x0053u
#define LIBNET_OP_TCP_RECV_RESP    0x8053u
#define LIBNET_OP_TCP_STATUS_REQ   0x0054u
#define LIBNET_OP_TCP_STATUS_RESP  0x8054u

// ---------------------------------------------------------------------------
// Shared preamble. Every message starts with this; `op` + 6 bytes of
// padding + seq = 8 bytes so the body starts 8-byte-aligned.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) libnet_msg_header {
    uint16_t op;                // LIBNET_OP_*
    uint16_t _pad;              // Zero on wire
    uint32_t seq;               // Client-assigned request id (echoed in resp)
} libnet_msg_header_t;

_Static_assert(sizeof(libnet_msg_header_t) == 8, "libnet_msg_header layout drift");

// ---------------------------------------------------------------------------
// NET_QUERY.
// ---------------------------------------------------------------------------
#define LIBNET_NET_QUERY_FIELD_CONFIG   1u    // IP / netmask / GW / MAC
#define LIBNET_NET_QUERY_FIELD_STATUS   2u    // Counters + running flag
#define LIBNET_NET_QUERY_FIELD_ALL      3u    // Both bundles

typedef struct __attribute__((packed)) libnet_net_query_req {
    libnet_msg_header_t hdr;    // op = LIBNET_OP_NET_QUERY_REQ
    uint32_t field;             // LIBNET_NET_QUERY_FIELD_*
} libnet_net_query_req_t;

typedef struct __attribute__((packed)) libnet_net_query_resp {
    libnet_msg_header_t hdr;    // op = LIBNET_OP_NET_QUERY_RESP
    int32_t  status;            // 0 or negative errno
    uint32_t field;             // Echo of req->field
    uint32_t ip;                // host-order
    uint32_t netmask;           // host-order
    uint32_t gateway;           // host-order
    uint32_t dns;               // host-order
    uint8_t  mac[6];
    uint8_t  link_up;           // 0/1
    uint8_t  stack_running;     // 0/1 (1 once DHCP ACK received or static)
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t arp_entries_count;
    uint32_t tcp_sockets_count;
    uint32_t udp_sockets_count;
    uint32_t _reserved;
} libnet_net_query_resp_t;

_Static_assert(sizeof(libnet_net_query_resp_t) <= CHAN_MSG_INLINE_MAX,
               "net_query_resp bigger than inline payload capacity");

// ---------------------------------------------------------------------------
// ICMP_ECHO.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) libnet_icmp_echo_req {
    libnet_msg_header_t hdr;
    uint32_t dst_ip;            // host-order
    uint16_t id;                // ICMP identifier (client-chosen)
    uint16_t seq;               // ICMP sequence
    uint32_t timeout_ms;        // Max wait for echo reply
    uint32_t payload_len;       // 0..64 bytes; echoed in payload[]
    uint8_t  payload[64];
} libnet_icmp_echo_req_t;

typedef struct __attribute__((packed)) libnet_icmp_echo_resp {
    libnet_msg_header_t hdr;
    int32_t  status;            // 0, -ETIMEDOUT, -EHOSTUNREACH, ...
    uint32_t src_ip;
    uint16_t id;
    uint16_t seq;
    uint64_t rtt_ns;
    uint32_t payload_len;
    uint8_t  payload[64];
} libnet_icmp_echo_resp_t;

// ---------------------------------------------------------------------------
// UDP_BIND / UDP_SENDTO.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) libnet_udp_bind_req {
    libnet_msg_header_t hdr;
    uint16_t local_port;        // host-order; 0 = ephemeral
    uint16_t _pad;
} libnet_udp_bind_req_t;

typedef struct __attribute__((packed)) libnet_udp_bind_resp {
    libnet_msg_header_t hdr;
    int32_t  status;
    uint32_t cookie;            // Opaque handle for subsequent sendto
    uint16_t local_port;        // Actual bound port (resolved if ephemeral)
    uint16_t _pad;
} libnet_udp_bind_resp_t;

typedef struct __attribute__((packed)) libnet_udp_sendto_req {
    libnet_msg_header_t hdr;
    uint32_t cookie;            // From prior bind
    uint32_t dst_ip;            // host-order
    uint16_t dst_port;
    uint16_t payload_len;       // 0..200; larger payloads deferred
    uint8_t  payload[200];
} libnet_udp_sendto_req_t;

typedef struct __attribute__((packed)) libnet_udp_sendto_resp {
    libnet_msg_header_t hdr;
    int32_t  status;
    uint32_t bytes_sent;
} libnet_udp_sendto_resp_t;

// ---------------------------------------------------------------------------
// DNS_QUERY.
// ---------------------------------------------------------------------------
#define LIBNET_DNS_MAX_NAME    63u   // Not full 253 — MVP fits in inline payload
#define LIBNET_DNS_MAX_ANSWERS 4u    // Return at most 4 A records

typedef struct __attribute__((packed)) libnet_dns_query_req {
    libnet_msg_header_t hdr;
    uint32_t timeout_ms;        // 0 = netd default (5000 ms)
    uint16_t qtype;             // 1 = A (others deferred to Stage D)
    uint8_t  name_len;          // 1..LIBNET_DNS_MAX_NAME
    uint8_t  _pad;
    uint8_t  name[64];          // ASCII, NUL-terminated if shorter than 64
} libnet_dns_query_req_t;

typedef struct __attribute__((packed)) libnet_dns_query_resp {
    libnet_msg_header_t hdr;
    int32_t  status;            // 0, -ENOENT, -ETIMEDOUT, -EPROTO, ...
    uint32_t answer_count;      // 0..LIBNET_DNS_MAX_ANSWERS
    uint32_t answers[LIBNET_DNS_MAX_ANSWERS];   // host-order IPv4
    uint32_t ttl_seconds;       // TTL of the first answer
} libnet_dns_query_resp_t;

// ---------------------------------------------------------------------------
// TCP lifecycle (Stage D). The `socket_cookie` returned by TCP_OPEN_RESP is a
// stable per-socket identifier — the client presents it for SEND/RECV/CLOSE/
// STATUS. Netd's pending-open state matches cookies by the req_seq of
// TCP_OPEN_REQ, so callers must serialise opens per channel.
//
// Data chunks are carried inline in CHAN_MSG_INLINE_MAX messages. With a 16-
// byte envelope (header + cookie + length fields) the usable payload per
// message is 200 bytes; libhttp fragments larger requests/responses across
// multiple SEND/RECV round-trips.
// ---------------------------------------------------------------------------
#define LIBNET_TCP_CHUNK_MAX     200u

// TCP open flags.
#define LIBNET_TCP_OPEN_FLAG_NONE     0u

typedef struct __attribute__((packed)) libnet_tcp_open_req {
    libnet_msg_header_t hdr;
    uint32_t dst_ip;        // host-order IPv4
    uint16_t dst_port;      // host-order
    uint16_t flags;         // reserved / LIBNET_TCP_OPEN_FLAG_*
    uint32_t timeout_ms;    // 0 = netd default (10 s)
} libnet_tcp_open_req_t;

typedef struct __attribute__((packed)) libnet_tcp_open_resp {
    libnet_msg_header_t hdr;
    int32_t  status;        // 0 = ESTABLISHED; negative errno otherwise
    uint32_t socket_cookie; // Opaque per-socket id
    uint16_t local_port;    // Ephemeral local port chosen by netd
    uint16_t _pad;
} libnet_tcp_open_resp_t;

_Static_assert(sizeof(libnet_tcp_open_req_t)  <= CHAN_MSG_INLINE_MAX,
               "tcp_open_req too big");
_Static_assert(sizeof(libnet_tcp_open_resp_t) <= CHAN_MSG_INLINE_MAX,
               "tcp_open_resp too big");

typedef struct __attribute__((packed)) libnet_tcp_close_req {
    libnet_msg_header_t hdr;
    uint32_t socket_cookie;
    uint32_t flags;             // reserved; 0 = graceful FIN, 1 = abort RST
} libnet_tcp_close_req_t;

typedef struct __attribute__((packed)) libnet_tcp_close_resp {
    libnet_msg_header_t hdr;
    int32_t status;
    uint32_t _pad;
} libnet_tcp_close_resp_t;

typedef struct __attribute__((packed)) libnet_tcp_send_req {
    libnet_msg_header_t hdr;
    uint32_t socket_cookie;
    uint16_t payload_len;       // 0..LIBNET_TCP_CHUNK_MAX
    uint16_t flags;             // bit0 = PSH
    uint8_t  payload[LIBNET_TCP_CHUNK_MAX];
} libnet_tcp_send_req_t;

typedef struct __attribute__((packed)) libnet_tcp_send_resp {
    libnet_msg_header_t hdr;
    int32_t  status;
    uint32_t bytes_sent;
} libnet_tcp_send_resp_t;

_Static_assert(sizeof(libnet_tcp_send_req_t) <= CHAN_MSG_INLINE_MAX,
               "tcp_send_req too big");

typedef struct __attribute__((packed)) libnet_tcp_recv_req {
    libnet_msg_header_t hdr;
    uint32_t socket_cookie;
    uint16_t max_bytes;         // Caller's ceiling; 0 = server default
    uint16_t _pad;
    uint32_t timeout_ms;        // 0 = return immediately (EAGAIN if no data)
} libnet_tcp_recv_req_t;

typedef struct __attribute__((packed)) libnet_tcp_recv_resp {
    libnet_msg_header_t hdr;
    int32_t  status;            // 0 = data delivered, -EPIPE on peer FIN,
                                // -EAGAIN on no-data-and-timeout-0,
                                // -ETIMEDOUT after timeout_ms elapsed
    uint16_t payload_len;       // 0..LIBNET_TCP_CHUNK_MAX
    uint16_t flags;             // bit0 = peer has FIN'd
    uint8_t  payload[LIBNET_TCP_CHUNK_MAX];
} libnet_tcp_recv_resp_t;

_Static_assert(sizeof(libnet_tcp_recv_resp_t) <= CHAN_MSG_INLINE_MAX,
               "tcp_recv_resp too big");

typedef struct __attribute__((packed)) libnet_tcp_status_req {
    libnet_msg_header_t hdr;
    uint32_t socket_cookie;
} libnet_tcp_status_req_t;

typedef struct __attribute__((packed)) libnet_tcp_status_resp {
    libnet_msg_header_t hdr;
    int32_t  status;
    uint8_t  state;             // TCP_STATE_* (netd.h)
    uint8_t  peer_fin;
    uint16_t _pad;
    uint32_t rx_buffered;       // Bytes sitting in the netd rx ring
    uint32_t tx_pending;        // Bytes not yet ACKed by peer
} libnet_tcp_status_resp_t;

// ---------------------------------------------------------------------------
// HELLO — liveness handshake, useful for tests that only want to know the
// dispatcher is alive.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) libnet_hello_req {
    libnet_msg_header_t hdr;
} libnet_hello_req_t;

typedef struct __attribute__((packed)) libnet_hello_resp {
    libnet_msg_header_t hdr;
    int32_t  status;
    int32_t  server_pid;
    uint64_t server_tsc;
    uint32_t protocol_version;  // 1 for Phase 22
    uint32_t _pad;
} libnet_hello_resp_t;

// ---------------------------------------------------------------------------
// High-level client helpers. Each opens a /sys/net/service connection (or
// reuses the one passed in), sends the request, waits for the response
// with matching seq, unpacks into the caller's struct. Returns 0 on
// success; negative errno otherwise.
//
// `timeout_ns` is the end-to-end budget across connect + round-trip.
// Callers that want separate budgets should use the lower-level send/recv
// helpers below.
// ---------------------------------------------------------------------------
int libnet_hello(libnet_client_ctx_t *ctx, uint64_t timeout_ns,
                 libnet_hello_resp_t *out);

int libnet_net_query(libnet_client_ctx_t *ctx, uint32_t field,
                     uint64_t timeout_ns,
                     libnet_net_query_resp_t *out);

int libnet_icmp_echo(libnet_client_ctx_t *ctx,
                     uint32_t dst_ip, uint16_t id, uint16_t seq,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t timeout_ms,
                     libnet_icmp_echo_resp_t *out);

int libnet_udp_bind(libnet_client_ctx_t *ctx, uint16_t local_port,
                    uint64_t timeout_ns,
                    libnet_udp_bind_resp_t *out);

int libnet_udp_sendto(libnet_client_ctx_t *ctx, uint32_t cookie,
                      uint32_t dst_ip, uint16_t dst_port,
                      const uint8_t *payload, uint32_t payload_len,
                      uint64_t timeout_ns,
                      libnet_udp_sendto_resp_t *out);

int libnet_dns_resolve(libnet_client_ctx_t *ctx, const char *hostname,
                       uint32_t timeout_ms,
                       libnet_dns_query_resp_t *out);

// TCP lifecycle helpers (Stage D).
//   libnet_tcp_open : blocking until ESTABLISHED or error; populates
//                     *out_cookie + *out_local_port.
//   libnet_tcp_send : copies up to LIBNET_TCP_CHUNK_MAX bytes into a TCP
//                     segment and emits it. Returns bytes_sent on success.
//   libnet_tcp_recv : blocks up to timeout_ms for data (status == 0 with
//                     payload), -EAGAIN on no data + timeout_ms==0, -EPIPE
//                     when the peer has FIN'd and no more data is buffered.
//   libnet_tcp_close: sends FIN. Returns once FIN is emitted (does NOT wait
//                     for the peer's final ACK — use libnet_tcp_status to
//                     poll state).
int libnet_tcp_open(libnet_client_ctx_t *ctx,
                    uint32_t dst_ip, uint16_t dst_port,
                    uint32_t timeout_ms,
                    uint32_t *out_cookie,
                    uint16_t *out_local_port);

int libnet_tcp_send(libnet_client_ctx_t *ctx, uint32_t cookie,
                    const uint8_t *payload, uint16_t payload_len,
                    uint64_t timeout_ns,
                    uint32_t *out_bytes_sent);

int libnet_tcp_recv(libnet_client_ctx_t *ctx, uint32_t cookie,
                    uint8_t *buf, uint16_t buf_cap,
                    uint32_t timeout_ms,
                    uint16_t *out_payload_len,
                    uint16_t *out_flags);

int libnet_tcp_close(libnet_client_ctx_t *ctx, uint32_t cookie,
                     uint64_t timeout_ns);

int libnet_tcp_status(libnet_client_ctx_t *ctx, uint32_t cookie,
                      uint64_t timeout_ns,
                      libnet_tcp_status_resp_t *out);

// Low-level send+recv pair for clients that want custom handling (e.g. the
// dispatcher tests). Both operate against an already-connected ctx.
int libnet_msg_send_recv(libnet_client_ctx_t *ctx,
                         const void *req_buf, uint16_t req_len,
                         void *resp_buf, uint16_t resp_cap,
                         uint16_t *out_resp_len,
                         uint64_t timeout_ns);

// ---------------------------------------------------------------------------
// Dispatcher-side helpers. Used by netd but exposed for unit tests.
// Validates that the inline payload starts with a libnet_msg_header_t and
// returns (op, seq) pairs. On failure returns negative errno without
// modifying the outputs.
// ---------------------------------------------------------------------------
int libnet_msg_unpack_header(const chan_msg_user_t *msg,
                             uint16_t *out_op, uint32_t *out_seq);

// Build a generic "error" response: shared header + int32_t status. Used
// when the request's op is recognised but the payload is malformed.
// Returns the total inline payload length.
uint16_t libnet_msg_build_err_response(void *buf, uint16_t cap,
                                       uint16_t resp_op, uint32_t req_seq,
                                       int32_t status);
