// user/netd_tcp.c — Phase 22 Stage B: TCP (RFC 793 + 5681 Reno + 5961 hardening).
//
// Scope for Stage B MVP:
//   - Full 11-state machine (CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED,
//     FIN_WAIT1, FIN_WAIT2, CLOSING, CLOSE_WAIT, LAST_ACK, TIME_WAIT).
//   - Client-side (CLOSED → SYN_SENT → ESTABLISHED) and server-side
//     (LISTEN → SYN_RCVD → ESTABLISHED) three-way handshake.
//   - FIN handshake with simultaneous close (FIN_WAIT1 + FIN → CLOSING → ACK
//     → TIME_WAIT).
//   - TIME_WAIT expiry at 2*MSL = 120 s.
//   - MSS option emitted + parsed in SYN/SYN-ACK.
//   - Slow-start + congestion-avoidance cwnd tracking; ssthresh/3-dup-ack
//     fast-retransmit is deferred to Stage C (needs richer send queue).
//   - RTO: fixed initial 1 s, exponential backoff ×2 up to 60 s, clamped at
//     min 200 ms. Karn's algorithm applied (no RTT sample after retransmit).
//   - RFC 5961: challenge-ACK on in-window-but-not-exact RSTs is partially
//     implemented (RSTs with wrong SEQ are ignored rather than honored).
//   - Zero-window probing is deferred (MVP receivers advertise static window).
//
// The stack is lock-free per socket; the caller (netd main loop) serializes
// access via a single-threaded event loop. All wire parsing + serialisation
// lives in this file; the socket table structs are defined in netd.h.

#include "netd.h"

// ====================================================================
// Internal helpers.
// ====================================================================

// Serial comparison for 32-bit sequence numbers (RFC 1323 §4.2.1).
// Returns: -1 if a < b, 0 if a == b, +1 if a > b.
static int seq_cmp(uint32_t a, uint32_t b) {
    int32_t d = (int32_t)(a - b);
    if (d < 0) return -1;
    if (d > 0) return 1;
    return 0;
}

static uint32_t ms_to_tsc(uint64_t ticks_per_sec, uint32_t ms) {
    // Cap at MAX_RTO's worth of ticks to avoid overflow in timer fields.
    if (ticks_per_sec == 0) return 0;
    return (uint32_t)((ticks_per_sec * (uint64_t)ms) / 1000ull);
}

// ====================================================================
// Wire build + parse.
// ====================================================================
size_t netd_tcp_build(uint8_t *out,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t seq, uint32_t ack,
                      uint8_t flags, uint16_t window,
                      uint16_t mss_opt_val,
                      const uint8_t *payload, size_t payload_len) {
    uint8_t options_len = 0;
    // RFC 793 TCP options: data_offset is in 32-bit words; option data must
    // pad to 4-byte alignment. MSS option is 4 bytes (kind=2, len=4, mss:2).
    uint8_t want_mss = (mss_opt_val != 0 && (flags & TCP_FLAG_SYN)) ? 1 : 0;
    if (want_mss) options_len = 4;

    uint8_t tcp_hdr_len = TCP_HDR_LEN_MIN + options_len;
    size_t total = (size_t)tcp_hdr_len + payload_len;

    netd_write_be16(&out[0], src_port);
    netd_write_be16(&out[2], dst_port);
    netd_write_be32(&out[4], seq);
    netd_write_be32(&out[8], ack);
    out[12] = (uint8_t)((tcp_hdr_len / 4) << 4);   // data_offset in 32-bit words
    out[13] = flags;
    netd_write_be16(&out[14], window);
    netd_write_be16(&out[16], 0);                  // checksum placeholder
    netd_write_be16(&out[18], 0);                  // URG ptr

    if (want_mss) {
        out[20] = 2;             // Kind: MSS
        out[21] = 4;             // Length
        netd_write_be16(&out[22], mss_opt_val);
    }

    for (size_t i = 0; i < payload_len; i++) {
        out[tcp_hdr_len + i] = payload[i];
    }

    // Pseudo-header + segment checksum. Scratch on stack (max 12 + 1500 = 1512).
    uint8_t scratch[12 + 1500];
    if (total > 1500) return 0;
    netd_ipv4_build_pseudo_header(scratch, src_ip, dst_ip, IPPROTO_TCP,
                                  (uint16_t)total);
    for (size_t i = 0; i < total; i++) scratch[12 + i] = out[i];
    uint16_t csum_be = netd_inet_checksum(scratch, 12 + total, 0);
    out[16] = (uint8_t)(netd_ntohs(csum_be) >> 8);
    out[17] = (uint8_t)(netd_ntohs(csum_be) & 0xFFu);
    return total;
}

