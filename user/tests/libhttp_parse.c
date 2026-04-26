// user/tests/libhttp_parse.c — Phase 22 Stage D TAP test.
//
// Pure-wire coverage of libhttp's URL parser, status-line parser,
// header-block walker, and chunked decoder. These are the pieces that run
// entirely offline; live-TCP coverage of libhttp lives in Stage E once
// httptest migrates to the channel-RPC stack.

#include "libtap.h"
#include "../libhttp/libhttp.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

extern void syscall_exit(int code);

// --- URL parser ------------------------------------------------------------
static void test_url_parse_http_basic(void) {
    http_url_t u;
    TAP_ASSERT(http_url_parse("http://example.com/", 19, &u) == 0,
               "url: http://example.com/ parses");
    TAP_ASSERT(strcmp(u.scheme, "http") == 0, "url: scheme=http");
    TAP_ASSERT(strcmp(u.host,   "example.com") == 0, "url: host=example.com");
    TAP_ASSERT(u.port == 80, "url: default port 80");
    TAP_ASSERT(strcmp(u.path, "/") == 0, "url: path=/");
    TAP_ASSERT(u.is_tls == 0, "url: is_tls=0 for http://");
}

static void test_url_parse_https_port_path(void) {
    http_url_t u;
    const char *s = "https://example.com:8443/foo/bar?baz=1";
    TAP_ASSERT(http_url_parse(s, strlen(s), &u) == 0,
               "url: https with port+path+query parses");
    TAP_ASSERT(strcmp(u.scheme, "https") == 0, "url: scheme=https");
    TAP_ASSERT(u.port == 8443, "url: explicit port 8443 honored");
    TAP_ASSERT(strcmp(u.path, "/foo/bar?baz=1") == 0,
               "url: path+query preserved");
    TAP_ASSERT(u.is_tls == 1, "url: is_tls=1 for https://");
}

static void test_url_parse_default_port(void) {
    http_url_t u;
    TAP_ASSERT(http_url_parse("https://a/", 10, &u) == 0,
               "url: https with empty path parses");
    TAP_ASSERT(u.port == 443, "url: default https port 443");
}

static void test_url_parse_rejects_malformed(void) {
    http_url_t u;
    TAP_ASSERT(http_url_parse("ftp://x/", 8, &u) < 0,
               "url: ftp:// rejected");
    TAP_ASSERT(http_url_parse("http:///missing-host", 20, &u) < 0,
               "url: empty host rejected");
    TAP_ASSERT(http_url_parse("http://host:/x", 14, &u) < 0,
               "url: empty port rejected");
}

// --- Status line -----------------------------------------------------------
static void test_status_line_ok(void) {
    const char *s = "HTTP/1.1 200 OK\r\n";
    int code = 0;
    int n = http_parse_status_line(s, strlen(s), &code);
    TAP_ASSERT(n > 0, "status: valid HTTP/1.1 200 OK consumed");
    TAP_ASSERT(code == 200, "status: code=200");
}

static void test_status_line_3xx(void) {
    const char *s = "HTTP/1.0 302 Found\r\n";
    int code = 0;
    int n = http_parse_status_line(s, strlen(s), &code);
    TAP_ASSERT(n > 0, "status: HTTP/1.0 302 Found consumed");
    TAP_ASSERT(code == 302, "status: code=302 (redirect)");
}

static void test_status_line_reject(void) {
    const char *bad1 = "HTTP/2.0 200 OK\r\n"; // HTTP/2 not supported
    const char *bad2 = "HTTP/1.1 20 OK\r\n";  // 2-digit code
    int code = 0;
    TAP_ASSERT(http_parse_status_line(bad1, strlen(bad1), &code) < 0,
               "status: HTTP/2 rejected");
    TAP_ASSERT(http_parse_status_line(bad2, strlen(bad2), &code) < 0,
               "status: 2-digit code rejected");
}

// --- Header block ---------------------------------------------------------
typedef struct hvis_state {
    int content_length;
    char content_type[64];
    int saw_end;
} hvis_state_t;

