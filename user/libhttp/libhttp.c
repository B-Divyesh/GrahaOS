// user/libhttp/libhttp.c — Phase 22 Stage D.
//
// HTTP/1.1 client over libnet TCP. See libhttp.h for the public contract.

#include "libhttp.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"

// TLS is wired in Stage D U19. The weak-linkage declarations below let
// libhttp compile even when libtls-mg.a is absent; a stub in libhttp.c
// handles the HTTPS path when TLS is unavailable.
struct libtls_ctx;

__attribute__((weak))
int libtls_connect(libnet_client_ctx_t *netctx, uint32_t tcp_cookie,
                   const char *sni, struct libtls_ctx **out_ctx);

__attribute__((weak))
int libtls_write(struct libtls_ctx *ctx, const uint8_t *buf, uint32_t len);

__attribute__((weak))
int libtls_read(struct libtls_ctx *ctx, uint8_t *buf, uint32_t cap,
                uint32_t timeout_ms);

__attribute__((weak))
void libtls_close(struct libtls_ctx *ctx);

// ---------------------------------------------------------------------------
// Case helpers + small internal utilities.
// ---------------------------------------------------------------------------
static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

int http_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = to_lower(*a), cb = to_lower(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)to_lower(*a) - (int)(unsigned char)to_lower(*b);
}

int http_strncasecmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && *b) {
        char ca = to_lower(*a), cb = to_lower(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)to_lower(*a) - (int)(unsigned char)to_lower(*b);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_u16(const char *s, size_t len, uint16_t *out) {
    uint32_t v = 0;
    if (len == 0) return -5;
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(s[i])) return -5;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 0xFFFFu) return -5;
    }
    *out = (uint16_t)v;
    return 0;
}

static int parse_u32(const char *s, size_t len, uint32_t *out) {
    uint64_t v = 0;
    if (len == 0) return -5;
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(s[i])) return -5;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 0xFFFFFFFFu) return -5;
    }
    *out = (uint32_t)v;
    return 0;
}

// Dotted-IPv4 parse (host-order result).
static int parse_dotted_ipv4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    uint32_t octet_start = 0;
    uint32_t octets_seen = 0;
    uint32_t i = 0;
    while (s[i]) {
        uint32_t j = i;
        while (s[j] && s[j] != '.') j++;
        uint16_t o = 0;
        if (parse_u16(s + i, j - i, &o) < 0 || o > 255) return -5;
        v = (v << 8) | (o & 0xFFu);
        octets_seen++;
        i = j;
        if (s[i] == '.') i++;
        if (octets_seen > 4) return -5;
        (void)octet_start;
    }
    if (octets_seen != 4) return -5;
    *out = v;
    return 0;
}