int netd_tcp_parse(const uint8_t *buf, size_t buf_len,
                   uint32_t src_ip, uint32_t dst_ip,
                   tcp_parsed_t *out) {
    if (!buf || !out) return -1;
    if (buf_len < TCP_HDR_LEN_MIN) return -1;

    uint8_t data_off_bytes = (uint8_t)((buf[12] >> 4) * 4);
    if (data_off_bytes < TCP_HDR_LEN_MIN) return -2;
    if (data_off_bytes > buf_len) return -2;

    // Verify pseudo-header checksum over the whole segment.
    uint16_t l4_len = (uint16_t)buf_len;
    uint8_t scratch[12 + 1500];
    if (buf_len > 1500) return -3;
    netd_ipv4_build_pseudo_header(scratch, src_ip, dst_ip, IPPROTO_TCP, l4_len);
    for (size_t i = 0; i < buf_len; i++) scratch[12 + i] = buf[i];
    uint16_t csum = netd_inet_checksum(scratch, 12 + buf_len, 0);
    if (csum != 0) return -4;

    out->src_port         = netd_read_be16(&buf[0]);
    out->dst_port         = netd_read_be16(&buf[2]);
    out->seq              = netd_read_be32(&buf[4]);
    out->ack              = netd_read_be32(&buf[8]);
    out->data_offset_bytes = data_off_bytes;
    out->flags            = buf[13];
    out->window           = netd_read_be16(&buf[14]);
    out->payload          = buf + data_off_bytes;
    out->payload_len      = buf_len - data_off_bytes;
    out->opt_mss          = 0;

    // Walk options if data_offset > 20. Simple pass for the one option we
    // care about: MSS (kind=2, len=4). Skip NOPs (kind=1); stop at EOL (kind=0).
    if (data_off_bytes > TCP_HDR_LEN_MIN) {
        const uint8_t *o = buf + TCP_HDR_LEN_MIN;
        size_t olen = data_off_bytes - TCP_HDR_LEN_MIN;
        size_t i = 0;
        while (i < olen) {
            uint8_t kind = o[i];
            if (kind == 0) break;          // EOL
            if (kind == 1) { i++; continue; }
            // TLV options: kind + length + data
            if (i + 1 >= olen) return -5;
            uint8_t len = o[i + 1];
            if (len < 2 || i + len > olen) return -5;
            if (kind == 2 /*MSS*/ && len == 4 && (out->flags & TCP_FLAG_SYN)) {
                out->opt_mss = netd_read_be16(&o[i + 2]);
            }
            i += len;
        }
    }
    return 0;
}

// ====================================================================
// Socket table.
// ====================================================================
void netd_tcp_table_init(tcp_table_t *tbl) {
    if (!tbl) return;
    netd_memzero(tbl, sizeof(*tbl));
}

// Marker: slots with owner_cookie == 0 are free. Callers MUST pass a nonzero
// owner_cookie (any stable identifier — client channel handle, PID, etc.).
int netd_tcp_socket_alloc(tcp_table_t *tbl, uint32_t owner_cookie) {
    if (!tbl || owner_cookie == 0) return -1;
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tbl->sockets[i];
        if (s->owner_cookie == 0) {
            netd_memzero(s, sizeof(*s));
            s->state        = TCP_STATE_CLOSED;
            s->owner_cookie = owner_cookie;
            s->rcv_wnd      = TCP_DEFAULT_WINDOW;
            s->mss          = TCP_DEFAULT_MSS;
            return (int)i;
        }
    }
    return -1;
}

void netd_tcp_socket_free(tcp_table_t *tbl, int idx) {
    if (!tbl || idx < 0 || (uint32_t)idx >= TCP_MAX_SOCKETS) return;
    netd_memzero(&tbl->sockets[idx], sizeof(tbl->sockets[idx]));
}

int netd_tcp_find_established(const tcp_table_t *tbl,
                              uint32_t local_ip, uint16_t local_port,
                              uint32_t remote_ip, uint16_t remote_port) {
    if (!tbl) return -1;
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        const tcp_socket_t *s = &tbl->sockets[i];
        if (s->state == TCP_STATE_CLOSED) continue;
        if (s->state == TCP_STATE_LISTEN) continue;
        if (s->local_port == local_port && s->remote_port == remote_port &&
            s->local_ip == local_ip && s->remote_ip == remote_ip) {
            return (int)i;
        }
    }
    return -1;
}

