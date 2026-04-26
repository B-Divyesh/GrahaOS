// user/libnet/libnet_msg.c — Phase 22 Stage C.
//
// Client helpers + dispatcher-side utilities for `/sys/net/service` messages.
// The design prioritises legibility over compactness: every helper is a
// straight-line request/response pair bracketed by pack → send → recv →
// verify-seq → unpack. No fancy retry policy — callers budget their own
// timeout_ns and bubble up errnos.

#include "libnet_msg.h"

#include <stdint.h>
#include <stddef.h>

#include "libnet.h"
#include "../syscalls.h"

extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

// 32-bit xorshift; seed via rdtsc so sequential helpers don't collide even
// if the caller doesn't seed `seq`.
static uint32_t libnet_rng_state = 0x13572468u;

static uint32_t libnet_rng_next(void) {
    uint32_t x = libnet_rng_state;
    if (x == 0) {
        uint64_t hi = 0, lo = 0;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        x = (uint32_t)(lo ^ (hi << 1));
        if (x == 0) x = 0x9e3779b9u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    libnet_rng_state = x ? x : 0x9e3779b9u;
    return libnet_rng_state;
}

// Fill the shared header. Caller passes the buffer cast to libnet_msg_header_t*.
static void libnet_msg_set_header(libnet_msg_header_t *h, uint16_t op,
                                   uint32_t seq) {
    h->op   = op;
    h->_pad = 0;
    h->seq  = seq;
}

int libnet_msg_unpack_header(const chan_msg_user_t *msg,
                             uint16_t *out_op, uint32_t *out_seq) {
    if (!msg || !out_op || !out_seq) return -5 /* -EINVAL */;
    if (msg->header.inline_len < sizeof(libnet_msg_header_t)) return -5;
    const libnet_msg_header_t *h =
        (const libnet_msg_header_t *)msg->inline_payload;
    *out_op  = h->op;
    *out_seq = h->seq;
    return 0;
}

uint16_t libnet_msg_build_err_response(void *buf, uint16_t cap,
                                       uint16_t resp_op, uint32_t req_seq,
                                       int32_t status) {
    if (cap < sizeof(libnet_msg_header_t) + sizeof(int32_t)) return 0;
    libnet_msg_header_t *h = (libnet_msg_header_t *)buf;
    libnet_msg_set_header(h, resp_op, req_seq);
    int32_t *st = (int32_t *)((uint8_t *)buf + sizeof(*h));
    *st = status;
    return (uint16_t)(sizeof(*h) + sizeof(int32_t));
}

int libnet_msg_send_recv(libnet_client_ctx_t *ctx,
                         const void *req_buf, uint16_t req_len,
                         void *resp_buf, uint16_t resp_cap,
                         uint16_t *out_resp_len,
                         uint64_t timeout_ns) {
    if (!ctx || !req_buf || !resp_buf) return -5;
    if (req_len < sizeof(libnet_msg_header_t)) return -5;

    const libnet_msg_header_t *req_hdr =
        (const libnet_msg_header_t *)req_buf;
    uint32_t expected_seq = req_hdr->seq;

    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = req_len;
    m.header.nhandles   = 0;
    // Channels on /sys/net/service are typed grahaos.net.socket.v1. The
    // kernel cap_object layer validates this at send time using the type
    // hash stamped into the channel at creation. Clients that want to pass
    // additional handles set them in `m.handles[]`; Stage C doesn't use
    // that path.
    memcpy(m.inline_payload, req_buf, req_len);

    long sc = syscall_chan_send(ctx->wr_req, &m, timeout_ns);
    if (sc < 0) return (int)sc;

    for (;;) {
        memset(&m, 0, sizeof(m));
        long rc = syscall_chan_recv(ctx->rd_resp, &m, timeout_ns);
        if (rc < 0) return (int)rc;
        if (m.header.inline_len < sizeof(libnet_msg_header_t)) continue;

        const libnet_msg_header_t *resp_hdr =
            (const libnet_msg_header_t *)m.inline_payload;
        if (resp_hdr->seq != expected_seq) {
            // Stale response from an earlier cancelled request — drop and
            // keep waiting. Belt-and-braces; Stage C never interleaves.
            continue;
        }
        if (m.header.inline_len > resp_cap) return -5;
        memcpy(resp_buf, m.inline_payload, m.header.inline_len);
        if (out_resp_len) *out_resp_len = m.header.inline_len;
        return 0;
    }
}

// --- High-level helpers -----------------------------------------------------

int libnet_hello(libnet_client_ctx_t *ctx, uint64_t timeout_ns,
                 libnet_hello_resp_t *out) {
    if (!ctx || !out) return -5;
    libnet_hello_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_HELLO_REQ, libnet_rng_next());

    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_HELLO_RESP) return -5;
    return out->status;
}