// ---------------------------------------------------------------------------
// URL parser.
// ---------------------------------------------------------------------------
int http_url_parse(const char *url, size_t url_len, http_url_t *out) {
    if (!url || !out) return -5;
    if (url_len > LIBHTTP_MAX_URL_LEN) return -5;
    memset(out, 0, sizeof(*out));

    size_t i = 0;
    // Scheme.
    if (url_len >= 7 && http_strncasecmp(url, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->port    = 80;
        out->is_tls  = 0;
        i = 7;
    } else if (url_len >= 8 && http_strncasecmp(url, "https://", 8) == 0) {
        strcpy(out->scheme, "https");
        out->port    = 443;
        out->is_tls  = 1;
        i = 8;
    } else {
        return -5;
    }

    // Host (up to ':' or '/' or end).
    size_t host_start = i;
    while (i < url_len && url[i] != ':' && url[i] != '/') i++;
    size_t host_len = i - host_start;
    if (host_len == 0 || host_len > LIBHTTP_MAX_HOST_LEN) return -5;
    memcpy(out->host, url + host_start, host_len);
    out->host[host_len] = '\0';

    // Optional explicit port.
    if (i < url_len && url[i] == ':') {
        i++;
        size_t port_start = i;
        while (i < url_len && url[i] != '/') i++;
        size_t port_len = i - port_start;
        if (port_len == 0) return -5;
        uint16_t p = 0;
        if (parse_u16(url + port_start, port_len, &p) < 0) return -5;
        if (p == 0) return -5;
        out->port = p;
    }

    // Path (includes query + fragment — servers only honour query).
    if (i >= url_len) {
        strcpy(out->path, "/");
    } else {
        size_t path_len = url_len - i;
        if (path_len > LIBHTTP_MAX_PATH_LEN) return -5;
        memcpy(out->path, url + i, path_len);
        out->path[path_len] = '\0';
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Status line + header parser.
// ---------------------------------------------------------------------------
int http_parse_status_line(const char *buf, size_t len, int *out_code) {
    if (!buf || !out_code) return -5;
    // "HTTP/1.1 200 OK\r\n"
    if (len < 13) return -5;
    if (http_strncasecmp(buf, "HTTP/1.", 7) != 0) return -5;
    // Skip past version (HTTP/1.X).
    size_t i = 7;
    if (i >= len || !is_digit(buf[i])) return -5;
    i++;
    if (i >= len || buf[i] != ' ') return -5;
    i++;
    // Status code: 3 digits.
    if (i + 3 > len) return -5;
    if (!is_digit(buf[i]) || !is_digit(buf[i + 1]) || !is_digit(buf[i + 2])) {
        return -5;
    }
    *out_code = (buf[i] - '0') * 100 + (buf[i + 1] - '0') * 10 + (buf[i + 2] - '0');
    i += 3;
    // Skip remainder up to \r\n.
    while (i < len && buf[i] != '\r' && buf[i] != '\n') i++;
    if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') return -5;
    i += 2;
    return (int)i;
}

int http_parse_header_block(const char *buf, size_t len,
                            http_header_visitor visitor, void *vctx) {
    size_t i = 0;
    while (i + 1 < len) {
        // End of headers: empty CRLF.
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return (int)(i + 2);
        }
        // Name: up to ':'.
        size_t name_start = i;
        while (i < len && buf[i] != ':' && buf[i] != '\r' && buf[i] != '\n') i++;
        if (i >= len || buf[i] != ':') return -5;
        size_t name_len = i - name_start;
        if (name_len == 0) return -5;
        i++;
        // LWS after colon.
        while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
        // Value: up to CRLF.
        size_t value_start = i;
        while (i < len && buf[i] != '\r' && buf[i] != '\n') i++;
        size_t value_len = i - value_start;
        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') return -5;
        if (visitor) {
            visitor(vctx, buf + name_start, name_len,
                    buf + value_start, value_len);
        }
        i += 2;
    }
    return -5;
}

// ---------------------------------------------------------------------------
// Chunked body decoder (RFC 7230 §4.1).
// Grammar:
//   chunk = chunk-size [ ; ext ] CRLF chunk-data CRLF
//   last-chunk = "0" [ ; ext ] CRLF CRLF (empty trailer)
// We intentionally ignore chunk extensions + trailers.
// ---------------------------------------------------------------------------
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int http_chunked_decode(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_len) {
    size_t si = 0, di = 0;
    while (si < src_len) {
        // Parse hex chunk-size.
        uint32_t sz = 0;
        int saw_digit = 0;
        while (si < src_len && hex_digit((char)src[si]) >= 0) {
            sz = (sz << 4) | (uint32_t)hex_digit((char)src[si]);
            si++;
            saw_digit = 1;
            if (sz > LIBHTTP_MAX_BODY_BYTES) return -5;
        }
        if (!saw_digit) return -5;
        // Skip extensions up to CRLF.
        while (si < src_len && src[si] != '\r') si++;
        if (si + 1 >= src_len || src[si] != '\r' || src[si + 1] != '\n') return -5;
        si += 2;
        if (sz == 0) {
            // Last chunk. Optionally skip trailers up to the final CRLF.
            while (si + 1 < src_len && !(src[si] == '\r' && src[si + 1] == '\n')) {
                // Skip a trailer line.
                while (si < src_len && src[si] != '\r') si++;
                if (si + 1 >= src_len) return -5;
                si += 2;
            }
            return (int)di;
        }
        if (si + sz > src_len) return -5;
        if (di + sz > dst_cap) return -5;
        // Copy bytes. Safe even when src == dst because we never overwrite
        // unread src bytes (di grows at the same pace as si).
        for (uint32_t k = 0; k < sz; k++) {
            dst[di + k] = src[si + k];
        }
        si += sz;
        di += sz;
        // Trailing CRLF after chunk-data.
        if (si + 1 >= src_len || src[si] != '\r' || src[si + 1] != '\n') return -5;
        si += 2;
    }
    return -5;
}

// ---------------------------------------------------------------------------
// Raw-byte I/O over libnet TCP (fragments outbound writes into
// LIBNET_TCP_CHUNK_MAX segments; reads via repeated libnet_tcp_recv calls
// until `out_len` is filled or the peer FIN's).
// ---------------------------------------------------------------------------
static int tcp_send_all(libnet_client_ctx_t *nc, uint32_t cookie,
                        const uint8_t *buf, uint32_t len) {
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > LIBNET_TCP_CHUNK_MAX) chunk = LIBNET_TCP_CHUNK_MAX;
        uint32_t sent = 0;
        int rc = libnet_tcp_send(nc, cookie, buf + offset, (uint16_t)chunk,
                                 5000000000ULL, &sent);
        if (rc < 0) return rc;
        if (sent == 0) return -32 /* EPIPE */;
        offset += sent;
    }
    return 0;
}