static void test_visitor(void *ctx, const char *name, size_t nlen,
                         const char *val, size_t vlen) {
    hvis_state_t *s = (hvis_state_t *)ctx;
    if (nlen == 14 && http_strncasecmp(name, "Content-Length", 14) == 0) {
        char tmp[16];
        size_t k = vlen < 15 ? vlen : 15;
        for (size_t i = 0; i < k; i++) tmp[i] = val[i];
        tmp[k] = '\0';
        int v = 0;
        for (size_t i = 0; i < k && tmp[i] >= '0' && tmp[i] <= '9'; i++) {
            v = v * 10 + (tmp[i] - '0');
        }
        s->content_length = v;
    } else if (nlen == 12 && http_strncasecmp(name, "Content-Type", 12) == 0) {
        size_t k = vlen < 63 ? vlen : 63;
        for (size_t i = 0; i < k; i++) s->content_type[i] = val[i];
        s->content_type[k] = '\0';
    }
}

static void test_header_block_basic(void) {
    const char *h = "Host: example.com\r\n"
                     "Content-Length: 42\r\n"
                     "Content-Type: text/plain\r\n"
                     "\r\n";
    hvis_state_t st;
    memset(&st, 0, sizeof(st));
    int n = http_parse_header_block(h, strlen(h), test_visitor, &st);
    TAP_ASSERT(n > 0, "headers: block consumed");
    TAP_ASSERT((size_t)n == strlen(h),
               "headers: n equals block length (including trailing CRLF)");
    TAP_ASSERT(st.content_length == 42,
               "headers: Content-Length parsed as 42");
    TAP_ASSERT(strcmp(st.content_type, "text/plain") == 0,
               "headers: Content-Type captured");
}

static void test_header_block_missing_crlf(void) {
    const char *h = "Host: example.com\r\nContent-Length: 5\r\n";
    hvis_state_t st;
    memset(&st, 0, sizeof(st));
    int n = http_parse_header_block(h, strlen(h), test_visitor, &st);
    TAP_ASSERT(n < 0, "headers: missing blank-line terminator rejected");
}

// --- Chunked decoder ------------------------------------------------------
static void test_chunked_simple(void) {
    const char *src = "5\r\nhello\r\n0\r\n\r\n";
    uint8_t dst[32];
    int n = http_chunked_decode(dst, sizeof(dst),
                                (const uint8_t *)src, strlen(src));
    TAP_ASSERT(n == 5, "chunked: simple 5-byte chunk decodes");
    TAP_ASSERT(memcmp(dst, "hello", 5) == 0,
               "chunked: payload == \"hello\"");
}

static void test_chunked_multiple(void) {
    const char *src = "3\r\nabc\r\n4\r\nWXYZ\r\n0\r\n\r\n";
    uint8_t dst[32];
    int n = http_chunked_decode(dst, sizeof(dst),
                                (const uint8_t *)src, strlen(src));
    TAP_ASSERT(n == 7, "chunked: multi-chunk decodes to 7 bytes");
    TAP_ASSERT(memcmp(dst, "abcWXYZ", 7) == 0,
               "chunked: chunks concatenated correctly");
}

static void test_chunked_hex_size(void) {
    const char *src = "1A\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\n";
    uint8_t dst[64];
    int n = http_chunked_decode(dst, sizeof(dst),
                                (const uint8_t *)src, strlen(src));
    TAP_ASSERT(n == 26, "chunked: hex 1A = 26 bytes decoded");
}

static void test_chunked_malformed(void) {
    const char *src = "xyz\r\n";    // Non-hex size
    uint8_t dst[16];
    int n = http_chunked_decode(dst, sizeof(dst),
                                (const uint8_t *)src, strlen(src));
    TAP_ASSERT(n < 0, "chunked: non-hex size rejected");
}

// --- strcasecmp helpers ---------------------------------------------------
static void test_strcasecmp(void) {
    TAP_ASSERT(http_strcasecmp("HOST", "host") == 0,
               "strcase: HOST == host");
    TAP_ASSERT(http_strcasecmp("abc", "abd") != 0,
               "strcase: abc != abd");
    TAP_ASSERT(http_strncasecmp("Connection: close", "CONNECTION", 10) == 0,
               "strncase: prefix match case-insensitive");
}

void _start(void) {
    tap_plan(0);
    test_url_parse_http_basic();
    test_url_parse_https_port_path();
    test_url_parse_default_port();
    test_url_parse_rejects_malformed();
    test_status_line_ok();
    test_status_line_3xx();
    test_status_line_reject();
    test_header_block_basic();
    test_header_block_missing_crlf();
    test_chunked_simple();
    test_chunked_multiple();
    test_chunked_hex_size();
    test_chunked_malformed();
    test_strcasecmp();
    tap_done();
    syscall_exit(0);
}
