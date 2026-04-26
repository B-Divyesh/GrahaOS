// user/tests/netd_dns.c — Phase 22 Stage C.
//
// TAP unit tests for the pure DNS wire helpers in netd_dns.c. No daemon
// involved; we hand-craft query buffers and response fixtures and verify
// the parser extracts what we expect.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../libtap.h"
#include "../netd.h"

extern int printf(const char *fmt, ...);

// =====================================================================
// Query construction.
// =====================================================================
static void test_build_query_simple(void) {
    uint8_t q[128];
    int n = netd_dns_build_query(q, sizeof(q), 0x1234, "example.com", 11);
    TAP_ASSERT(n > 0, "1. build_query example.com returns positive length");
    TAP_ASSERT(n == 12 + 1 + 7 + 1 + 3 + 1 + 4,
               "2. build_query byte count matches expected layout");
    TAP_ASSERT(q[0] == 0x12 && q[1] == 0x34,
               "3. xid encoded big-endian at [0..1]");
    TAP_ASSERT(q[2] == 0x01 && q[3] == 0x00,
               "4. flags == 0x0100 (recursion desired)");
    TAP_ASSERT(q[4] == 0x00 && q[5] == 0x01,
               "5. QDCOUNT == 1");
    TAP_ASSERT(q[6] == 0x00 && q[7] == 0x00 &&
               q[8] == 0x00 && q[9] == 0x00 &&
               q[10] == 0x00 && q[11] == 0x00,
               "6. AN/NS/AR counts all zero");
    // qname: [7]example[3]com[0]
    TAP_ASSERT(q[12] == 7 && q[13] == 'e' && q[14] == 'x' &&
               q[15] == 'a' && q[16] == 'm' && q[17] == 'p' &&
               q[18] == 'l' && q[19] == 'e',
               "7. first label is 'example'");
    TAP_ASSERT(q[20] == 3 && q[21] == 'c' && q[22] == 'o' && q[23] == 'm',
               "8. second label is 'com'");
    TAP_ASSERT(q[24] == 0, "9. labels terminated by zero-length");
    TAP_ASSERT(q[25] == 0 && q[26] == 1, "10. qtype == A (1)");
    TAP_ASSERT(q[27] == 0 && q[28] == 1, "11. qclass == IN (1)");
}

static void test_build_query_invalid(void) {
    uint8_t q[128];
    int n = netd_dns_build_query(q, sizeof(q), 0x1234, "", 0);
    TAP_ASSERT(n < 0, "12. empty hostname rejected");

    n = netd_dns_build_query(q, sizeof(q), 0x1234, ".bad", 4);
    TAP_ASSERT(n < 0, "13. hostname starting with '.' rejected");

    n = netd_dns_build_query(q, sizeof(q), 0x1234, "a..b", 4);
    TAP_ASSERT(n < 0, "14. empty middle label rejected");

    // Tiny buffer: header is 12 bytes + name + 4, so cap=17 can fit
    // 1-byte name. cap=16 should fail.
    n = netd_dns_build_query(q, 16, 0x1234, "a", 1);
    TAP_ASSERT(n < 0, "15. too-small buffer rejected");
}

// =====================================================================
// Response parsing.
// =====================================================================
static void test_parse_response_noerror(void) {
    // Canned response: xid=0x1234, QR=1 RD=1 RA=1 rcode=0
    // qdcount=1 ancount=2 nscount=0 arcount=0
    // Question: example.com A IN
    // Answer 1: name-ptr(0x0C) A IN ttl=300 rdlen=4 1.2.3.4
    // Answer 2: name-ptr(0x0C) A IN ttl=60  rdlen=4 5.6.7.8
    uint8_t buf[128];
    size_t n = 0;
    buf[n++] = 0x12; buf[n++] = 0x34;                // xid
    buf[n++] = 0x81; buf[n++] = 0x80;                // flags (QR+RD+RA, rcode=0)
    buf[n++] = 0x00; buf[n++] = 0x01;                // qd
    buf[n++] = 0x00; buf[n++] = 0x02;                // an
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    // QNAME: 7 example 3 com 0
    buf[n++] = 7; memcpy(&buf[n], "example", 7); n += 7;
    buf[n++] = 3; memcpy(&buf[n], "com", 3);     n += 3;
    buf[n++] = 0;
    // QTYPE / QCLASS
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    // Answer 1
    buf[n++] = 0xC0; buf[n++] = 0x0C;                // name pointer to qname
    buf[n++] = 0x00; buf[n++] = 0x01;                // type A
    buf[n++] = 0x00; buf[n++] = 0x01;                // class IN
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x01; buf[n++] = 0x2C;  // ttl=300
    buf[n++] = 0x00; buf[n++] = 0x04;                // rdlen=4
    buf[n++] = 1; buf[n++] = 2; buf[n++] = 3; buf[n++] = 4;
    // Answer 2
    buf[n++] = 0xC0; buf[n++] = 0x0C;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x3C;  // ttl=60
    buf[n++] = 0x00; buf[n++] = 0x04;
    buf[n++] = 5; buf[n++] = 6; buf[n++] = 7; buf[n++] = 8;

    dns_query_result_t r;
    memset(&r, 0, sizeof(r));
    int pr = netd_dns_parse_response(buf, n, 0x1234, &r);
    TAP_ASSERT(pr == 0, "16. parse_response NOERROR succeeds");
    TAP_ASSERT(r.xid == 0x1234, "17. xid captured");
    TAP_ASSERT(r.rcode == DNS_RCODE_NOERROR, "18. rcode == NOERROR");
    TAP_ASSERT(r.ip_count == 2, "19. two A records parsed");
    TAP_ASSERT(r.ips[0] == 0x01020304u, "20. first IP == 1.2.3.4");
    TAP_ASSERT(r.ips[1] == 0x05060708u, "21. second IP == 5.6.7.8");
    TAP_ASSERT(r.ttl_seconds == 300u, "22. ttl from first answer");
}