// Append bytes from libnet_tcp_recv into a growable buffer. `remaining_ms`
// is updated as time elapses; caller treats <=0 as "time budget exhausted".
static int tcp_recv_into(libnet_client_ctx_t *nc, uint32_t cookie,
                         uint8_t **body_buf, uint32_t *body_len,
                         uint32_t *body_cap, uint32_t *remaining_ms,
                         uint8_t *peer_fin) {
    // Pull up to LIBNET_TCP_CHUNK_MAX bytes; block up to `remaining_ms` but
    // never longer than a single 500 ms slice so we can poll our own budget.
    uint32_t timeout_ms = *remaining_ms > 500u ? 500u : *remaining_ms;
    uint8_t   chunk[LIBNET_TCP_CHUNK_MAX];
    uint16_t  got = 0;
    uint16_t  flags = 0;
    int rc = libnet_tcp_recv(nc, cookie, chunk, sizeof(chunk),
                             timeout_ms, &got, &flags);
    // Update our budget — imprecise by design (wall-clock in MVP).
    if (*remaining_ms >= timeout_ms) *remaining_ms -= timeout_ms;
    else                              *remaining_ms  = 0;

    if (flags & 1u) *peer_fin = 1;

    if (rc == -32 /* EPIPE */) return 1;       // Peer closed cleanly.
    if (rc == -11 /* EAGAIN */) return 0;      // No data this slice.
    if (rc == -110 /* ETIMEDOUT */) return 0;  // Same — just a budget tick.
    if (rc < 0) return rc;

    if (got == 0) return 0;

    if (*body_len + got > *body_cap) {
        uint32_t new_cap = *body_cap ? *body_cap * 2u : 4096u;
        while (new_cap < *body_len + got) new_cap *= 2u;
        if (new_cap > LIBHTTP_MAX_BODY_BYTES + 1024u) {
            new_cap = LIBHTTP_MAX_BODY_BYTES + 1024u;
        }
        uint8_t *nb = (uint8_t *)malloc(new_cap);
        if (!nb) return -12 /* ENOMEM */;
        if (*body_len) memcpy(nb, *body_buf, *body_len);
        if (*body_buf) free(*body_buf);
        *body_buf = nb;
        *body_cap = new_cap;
    }
    uint32_t room = (*body_cap > *body_len) ? (*body_cap - *body_len) : 0;
    uint32_t take = (got > room) ? room : got;
    if (take) memcpy(*body_buf + *body_len, chunk, take);
    *body_len += take;
    return 0;
}