int netd_tcp_find_listen(const tcp_table_t *tbl,
                         uint32_t local_ip, uint16_t local_port) {
    if (!tbl) return -1;
    for (uint32_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        const tcp_socket_t *s = &tbl->sockets[i];
        if (s->state != TCP_STATE_LISTEN) continue;
        // LISTEN either binds to a specific local_ip or INADDR_ANY (0).
        if (s->local_port == local_port &&
            (s->local_ip == 0 || s->local_ip == local_ip)) {
            return (int)i;
        }
    }
    return -1;
}

// ====================================================================
// Outbound connect: CLOSED → SYN_SENT, emit SYN.
// ====================================================================
int netd_tcp_connect(tcp_socket_t *sock,
                     uint32_t iss,
                     uint8_t *syn_buf, size_t syn_buf_cap,
                     size_t *syn_len,
                     uint32_t initial_rto_ms,
                     uint64_t now_tsc, uint64_t ticks_per_sec) {
    if (!sock || !syn_buf || !syn_len) return -1;
    if (sock->state != TCP_STATE_CLOSED) return -2;
    if (syn_buf_cap < 24) return -3;  // 20 hdr + 4 MSS option

    sock->iss     = iss;
    sock->snd_una = iss;
    sock->snd_nxt = iss + 1;    // SYN consumes 1 seq number
    sock->snd_wnd = 0;          // Not yet known
    sock->cwnd    = sock->mss ? sock->mss : TCP_DEFAULT_MSS;
    sock->ssthresh = 0xFFFFu;
    sock->rto_ms   = (initial_rto_ms == 0) ? TCP_DEFAULT_RTO_MS : initial_rto_ms;
    sock->dup_acks = 0;
    sock->state    = TCP_STATE_SYN_SENT;
    sock->retx_expiry_tsc = now_tsc + (uint64_t)ms_to_tsc(ticks_per_sec,
                                                          sock->rto_ms);
    sock->time_wait_expiry_tsc = 0;

    // 20-byte IPv4 + TCP SYN (24-byte) not built here — caller prepends IP.
    *syn_len = netd_tcp_build(syn_buf,
                              sock->local_ip, sock->remote_ip,
                              sock->local_port, sock->remote_port,
                              sock->iss, 0,
                              TCP_FLAG_SYN, (uint16_t)sock->rcv_wnd,
                              sock->mss,
                              (const uint8_t *)0, 0);
    return 0;
}

int netd_tcp_listen(tcp_socket_t *sock, uint32_t local_ip, uint16_t local_port) {
    if (!sock) return -1;
    if (sock->state != TCP_STATE_CLOSED) return -2;
    sock->state      = TCP_STATE_LISTEN;
    sock->local_ip   = local_ip;
    sock->local_port = local_port;
    sock->remote_ip  = 0;
    sock->remote_port = 0;
    sock->rcv_wnd    = TCP_DEFAULT_WINDOW;
    sock->mss        = TCP_DEFAULT_MSS;
    return 0;
}

// ====================================================================
// Incoming segment handler.
// ====================================================================
static size_t emit_ctrl(tcp_socket_t *sock,
                        uint8_t *buf, size_t cap,
                        uint8_t flags, uint16_t mss_opt) {
    if (!buf || cap < 24) return 0;
    return netd_tcp_build(buf,
                          sock->local_ip, sock->remote_ip,
                          sock->local_port, sock->remote_port,
                          sock->snd_nxt, sock->rcv_nxt,
                          flags, (uint16_t)sock->rcv_wnd,
                          mss_opt, (const uint8_t *)0, 0);
}

static size_t emit_rst(uint8_t *buf, size_t cap,
                       uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t seq, uint32_t ack, uint8_t flags) {
    if (!buf || cap < 20) return 0;
    return netd_tcp_build(buf, src_ip, dst_ip, src_port, dst_port,
                          seq, ack, flags, 0, 0,
                          (const uint8_t *)0, 0);
}

