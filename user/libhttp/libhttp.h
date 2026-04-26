// user/libhttp/libhttp.h — Phase 22 Stage D.
//
// HTTP/1.1 client library on top of libnet's TCP primitives. Targets the
// same surface area as the deleted kernel SYS_HTTP_GET / SYS_HTTP_POST
// syscalls so Stage E migrations can swap in a few lines:
//
//     // before
//     int rc = syscall_http_get(url, resp_buf, resp_cap);
//     // after
//     http_response_t resp;
//     int rc = http_get(&resp, url, /*timeout_ms=*/10000);
//     // ...use resp.body, resp.body_len, resp.status_code...
//     http_response_free(&resp);
//
// Scope (Stage D MVP):
//   - http:// and https:// schemes.
//   - GET and POST (arbitrary Content-Type).
//   - Content-Length and `Transfer-Encoding: chunked` bodies.
//   - 3xx redirect follow, up to LIBHTTP_MAX_REDIRECTS hops.
//   - Host header with implicit port (omitted for 80/443) + `Connection:
//     close` always — keep-alive is deferred to a future phase.
//   - Timeout budget shared across DNS + TCP open + request + response.
//   - Bodies up to LIBHTTP_MAX_BODY_BYTES; larger responses truncate with
//     `body_truncated=1` set.
//
// Stage D does NOT cover: streaming bodies (caller-supplied iterator),
// header enumeration, WebSocket upgrade, HTTP/2, gzip decoding, or
// authentication schemes. Those can land in a post-MVP phase once libhttp
// has surfaced real needs.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../libnet/libnet.h"

#define LIBHTTP_MAX_REDIRECTS   5u
#define LIBHTTP_MAX_URL_LEN     1024u
#define LIBHTTP_MAX_HOST_LEN    255u    // RFC 1035 §2.3.4
#define LIBHTTP_MAX_PATH_LEN    1024u
#define LIBHTTP_MAX_BODY_BYTES  (128u * 1024u)  // 128 KiB cap per response
#define LIBHTTP_DEFAULT_TIMEOUT_MS  10000u

// Parsed URL. Buffers are inline so callers don't have to free anything.
typedef struct http_url {
    char     scheme[8];          // "http" or "https"
    char     host[LIBHTTP_MAX_HOST_LEN + 1];
    uint16_t port;               // Defaults: 80 for http, 443 for https
    char     path[LIBHTTP_MAX_PATH_LEN + 1];  // Includes leading '/' + query
    uint8_t  is_tls;             // 1 if scheme == "https"
} http_url_t;

// Parsed HTTP response. `body` is malloc'd; caller MUST pair every
// successful http_get / http_post with http_response_free.
typedef struct http_response {
    int      status_code;        // e.g. 200, 404
    uint32_t body_len;           // Bytes valid in body (may be 0)
    uint32_t body_cap;           // Allocated size; body_len <= body_cap
    uint8_t *body;               // malloc'd buffer. NULL when body_cap == 0
    char     content_type[128];  // Copy of first Content-Type header (NUL-term)
    char     location[LIBHTTP_MAX_URL_LEN + 1];  // 3xx Location header
    uint8_t  body_truncated;     // Set if response exceeded LIBHTTP_MAX_BODY_BYTES
    uint8_t  chunked;            // Set if Transfer-Encoding: chunked was seen
    uint8_t  _pad[2];
} http_response_t;

// Public API -----------------------------------------------------------------

// Parse a URL. Accepts "http://" and "https://" with optional port + path.
// Returns 0 on success, negative errno on malformed input.
int http_url_parse(const char *url, size_t url_len, http_url_t *out);

// GET <url>. On success populates *resp; caller frees with http_response_free.
// `timeout_ms == 0` uses LIBHTTP_DEFAULT_TIMEOUT_MS.
int http_get(http_response_t *resp, const char *url, uint32_t timeout_ms);

// POST <body> to <url> with Content-Type <content_type> (may be NULL for
// "application/octet-stream"). Body pointer + length may be 0/NULL.
int http_post(http_response_t *resp, const char *url,
              const uint8_t *body, uint32_t body_len,
              const char *content_type,
              uint32_t timeout_ms);

// Free the allocated body buffer and zero the struct. Safe to call on a
// zero-init'd struct (no-op).
void http_response_free(http_response_t *resp);

// Internal helpers exposed for unit tests ------------------------------------

// Parse a status line of the form "HTTP/1.1 200 OK\r\n" into out_code.
// Returns the number of bytes consumed, or negative on malformed input.
int http_parse_status_line(const char *buf, size_t len, int *out_code);

// Walk a CRLF-delimited header block starting at `buf`. For each line,
// the caller's visitor is invoked with (name, value). Returns the number
// of bytes consumed up to and including the terminating empty line
// ("\r\n\r\n"), or negative on malformed input.
typedef void (*http_header_visitor)(void *ctx, const char *name,
                                    size_t name_len,
                                    const char *value, size_t value_len);
int http_parse_header_block(const char *buf, size_t len,
                            http_header_visitor visitor, void *visitor_ctx);

// Decode a chunked body in place. Returns the decoded length on success,
// negative on malformed input. `src` and `dst` may alias (in-place
// decoding is the common case).
int http_chunked_decode(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_len);

// Case-insensitive ASCII string compare. Returns 0 on equal.
int http_strcasecmp(const char *a, const char *b);
int http_strncasecmp(const char *a, const char *b, size_t n);