// ---------------------------------------------------------------------------
// Request writer helpers.
// ---------------------------------------------------------------------------
static size_t fmt_decimal(char *dst, uint32_t v) {
    char tmp[11];
    size_t n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (size_t i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    return n;
}

// Compose the request headers into `hdr_buf`. Returns total header length or
// 0 on overflow. Trailing body (if any) is NOT appended.
static uint32_t build_request_headers(char *hdr_buf, uint32_t hdr_cap,
                                      const char *method,
                                      const http_url_t *url,
                                      uint32_t body_len,
                                      const char *content_type) {
    size_t off = 0;
    size_t method_len = strlen(method);
    if (off + method_len + 1 >= hdr_cap) return 0;
    memcpy(hdr_buf + off, method, method_len); off += method_len;
    hdr_buf[off++] = ' ';

    size_t path_len = strlen(url->path);
    if (off + path_len + 12 >= hdr_cap) return 0;
    memcpy(hdr_buf + off, url->path, path_len); off += path_len;
    memcpy(hdr_buf + off, " HTTP/1.1\r\n", 11); off += 11;

    // Host.
    size_t host_len = strlen(url->host);
    memcpy(hdr_buf + off, "Host: ", 6); off += 6;
    if (off + host_len >= hdr_cap) return 0;
    memcpy(hdr_buf + off, url->host, host_len); off += host_len;
    if ((url->is_tls && url->port != 443) ||
        (!url->is_tls && url->port != 80)) {
        if (off + 6 >= hdr_cap) return 0;
        hdr_buf[off++] = ':';
        off += fmt_decimal(hdr_buf + off, url->port);
    }
    memcpy(hdr_buf + off, "\r\n", 2); off += 2;

    // Connection: close.
    memcpy(hdr_buf + off, "Connection: close\r\n", 19); off += 19;

    // User-Agent.
    const char *ua = "User-Agent: libhttp/0.1\r\n";
    size_t ua_len = strlen(ua);
    if (off + ua_len >= hdr_cap) return 0;
    memcpy(hdr_buf + off, ua, ua_len); off += ua_len;

    // Accept-Encoding: identity (no gzip support yet).
    const char *ae = "Accept-Encoding: identity\r\n";
    size_t ae_len = strlen(ae);
    if (off + ae_len >= hdr_cap) return 0;
    memcpy(hdr_buf + off, ae, ae_len); off += ae_len;

    // Content-Length (even zero is fine for POST semantics; GET omits).
    if (body_len > 0 || content_type) {
        memcpy(hdr_buf + off, "Content-Length: ", 16); off += 16;
        if (off + 12 >= hdr_cap) return 0;
        off += fmt_decimal(hdr_buf + off, body_len);
        memcpy(hdr_buf + off, "\r\n", 2); off += 2;

        const char *ct = content_type ? content_type : "application/octet-stream";
        size_t ct_len = strlen(ct);
        if (off + 14 + ct_len + 2 >= hdr_cap) return 0;
        memcpy(hdr_buf + off, "Content-Type: ", 14); off += 14;
        memcpy(hdr_buf + off, ct, ct_len); off += ct_len;
        memcpy(hdr_buf + off, "\r\n", 2); off += 2;
    }

    if (off + 2 >= hdr_cap) return 0;
    memcpy(hdr_buf + off, "\r\n", 2); off += 2;
    return (uint32_t)off;
}

// ---------------------------------------------------------------------------
// Response visitor: extract Content-Length + Transfer-Encoding + Location.
// ---------------------------------------------------------------------------
typedef struct hdr_ctx {
    int32_t  content_length;       // -1 when absent
    uint8_t  chunked;
    char     content_type[128];
    char     location[LIBHTTP_MAX_URL_LEN + 1];
} hdr_ctx_t;

static void header_visitor(void *vctx, const char *name, size_t name_len,
                           const char *value, size_t value_len) {
    hdr_ctx_t *ctx = (hdr_ctx_t *)vctx;
    char lname[64];
    if (name_len >= sizeof(lname)) return;   // Too-long header names ignored.
    for (size_t i = 0; i < name_len; i++) lname[i] = to_lower(name[i]);
    lname[name_len] = '\0';

    if (strcmp(lname, "content-length") == 0) {
        uint32_t v = 0;
        if (parse_u32(value, value_len, &v) == 0) {
            ctx->content_length = (int32_t)v;
        }
    } else if (strcmp(lname, "transfer-encoding") == 0) {
        // Common: "chunked" (case-insensitive).
        if (value_len == 7 && http_strncasecmp(value, "chunked", 7) == 0) {
            ctx->chunked = 1;
        }
    } else if (strcmp(lname, "content-type") == 0) {
        size_t n = value_len < sizeof(ctx->content_type) - 1 ?
                   value_len : sizeof(ctx->content_type) - 1;
        memcpy(ctx->content_type, value, n);
        ctx->content_type[n] = '\0';
    } else if (strcmp(lname, "location") == 0) {
        size_t n = value_len < sizeof(ctx->location) - 1 ?
                   value_len : sizeof(ctx->location) - 1;
        memcpy(ctx->location, value, n);
        ctx->location[n] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Core GET/POST worker. Returns status_code on success, negative errno
// otherwise. On success populates resp + sets body_cap / body_len / etc.
// ---------------------------------------------------------------------------
static int perform_request(http_response_t *resp, const http_url_t *url,
                           const char *method,
                           const uint8_t *body, uint32_t body_len,
                           const char *content_type,
                           uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = LIBHTTP_DEFAULT_TIMEOUT_MS;
    uint32_t remaining_ms = timeout_ms;

    // 1. Connect to netd.
    libnet_client_ctx_t nc;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE, 16,
                                               remaining_ms, &nc);
    if (rc < 0) return rc;

    // 2. Resolve host (unless dotted-IPv4).
    uint32_t dst_ip = 0;
    if (parse_dotted_ipv4(url->host, &dst_ip) < 0) {
        libnet_dns_query_resp_t dns;
        rc = libnet_dns_resolve(&nc, url->host,
                                remaining_ms > 5000u ? 5000u : remaining_ms,
                                &dns);
        if (rc < 0) return rc;
        if (dns.answer_count == 0) return -2 /* ENOENT */;
        dst_ip = dns.answers[0];
    }

    // 3. TCP open.
    uint32_t cookie = 0;
    uint16_t local_port = 0;
    rc = libnet_tcp_open(&nc, dst_ip, url->port,
                         remaining_ms > 6000u ? 6000u : remaining_ms,
                         &cookie, &local_port);
    if (rc < 0) return rc;

    // 4. TLS handshake if scheme=https.
    struct libtls_ctx *tls = NULL;
    if (url->is_tls) {
        if (!libtls_connect) {
            (void)libnet_tcp_close(&nc, cookie, 500000000ULL);
            return -71 /* EPROTO — TLS unavailable */;
        }
        rc = libtls_connect(&nc, cookie, url->host, &tls);
        if (rc < 0) {
            (void)libnet_tcp_close(&nc, cookie, 500000000ULL);
            return rc;
        }
    }

    // 5. Build + send request.
    char hdr_buf[2048];
    uint32_t hdr_len = build_request_headers(hdr_buf, sizeof(hdr_buf),
                                             method, url, body_len,
                                             content_type);
    if (hdr_len == 0) {
        (void)libnet_tcp_close(&nc, cookie, 500000000ULL);
        return -5;
    }
    if (tls) {
        rc = libtls_write(tls, (const uint8_t *)hdr_buf, hdr_len);
        if (rc >= 0 && body_len > 0) {
            rc = libtls_write(tls, body, body_len);
        }
    } else {
        rc = tcp_send_all(&nc, cookie, (const uint8_t *)hdr_buf, hdr_len);
        if (rc >= 0 && body_len > 0) {
            rc = tcp_send_all(&nc, cookie, body, body_len);
        }
    }
    if (rc < 0) {
        if (tls) libtls_close(tls);
        (void)libnet_tcp_close(&nc, cookie, 500000000ULL);
        return rc;
    }

    // 6. Recv response.
    uint8_t *body_buf = NULL;
    uint32_t b_len = 0, b_cap = 0;
    uint8_t peer_fin = 0;
    while (b_len < LIBHTTP_MAX_BODY_BYTES + 1024u && remaining_ms > 0) {
        if (tls) {
            // TLS mode: read in chunks into a local buffer, then stage into
            // body_buf like the plaintext path.
            uint8_t tmp[LIBNET_TCP_CHUNK_MAX];
            int got = libtls_read(tls, tmp, sizeof(tmp),
                                  remaining_ms > 500u ? 500u : remaining_ms);
            if (got < 0) break;
            if (got == 0) {
                if (peer_fin) break;
                // Still connecting — slice off 500 ms and keep looping.
                if (remaining_ms >= 500u) remaining_ms -= 500u;
                else                      remaining_ms  = 0;
                continue;
            }
            if (b_len + got > b_cap) {
                uint32_t nc2 = b_cap ? b_cap * 2u : 4096u;
                while (nc2 < b_len + (uint32_t)got) nc2 *= 2u;
                if (nc2 > LIBHTTP_MAX_BODY_BYTES + 1024u) {
                    nc2 = LIBHTTP_MAX_BODY_BYTES + 1024u;
                }
                uint8_t *nb = (uint8_t *)malloc(nc2);
                if (!nb) { if (body_buf) free(body_buf); return -12; }
                if (b_len) memcpy(nb, body_buf, b_len);
                if (body_buf) free(body_buf);
                body_buf = nb; b_cap = nc2;
            }
            uint32_t room = (b_cap > b_len) ? (b_cap - b_len) : 0;
            uint32_t take = ((uint32_t)got > room) ? room : (uint32_t)got;
            memcpy(body_buf + b_len, tmp, take);
            b_len += take;
        } else {
            int tr = tcp_recv_into(&nc, cookie, &body_buf, &b_len, &b_cap,
                                   &remaining_ms, &peer_fin);
            if (tr < 0) break;
            if (tr == 1 /* peer FIN */) break;
            if (tr == 0 && peer_fin) break;
        }

        // Heuristic: stop as soon as we see Content-Length body or chunked
        // terminator. But keep it simple for MVP — we read until the peer
        // FINs or we hit the cap.
    }

    // 7. Close.
    if (tls) libtls_close(tls);
    (void)libnet_tcp_close(&nc, cookie, 500000000ULL);

    if (b_len == 0) {
        if (body_buf) free(body_buf);
        return -5;
    }

    // 8. Parse status line.
    int status_bytes = http_parse_status_line((const char *)body_buf, b_len,
                                              &resp->status_code);
    if (status_bytes < 0) {
        if (body_buf) free(body_buf);
        return -5;
    }
    uint32_t cursor = (uint32_t)status_bytes;

    // 9. Parse headers.
    hdr_ctx_t hctx;
    memset(&hctx, 0, sizeof(hctx));
    hctx.content_length = -1;
    int hdr_bytes = http_parse_header_block((const char *)body_buf + cursor,
                                            b_len - cursor,
                                            header_visitor, &hctx);
    if (hdr_bytes < 0) {
        if (body_buf) free(body_buf);
        return -5;
    }
    cursor += (uint32_t)hdr_bytes;

    // 10. Assemble body.
    if (hctx.content_type[0]) {
        size_t n = strlen(hctx.content_type);
        if (n >= sizeof(resp->content_type)) n = sizeof(resp->content_type) - 1;
        memcpy(resp->content_type, hctx.content_type, n);
        resp->content_type[n] = '\0';
    }
    if (hctx.location[0]) {
        size_t n = strlen(hctx.location);
        if (n >= sizeof(resp->location)) n = sizeof(resp->location) - 1;
        memcpy(resp->location, hctx.location, n);
        resp->location[n] = '\0';
    }
    resp->chunked = hctx.chunked;

    uint32_t body_bytes = b_len - cursor;
    uint8_t *raw_body   = body_buf + cursor;
    uint8_t *out_body   = NULL;
    uint32_t out_len    = 0;
    uint32_t out_cap    = 0;
    uint8_t  truncated  = 0;

    if (hctx.chunked) {
        // Decode in place into out_body.
        out_cap = body_bytes;  // Can only shrink.
        if (out_cap > LIBHTTP_MAX_BODY_BYTES) {
            out_cap = LIBHTTP_MAX_BODY_BYTES;
            truncated = 1;
        }
        out_body = (uint8_t *)malloc(out_cap + 1);
        if (!out_body) { free(body_buf); return -12; }
        int dl = http_chunked_decode(out_body, out_cap, raw_body, body_bytes);
        if (dl < 0) {
            // Malformed chunked — ship whatever bytes we have verbatim.
            uint32_t n = body_bytes > out_cap ? out_cap : body_bytes;
            memcpy(out_body, raw_body, n);
            out_len = n;
        } else {
            out_len = (uint32_t)dl;
        }
    } else if (hctx.content_length >= 0) {
        uint32_t want = (uint32_t)hctx.content_length;
        uint32_t have = body_bytes < want ? body_bytes : want;
        if (have > LIBHTTP_MAX_BODY_BYTES) { have = LIBHTTP_MAX_BODY_BYTES; truncated = 1; }
        out_cap = have + 1;
        out_body = (uint8_t *)malloc(out_cap);
        if (!out_body) { free(body_buf); return -12; }
        if (have) memcpy(out_body, raw_body, have);
        out_len = have;
    } else {
        // No framing — take everything.
        uint32_t have = body_bytes;
        if (have > LIBHTTP_MAX_BODY_BYTES) { have = LIBHTTP_MAX_BODY_BYTES; truncated = 1; }
        out_cap = have + 1;
        out_body = (uint8_t *)malloc(out_cap);
        if (!out_body) { free(body_buf); return -12; }
        if (have) memcpy(out_body, raw_body, have);
        out_len = have;
    }
    out_body[out_len] = 0;   // Null-terminator for string callers.

    free(body_buf);

    resp->body            = out_body;
    resp->body_len        = out_len;
    resp->body_cap        = out_cap;
    resp->body_truncated  = truncated;
    return resp->status_code;
}

// ---------------------------------------------------------------------------
// Public GET/POST — wrap perform_request with redirect following.
// ---------------------------------------------------------------------------
static int do_request_with_redirects(http_response_t *resp, const char *url,
                                     const char *method,
                                     const uint8_t *body, uint32_t body_len,
                                     const char *content_type,
                                     uint32_t timeout_ms) {
    char cur_url[LIBHTTP_MAX_URL_LEN + 1];
    size_t url_len = strlen(url);
    if (url_len > LIBHTTP_MAX_URL_LEN) return -5;
    memcpy(cur_url, url, url_len);
    cur_url[url_len] = '\0';

    for (uint32_t hop = 0; hop <= LIBHTTP_MAX_REDIRECTS; hop++) {
        http_url_t parsed;
        int rc = http_url_parse(cur_url, strlen(cur_url), &parsed);
        if (rc < 0) return rc;

        http_response_free(resp);
        memset(resp, 0, sizeof(*resp));

        int pc = perform_request(resp, &parsed, method,
                                 body, body_len, content_type,
                                 timeout_ms);
        if (pc < 0) return pc;

        // 3xx — follow if Location is set and we haven't exhausted hops.
        if ((pc == 301 || pc == 302 || pc == 303 || pc == 307 || pc == 308) &&
            resp->location[0] &&
            hop < LIBHTTP_MAX_REDIRECTS) {
            // Only follow absolute redirects in MVP.
            if (resp->location[0] == '/') {
                // Relative path: keep host, swap path.
                size_t host_path_len =
                    strlen(parsed.scheme) + 3 + strlen(parsed.host);
                if (parsed.port != 80 && parsed.port != 443) host_path_len += 6;
                size_t loc_len = strlen(resp->location);
                if (host_path_len + loc_len > LIBHTTP_MAX_URL_LEN) return -5;
                size_t off = 0;
                strcpy(cur_url + off, parsed.scheme); off += strlen(parsed.scheme);
                memcpy(cur_url + off, "://", 3);      off += 3;
                strcpy(cur_url + off, parsed.host);   off += strlen(parsed.host);
                if ((parsed.is_tls && parsed.port != 443) ||
                    (!parsed.is_tls && parsed.port != 80)) {
                    cur_url[off++] = ':';
                    off += fmt_decimal(cur_url + off, parsed.port);
                }
                memcpy(cur_url + off, resp->location, loc_len);
                off += loc_len;
                cur_url[off] = '\0';
                continue;
            } else {
                // Absolute. Just copy.
                size_t n = strlen(resp->location);
                if (n > LIBHTTP_MAX_URL_LEN) return -5;
                memcpy(cur_url, resp->location, n);
                cur_url[n] = '\0';
                continue;
            }
        }
        return pc;
    }
    return -5;
}

int http_get(http_response_t *resp, const char *url, uint32_t timeout_ms) {
    if (!resp || !url) return -5;
    memset(resp, 0, sizeof(*resp));
    return do_request_with_redirects(resp, url, "GET", NULL, 0, NULL,
                                     timeout_ms);
}

int http_post(http_response_t *resp, const char *url,
              const uint8_t *body, uint32_t body_len,
              const char *content_type,
              uint32_t timeout_ms) {
    if (!resp || !url) return -5;
    memset(resp, 0, sizeof(*resp));
    return do_request_with_redirects(resp, url, "POST",
                                     body, body_len, content_type,
                                     timeout_ms);
}

void http_response_free(http_response_t *resp) {
    if (!resp) return;
    if (resp->body) {
        free(resp->body);
        resp->body = NULL;
    }
    resp->body_len = 0;
    resp->body_cap = 0;
}