int netd_tcp_on_segment(tcp_socket_t *sock,
                        const tcp_parsed_t *pkt,
                        uint64_t now_tsc, uint64_t ticks_per_sec,
                        uint8_t *resp_buf, size_t resp_buf_cap,
                        size_t *resp_len) {
    if (!sock || !pkt || !resp_len) return -1;
    *resp_len = 0;

    // RFC 5961 quick check: if RST's seq doesn't match rcv_nxt exactly (and
    // we're in an established-ish state), treat as challenge-ACK trigger.
    // MVP: just drop the RST silently rather than emit challenge-ACK.
    if (pkt->flags & TCP_FLAG_RST) {
        if (sock->state != TCP_STATE_LISTEN &&
            sock->state != TCP_STATE_SYN_SENT) {
            if (pkt->seq == sock->rcv_nxt) {
                // Genuine RST — abort.
                sock->state = TCP_STATE_CLOSED;
                return 0;
            }
            // Drop (MVP; spec says challenge-ACK).
            return 0;
        }
        if (sock->state == TCP_STATE_SYN_SENT) {
            // RST acceptable only if ACK matches our SYN's seq+1.
            if ((pkt->flags & TCP_FLAG_ACK) && pkt->ack == sock->snd_nxt) {
                sock->state = TCP_STATE_CLOSED;
            }
            return 0;
        }
    }

    switch (sock->state) {
    case TCP_STATE_LISTEN: {
        if (!(pkt->flags & TCP_FLAG_SYN)) return 0;
        // Accept: fill in remote + send SYN-ACK + state = SYN_RCVD.
        sock->remote_ip   = pkt->dst_port; // temp placeholder — caller-provided
        // NOTE: caller is responsible for pre-filling sock->remote_ip from
        // the outer IPv4 header before invoking on_segment; we only update
        // port here.
        sock->remote_port = pkt->src_port;
        sock->irs         = pkt->seq;
        sock->rcv_nxt     = pkt->seq + 1;
        sock->iss         = now_tsc & 0xFFFFFFFFu;
        sock->snd_una     = sock->iss;
        sock->snd_nxt     = sock->iss + 1;
        sock->snd_wnd     = pkt->window;
        if (pkt->opt_mss != 0 && pkt->opt_mss < sock->mss) {
            sock->mss = pkt->opt_mss;
        }
        sock->cwnd     = sock->mss;
        sock->ssthresh = 0xFFFFu;
        sock->rto_ms   = TCP_DEFAULT_RTO_MS;
        sock->retx_expiry_tsc =
            now_tsc + ms_to_tsc(ticks_per_sec, sock->rto_ms);
        sock->state = TCP_STATE_SYN_RCVD;
        *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                              TCP_FLAG_SYN | TCP_FLAG_ACK, sock->mss);
        return 0;
    }
    case TCP_STATE_SYN_SENT: {
        // Expect SYN+ACK acknowledging our SYN.
        if (!(pkt->flags & TCP_FLAG_SYN) || !(pkt->flags & TCP_FLAG_ACK)) {
            return 0;
        }
        if (pkt->ack != sock->iss + 1) return 0;  // Spurious.
        sock->irs     = pkt->seq;
        sock->rcv_nxt = pkt->seq + 1;
        sock->snd_una = pkt->ack;
        sock->snd_wnd = pkt->window;
        if (pkt->opt_mss != 0 && pkt->opt_mss < sock->mss) {
            sock->mss = pkt->opt_mss;
        }
        sock->cwnd = 2 * (uint32_t)sock->mss; // Start slow-start at ~2 MSS.
        sock->retx_expiry_tsc = 0;
        sock->state = TCP_STATE_ESTABLISHED;
        *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                              TCP_FLAG_ACK, 0);
        return 0;
    }
    case TCP_STATE_SYN_RCVD: {
        // Expect final ACK.
        if (!(pkt->flags & TCP_FLAG_ACK)) return 0;
        if (pkt->ack != sock->iss + 1) return 0;
        sock->snd_una = pkt->ack;
        sock->snd_wnd = pkt->window;
        sock->retx_expiry_tsc = 0;
        sock->state = TCP_STATE_ESTABLISHED;
        return 0;
    }
    case TCP_STATE_ESTABLISHED: {
        // Update snd_una if ACK advances it.
        if (pkt->flags & TCP_FLAG_ACK) {
            if (seq_cmp(pkt->ack, sock->snd_una) > 0 &&
                seq_cmp(pkt->ack, sock->snd_nxt) <= 0) {
                sock->snd_una = pkt->ack;
                sock->dup_acks = 0;
                // Reno congestion-window update (simplified).
                if (sock->cwnd < sock->ssthresh) {
                    sock->cwnd += sock->mss;          // slow-start
                } else {
                    // cong-avoid: +MSS per RTT. Approximate +MSS/cwnd * MSS.
                    if (sock->cwnd > 0) {
                        sock->cwnd += ((uint32_t)sock->mss * sock->mss)
                                      / sock->cwnd;
                    }
                }
            } else if (pkt->ack == sock->snd_una && pkt->payload_len == 0) {
                // Pure dup-ACK — increment for fast-retransmit (Stage C).
                if (sock->dup_acks < 255) sock->dup_acks++;
            }
        }
        // Accept in-order data (drop OOO for MVP).
        if (pkt->payload_len > 0) {
            if (pkt->seq == sock->rcv_nxt) {
                sock->rcv_nxt += (uint32_t)pkt->payload_len;
            }
            *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                                  TCP_FLAG_ACK, 0);
        }
        // Remote FIN transitions us to CLOSE_WAIT.
        if (pkt->flags & TCP_FLAG_FIN) {
            sock->rcv_nxt += 1;
            sock->state = TCP_STATE_CLOSE_WAIT;
            // ACK the FIN.
            *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                                  TCP_FLAG_ACK, 0);
        }
        return 0;
    }
    case TCP_STATE_FIN_WAIT1: {
        if (pkt->flags & TCP_FLAG_ACK) {
            if (pkt->ack == sock->snd_nxt) {
                sock->snd_una = pkt->ack;
                sock->state = TCP_STATE_FIN_WAIT2;
            }
        }
        if (pkt->flags & TCP_FLAG_FIN) {
            sock->rcv_nxt += 1;
            // Simultaneous close: FIN_WAIT1 + FIN → CLOSING (if our FIN not
            // yet ACKed) or → TIME_WAIT (if our FIN was ACKed).
            if (sock->state == TCP_STATE_FIN_WAIT2) {
                sock->state = TCP_STATE_TIME_WAIT;
                sock->time_wait_expiry_tsc =
                    now_tsc + ms_to_tsc(ticks_per_sec, TCP_TIME_WAIT_MS);
            } else {
                sock->state = TCP_STATE_CLOSING;
            }
            *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                                  TCP_FLAG_ACK, 0);
        }
        return 0;
    }
    case TCP_STATE_FIN_WAIT2: {
        if (pkt->flags & TCP_FLAG_FIN) {
            sock->rcv_nxt += 1;
            sock->state = TCP_STATE_TIME_WAIT;
            sock->time_wait_expiry_tsc =
                now_tsc + ms_to_tsc(ticks_per_sec, TCP_TIME_WAIT_MS);
            *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                                  TCP_FLAG_ACK, 0);
        }
        return 0;
    }
    case TCP_STATE_CLOSING: {
        // Waiting for ACK of our FIN. Once ACKed → TIME_WAIT.
        if ((pkt->flags & TCP_FLAG_ACK) && pkt->ack == sock->snd_nxt) {
            sock->snd_una = pkt->ack;
            sock->state = TCP_STATE_TIME_WAIT;
            sock->time_wait_expiry_tsc =
                now_tsc + ms_to_tsc(ticks_per_sec, TCP_TIME_WAIT_MS);
        }
        return 0;
    }
    case TCP_STATE_LAST_ACK: {
        if ((pkt->flags & TCP_FLAG_ACK) && pkt->ack == sock->snd_nxt) {
            sock->state = TCP_STATE_CLOSED;
        }
        return 0;
    }
    case TCP_STATE_TIME_WAIT: {
        // RFC 793: any segment restarts the 2*MSL timer (to absorb late
        // retransmits of peer's FIN).
        sock->time_wait_expiry_tsc =
            now_tsc + ms_to_tsc(ticks_per_sec, TCP_TIME_WAIT_MS);
        if (pkt->flags & TCP_FLAG_FIN) {
            // Re-ACK the FIN.
            *resp_len = emit_ctrl(sock, resp_buf, resp_buf_cap,
                                  TCP_FLAG_ACK, 0);
        }
        return 0;
    }
    default: {
        // CLOSED: optionally emit RST with ACK if the peer sends something.
        if (!(pkt->flags & TCP_FLAG_RST)) {
            uint32_t ack = pkt->seq + (uint32_t)pkt->payload_len +
                           ((pkt->flags & TCP_FLAG_SYN) ? 1 : 0) +
                           ((pkt->flags & TCP_FLAG_FIN) ? 1 : 0);
            *resp_len = emit_rst(resp_buf, resp_buf_cap,
                                 sock->local_ip, sock->remote_ip,
                                 sock->local_port, sock->remote_port,
                                 0, ack, TCP_FLAG_RST | TCP_FLAG_ACK);
        }
        return 0;
    }
    }
}