int libnet_net_query(libnet_client_ctx_t *ctx, uint32_t field,
                     uint64_t timeout_ns,
                     libnet_net_query_resp_t *out) {
    if (!ctx || !out) return -5;
    libnet_net_query_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_NET_QUERY_REQ, libnet_rng_next());
    req.field = field;

    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_NET_QUERY_RESP) return -5;
    return out->status;
}

int libnet_icmp_echo(libnet_client_ctx_t *ctx,
                     uint32_t dst_ip, uint16_t id, uint16_t seq,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t timeout_ms,
                     libnet_icmp_echo_resp_t *out) {
    if (!ctx || !out) return -5;
    if (payload_len > sizeof(((libnet_icmp_echo_req_t *)0)->payload)) return -5;

    libnet_icmp_echo_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_ICMP_ECHO_REQ, libnet_rng_next());
    req.dst_ip     = dst_ip;
    req.id         = id;
    req.seq        = seq;
    req.timeout_ms = timeout_ms;
    req.payload_len = payload_len;
    if (payload && payload_len) memcpy(req.payload, payload, payload_len);

    uint16_t resp_len = 0;
    uint64_t budget_ns = (uint64_t)(timeout_ms + 500u) * 1000000ULL;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, budget_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_ICMP_ECHO_RESP) return -5;
    return out->status;
}

int libnet_udp_bind(libnet_client_ctx_t *ctx, uint16_t local_port,
                    uint64_t timeout_ns,
                    libnet_udp_bind_resp_t *out) {
    if (!ctx || !out) return -5;
    libnet_udp_bind_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_UDP_BIND_REQ, libnet_rng_next());
    req.local_port = local_port;

    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_UDP_BIND_RESP) return -5;
    return out->status;
}

int libnet_udp_sendto(libnet_client_ctx_t *ctx, uint32_t cookie,
                      uint32_t dst_ip, uint16_t dst_port,
                      const uint8_t *payload, uint32_t payload_len,
                      uint64_t timeout_ns,
                      libnet_udp_sendto_resp_t *out) {
    if (!ctx || !out) return -5;
    if (payload_len > sizeof(((libnet_udp_sendto_req_t *)0)->payload))
        return -5;
    libnet_udp_sendto_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_UDP_SENDTO_REQ,
                          libnet_rng_next());
    req.cookie       = cookie;
    req.dst_ip       = dst_ip;
    req.dst_port     = dst_port;
    req.payload_len  = (uint16_t)payload_len;
    if (payload && payload_len) memcpy(req.payload, payload, payload_len);

    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_UDP_SENDTO_RESP) return -5;
    return out->status;
}

int libnet_tcp_open(libnet_client_ctx_t *ctx,
                    uint32_t dst_ip, uint16_t dst_port,
                    uint32_t timeout_ms,
                    uint32_t *out_cookie,
                    uint16_t *out_local_port) {
    if (!ctx) return -5;
    libnet_tcp_open_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_TCP_OPEN_REQ,
                          libnet_rng_next());
    req.dst_ip     = dst_ip;
    req.dst_port   = dst_port;
    req.flags      = 0;
    req.timeout_ms = timeout_ms;

    libnet_tcp_open_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    uint16_t resp_len = 0;
    uint64_t budget_ns =
        ((uint64_t)(timeout_ms ? timeout_ms : 10000u) + 1000u) * 1000000ULL;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req),
                                  &resp, sizeof(resp),
                                  &resp_len, budget_ns);
    if (rc < 0) return rc;
    if (resp.hdr.op != LIBNET_OP_TCP_OPEN_RESP) return -5;
    if (resp.status < 0) return resp.status;
    if (out_cookie) *out_cookie = resp.socket_cookie;
    if (out_local_port) *out_local_port = resp.local_port;
    return 0;
}