static void test_parse_response_nxdomain(void) {
    // xid=0x2222, QR=1 rcode=3 (NXDOMAIN), qdcount=1 ancount=0
    uint8_t buf[64];
    size_t n = 0;
    buf[n++] = 0x22; buf[n++] = 0x22;
    buf[n++] = 0x81; buf[n++] = 0x83;       // QR+RD+RA, rcode=3
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 3; memcpy(&buf[n], "foo", 3); n += 3;
    buf[n++] = 3; memcpy(&buf[n], "tld", 3); n += 3;
    buf[n++] = 0;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;

    dns_query_result_t r;
    memset(&r, 0, sizeof(r));
    int pr = netd_dns_parse_response(buf, n, 0x2222, &r);
    TAP_ASSERT(pr == 0, "23. parse_response NXDOMAIN returns 0");
    TAP_ASSERT(r.rcode == DNS_RCODE_NXDOMAIN, "24. rcode == NXDOMAIN");
    TAP_ASSERT(r.ip_count == 0, "25. no IPs returned");
}

static void test_parse_response_wrong_xid(void) {
    uint8_t buf[32];
    size_t n = 0;
    buf[n++] = 0x11; buf[n++] = 0x11;
    buf[n++] = 0x81; buf[n++] = 0x80;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    dns_query_result_t r;
    int pr = netd_dns_parse_response(buf, n, 0x2222, &r);
    TAP_ASSERT(pr < 0, "26. wrong xid returns negative");
}

static void test_parse_response_not_qr(void) {
    // A QUERY (QR=0) should be rejected.
    uint8_t buf[32];
    size_t n = 0;
    buf[n++] = 0x33; buf[n++] = 0x33;
    buf[n++] = 0x01; buf[n++] = 0x00;   // RD=1, QR=0
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    dns_query_result_t r;
    int pr = netd_dns_parse_response(buf, n, 0x3333, &r);
    TAP_ASSERT(pr < 0, "27. response with QR=0 rejected");
}

static void test_parse_response_skips_non_a(void) {
    // qdcount=1 ancount=3: AAAA (type 28) / CNAME (type 5) / A. Only A is
    // extracted.
    uint8_t buf[256];
    size_t n = 0;
    buf[n++] = 0x44; buf[n++] = 0x44;
    buf[n++] = 0x81; buf[n++] = 0x80;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x03;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 3; memcpy(&buf[n], "foo", 3); n += 3;
    buf[n++] = 3; memcpy(&buf[n], "com", 3); n += 3;
    buf[n++] = 0;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    // AAAA answer (16-byte rdata)
    buf[n++] = 0xC0; buf[n++] = 0x0C;
    buf[n++] = 0x00; buf[n++] = 0x1C;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x3C;
    buf[n++] = 0x00; buf[n++] = 0x10;
    for (int i = 0; i < 16; i++) buf[n++] = (uint8_t)(0x20 + i);
    // CNAME answer (rdata = "example.com" encoded, 13 bytes)
    buf[n++] = 0xC0; buf[n++] = 0x0C;
    buf[n++] = 0x00; buf[n++] = 0x05;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x3C;
    buf[n++] = 0x00; buf[n++] = 0x0D;
    buf[n++] = 7; memcpy(&buf[n], "example", 7); n += 7;
    buf[n++] = 3; memcpy(&buf[n], "com", 3); n += 3;
    buf[n++] = 0;
    // A answer
    buf[n++] = 0xC0; buf[n++] = 0x0C;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x01; buf[n++] = 0x2C;
    buf[n++] = 0x00; buf[n++] = 0x04;
    buf[n++] = 9; buf[n++] = 9; buf[n++] = 9; buf[n++] = 9;

    dns_query_result_t r;
    memset(&r, 0, sizeof(r));
    int pr = netd_dns_parse_response(buf, n, 0x4444, &r);
    TAP_ASSERT(pr == 0, "28. mixed-type response parses successfully");
    TAP_ASSERT(r.ip_count == 1, "29. only one A record extracted");
    TAP_ASSERT(r.ips[0] == 0x09090909u, "30. extracted A = 9.9.9.9");
}

static void test_parse_response_truncated(void) {
    // Header claims ancount=1 but buffer ends mid-answer.
    uint8_t buf[20];
    size_t n = 0;
    buf[n++] = 0x55; buf[n++] = 0x55;
    buf[n++] = 0x81; buf[n++] = 0x80;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = 1; buf[n++] = 'a';
    buf[n++] = 0;
    buf[n++] = 0x00; buf[n++] = 0x01;
    buf[n++] = 0x00; buf[n++] = 0x01;
    // Answer section truncated — only 0 bytes remain; parser must reject.
    dns_query_result_t r;
    memset(&r, 0, sizeof(r));
    int pr = netd_dns_parse_response(buf, n, 0x5555, &r);
    TAP_ASSERT(pr < 0, "31. truncated response rejected");
}

// =====================================================================
void _start(void) {
    printf("=== netd DNS wire-level test suite (Phase 22 Stage C) ===\n");
    tap_plan(31);

    test_build_query_simple();
    test_build_query_invalid();
    test_parse_response_noerror();
    test_parse_response_nxdomain();
    test_parse_response_wrong_xid();
    test_parse_response_not_qr();
    test_parse_response_skips_non_a();
    test_parse_response_truncated();

    tap_done();
    exit(0);
}