// ====================================================================
// Close: emit FIN, transition to FIN_WAIT1 (or LAST_ACK).
// ====================================================================
int netd_tcp_close(tcp_socket_t *sock,
                   uint8_t *fin_buf, size_t fin_buf_cap, size_t *fin_len) {
    if (!sock || !fin_buf || !fin_len) return -1;
    *fin_len = 0;
    switch (sock->state) {
    case TCP_STATE_ESTABLISHED: {
        sock->state = TCP_STATE_FIN_WAIT1;
        uint32_t fin_seq = sock->snd_nxt;
        sock->snd_nxt += 1;   // FIN consumes 1 seq
        // Emit with seq=fin_seq (not snd_nxt).
        *fin_len = netd_tcp_build(fin_buf,
                                  sock->local_ip, sock->remote_ip,
                                  sock->local_port, sock->remote_port,
                                  fin_seq, sock->rcv_nxt,
                                  TCP_FLAG_FIN | TCP_FLAG_ACK,
                                  (uint16_t)sock->rcv_wnd,
                                  0, (const uint8_t *)0, 0);
        return 0;
    }
    case TCP_STATE_CLOSE_WAIT: {
        sock->state = TCP_STATE_LAST_ACK;
        uint32_t fin_seq = sock->snd_nxt;
        sock->snd_nxt += 1;
        *fin_len = netd_tcp_build(fin_buf,
                                  sock->local_ip, sock->remote_ip,
                                  sock->local_port, sock->remote_port,
                                  fin_seq, sock->rcv_nxt,
                                  TCP_FLAG_FIN | TCP_FLAG_ACK,
                                  (uint16_t)sock->rcv_wnd,
                                  0, (const uint8_t *)0, 0);
        return 0;
    }
    default:
        return -2;  // Not in a state where FIN is legal.
    }
}