int libnet_tcp_send(libnet_client_ctx_t *ctx, uint32_t cookie,
                    const uint8_t *payload, uint16_t payload_len,
                    uint64_t timeout_ns,
                    uint32_t *out_bytes_sent) {
    if (!ctx || (!payload && payload_len)) return -5;
    if (payload_len > LIBNET_TCP_CHUNK_MAX) return -5;

    libnet_tcp_send_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_TCP_SEND_REQ,
                          libnet_rng_next());
    req.socket_cookie = cookie;
    req.payload_len   = payload_len;
    req.flags         = 0;
    if (payload_len) memcpy(req.payload, payload, payload_len);

    // The payload trailer may be unused — send only the populated prefix.
    uint16_t req_bytes = (uint16_t)(sizeof(libnet_msg_header_t) +
                                    sizeof(uint32_t) +
                                    sizeof(uint16_t) * 2 +
                                    payload_len);

    libnet_tcp_send_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, req_bytes,
                                  &resp, sizeof(resp),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (resp.hdr.op != LIBNET_OP_TCP_SEND_RESP) return -5;
    if (resp.status < 0) return resp.status;
    if (out_bytes_sent) *out_bytes_sent = resp.bytes_sent;
    return 0;
}

int libnet_tcp_recv(libnet_client_ctx_t *ctx, uint32_t cookie,
                    uint8_t *buf, uint16_t buf_cap,
                    uint32_t timeout_ms,
                    uint16_t *out_payload_len,
                    uint16_t *out_flags) {
    if (!ctx || (!buf && buf_cap)) return -5;
    uint16_t cap = buf_cap;
    if (cap > LIBNET_TCP_CHUNK_MAX) cap = LIBNET_TCP_CHUNK_MAX;

    libnet_tcp_recv_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_TCP_RECV_REQ,
                          libnet_rng_next());
    req.socket_cookie = cookie;
    req.max_bytes     = cap;
    req.timeout_ms    = timeout_ms;

    libnet_tcp_recv_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    uint16_t resp_len = 0;
    uint64_t budget_ns = ((uint64_t)(timeout_ms + 1000u)) * 1000000ULL;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req),
                                  &resp, sizeof(resp),
                                  &resp_len, budget_ns);
    if (rc < 0) return rc;
    if (resp.hdr.op != LIBNET_OP_TCP_RECV_RESP) return -5;
    if (resp.status < 0) {
        if (out_payload_len) *out_payload_len = 0;
        if (out_flags) *out_flags = resp.flags;
        return resp.status;
    }
    uint16_t n = resp.payload_len;
    if (n > cap) n = cap;
    if (n) memcpy(buf, resp.payload, n);
    if (out_payload_len) *out_payload_len = n;
    if (out_flags) *out_flags = resp.flags;
    return 0;
}

int libnet_tcp_close(libnet_client_ctx_t *ctx, uint32_t cookie,
                     uint64_t timeout_ns) {
    if (!ctx) return -5;
    libnet_tcp_close_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_TCP_CLOSE_REQ,
                          libnet_rng_next());
    req.socket_cookie = cookie;
    req.flags         = 0;

    libnet_tcp_close_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req),
                                  &resp, sizeof(resp),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (resp.hdr.op != LIBNET_OP_TCP_CLOSE_RESP) return -5;
    return resp.status;
}

int libnet_tcp_status(libnet_client_ctx_t *ctx, uint32_t cookie,
                      uint64_t timeout_ns,
                      libnet_tcp_status_resp_t *out) {
    if (!ctx || !out) return -5;
    libnet_tcp_status_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_TCP_STATUS_REQ,
                          libnet_rng_next());
    req.socket_cookie = cookie;

    uint16_t resp_len = 0;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req),
                                  out, sizeof(*out),
                                  &resp_len, timeout_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_TCP_STATUS_RESP) return -5;
    return out->status;
}

int libnet_dns_resolve(libnet_client_ctx_t *ctx, const char *hostname,
                       uint32_t timeout_ms,
                       libnet_dns_query_resp_t *out) {
    if (!ctx || !hostname || !out) return -5;
    libnet_dns_query_req_t req;
    memset(&req, 0, sizeof(req));
    libnet_msg_set_header(&req.hdr, LIBNET_OP_DNS_QUERY_REQ,
                          libnet_rng_next());
    uint32_t nlen = 0;
    while (hostname[nlen] && nlen < LIBNET_DNS_MAX_NAME) nlen++;
    if (hostname[nlen] != '\0') return -5;   // name too long
    if (nlen == 0) return -5;
    req.timeout_ms = timeout_ms;
    req.qtype      = 1u;
    req.name_len   = (uint8_t)nlen;
    memcpy(req.name, hostname, nlen);

    uint16_t resp_len = 0;
    uint64_t budget_ns = (uint64_t)(timeout_ms + 500u) * 1000000ULL;
    int rc = libnet_msg_send_recv(ctx, &req, sizeof(req), out, sizeof(*out),
                                  &resp_len, budget_ns);
    if (rc < 0) return rc;
    if (out->hdr.op != LIBNET_OP_DNS_QUERY_RESP) return -5;
    return out->status;
}