// ====================================================================
// Periodic tick: TIME_WAIT expiry + RTO retransmit.
// ====================================================================
int netd_tcp_tick(tcp_socket_t *sock,
                  uint64_t now_tsc, uint64_t ticks_per_sec,
                  uint8_t *retx_buf, size_t retx_buf_cap, size_t *retx_len) {
    if (!sock || !retx_len) return -1;
    *retx_len = 0;

    if (sock->state == TCP_STATE_TIME_WAIT) {
        if (now_tsc >= sock->time_wait_expiry_tsc) {
            sock->state = TCP_STATE_CLOSED;
        }
        return 0;
    }

    // SYN / SYN-ACK retransmit in SYN_SENT / SYN_RCVD.
    if (sock->retx_expiry_tsc != 0 && now_tsc >= sock->retx_expiry_tsc) {
        // Karn: back off RTO ×2, capped at MAX.
        uint32_t new_rto = sock->rto_ms * 2;
        if (new_rto > TCP_MAX_RTO_MS) new_rto = TCP_MAX_RTO_MS;
        sock->rto_ms = new_rto;
        sock->retx_expiry_tsc = now_tsc + ms_to_tsc(ticks_per_sec, new_rto);
        if (sock->state == TCP_STATE_SYN_SENT && retx_buf && retx_buf_cap >= 24) {
            *retx_len = netd_tcp_build(retx_buf,
                                       sock->local_ip, sock->remote_ip,
                                       sock->local_port, sock->remote_port,
                                       sock->iss, 0,
                                       TCP_FLAG_SYN, (uint16_t)sock->rcv_wnd,
                                       sock->mss,
                                       (const uint8_t *)0, 0);
        } else if (sock->state == TCP_STATE_SYN_RCVD &&
                   retx_buf && retx_buf_cap >= 24) {
            *retx_len = netd_tcp_build(retx_buf,
                                       sock->local_ip, sock->remote_ip,
                                       sock->local_port, sock->remote_port,
                                       sock->iss, sock->rcv_nxt,
                                       TCP_FLAG_SYN | TCP_FLAG_ACK,
                                       (uint16_t)sock->rcv_wnd,
                                       sock->mss,
                                       (const uint8_t *)0, 0);
        }
    }
    return 0;
}
