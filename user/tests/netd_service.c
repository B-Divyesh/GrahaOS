// user/tests/netd_service.c — Phase 22 Stage C.
//
// TAP unit tests for libnet's /sys/net/service wire helpers:
//   - libnet_msg_unpack_header
//   - libnet_msg_build_err_response
//   - op-code / response-bit conventions
//   - net_query / icmp_echo / dns_query struct layouts fit CHAN_MSG_INLINE_MAX.
//
// No live daemon. We forge chan_msg_user_t buffers in memory and drive the
// pack/unpack helpers directly.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../libtap.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

static void test_header_unpack(void) {
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));

    libnet_msg_header_t *h = (libnet_msg_header_t *)m.inline_payload;
    h->op   = LIBNET_OP_NET_QUERY_REQ;
    h->_pad = 0;
    h->seq  = 0xDEADBEEFu;
    m.header.inline_len = sizeof(*h);

    uint16_t op = 0;
    uint32_t seq = 0;
    int rc = libnet_msg_unpack_header(&m, &op, &seq);
    TAP_ASSERT(rc == 0, "1. unpack_header succeeds");
    TAP_ASSERT(op == LIBNET_OP_NET_QUERY_REQ,
               "2. unpack_header captures op");
    TAP_ASSERT(seq == 0xDEADBEEFu, "3. unpack_header captures seq");
}

static void test_header_too_short(void) {
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = 4;   // smaller than libnet_msg_header_t
    uint16_t op = 0xFFFFu;
    uint32_t seq = 0xFFFFFFFFu;
    int rc = libnet_msg_unpack_header(&m, &op, &seq);
    TAP_ASSERT(rc < 0, "4. truncated header rejected");
}

static void test_build_err_response(void) {
    uint8_t buf[32];
    memset(buf, 0xAA, sizeof(buf));
    uint16_t n = libnet_msg_build_err_response(buf, sizeof(buf),
                                                LIBNET_OP_NET_QUERY_RESP,
                                                0x1234u, -110 /*-ETIMEDOUT*/);
    TAP_ASSERT(n == sizeof(libnet_msg_header_t) + sizeof(int32_t),
               "5. err_response length correct");
    libnet_msg_header_t *h = (libnet_msg_header_t *)buf;
    TAP_ASSERT(h->op == LIBNET_OP_NET_QUERY_RESP,
               "6. err_response op preserved");
    TAP_ASSERT(h->seq == 0x1234u, "7. err_response seq preserved");
    int32_t *st = (int32_t *)(buf + sizeof(*h));
    TAP_ASSERT(*st == -110, "8. err_response status preserved");

    // Too-small buffer → returns 0.
    n = libnet_msg_build_err_response(buf, 4,
                                       LIBNET_OP_NET_QUERY_RESP, 1, -1);
    TAP_ASSERT(n == 0, "9. err_response rejects too-small buffer");
}

static void test_opcode_convention(void) {
    // Every known OP_RESP matches (OP_REQ | 0x8000).
    TAP_ASSERT((LIBNET_OP_HELLO_REQ  | LIBNET_OP_MASK_RESP) == LIBNET_OP_HELLO_RESP,
               "10. HELLO resp bit convention");
    TAP_ASSERT((LIBNET_OP_NET_QUERY_REQ | LIBNET_OP_MASK_RESP) == LIBNET_OP_NET_QUERY_RESP,
               "11. NET_QUERY resp bit convention");
    TAP_ASSERT((LIBNET_OP_ICMP_ECHO_REQ | LIBNET_OP_MASK_RESP) == LIBNET_OP_ICMP_ECHO_RESP,
               "12. ICMP_ECHO resp bit convention");
    TAP_ASSERT((LIBNET_OP_UDP_BIND_REQ  | LIBNET_OP_MASK_RESP) == LIBNET_OP_UDP_BIND_RESP,
               "13. UDP_BIND resp bit convention");
    TAP_ASSERT((LIBNET_OP_UDP_SENDTO_REQ | LIBNET_OP_MASK_RESP) == LIBNET_OP_UDP_SENDTO_RESP,
               "14. UDP_SENDTO resp bit convention");
    TAP_ASSERT((LIBNET_OP_DNS_QUERY_REQ | LIBNET_OP_MASK_RESP) == LIBNET_OP_DNS_QUERY_RESP,
               "15. DNS_QUERY resp bit convention");
    TAP_ASSERT((LIBNET_OP_TCP_OPEN_REQ  | LIBNET_OP_MASK_RESP) == LIBNET_OP_TCP_OPEN_RESP,
               "16. TCP_OPEN resp bit convention");
}

static void test_struct_size_fits_inline(void) {
    TAP_ASSERT(sizeof(libnet_net_query_resp_t) <= CHAN_MSG_INLINE_MAX,
               "17. net_query_resp fits inline payload");
    TAP_ASSERT(sizeof(libnet_icmp_echo_req_t) <= CHAN_MSG_INLINE_MAX,
               "18. icmp_echo_req fits inline payload");
    TAP_ASSERT(sizeof(libnet_icmp_echo_resp_t) <= CHAN_MSG_INLINE_MAX,
               "19. icmp_echo_resp fits inline payload");
    TAP_ASSERT(sizeof(libnet_udp_sendto_req_t) <= CHAN_MSG_INLINE_MAX,
               "20. udp_sendto_req fits inline payload");
    TAP_ASSERT(sizeof(libnet_dns_query_resp_t) <= CHAN_MSG_INLINE_MAX,
               "21. dns_query_resp fits inline payload");
}

static void test_header_alignment(void) {
    TAP_ASSERT(sizeof(libnet_msg_header_t) == 8,
               "22. libnet_msg_header_t is 8 bytes");
    // The hdr offset should be 0 inside every request/response.
    TAP_ASSERT(offsetof(libnet_net_query_req_t, hdr) == 0,
               "23. net_query_req.hdr at offset 0");
    TAP_ASSERT(offsetof(libnet_net_query_resp_t, hdr) == 0,
               "24. net_query_resp.hdr at offset 0");
    TAP_ASSERT(offsetof(libnet_icmp_echo_resp_t, status) == sizeof(libnet_msg_header_t),
               "25. icmp_echo_resp.status right after header");
    TAP_ASSERT(offsetof(libnet_dns_query_req_t, name) >= sizeof(libnet_msg_header_t),
               "26. dns_query_req.name after header");
}

static void test_net_query_fields(void) {
    TAP_ASSERT(LIBNET_NET_QUERY_FIELD_CONFIG == 1u,
               "27. NET_QUERY_FIELD_CONFIG == 1");
    TAP_ASSERT(LIBNET_NET_QUERY_FIELD_STATUS == 2u,
               "28. NET_QUERY_FIELD_STATUS == 2");
    TAP_ASSERT(LIBNET_NET_QUERY_FIELD_ALL    == 3u,
               "29. NET_QUERY_FIELD_ALL == 3");
}

void _start(void) {
    printf("=== libnet /sys/net/service message test suite (Phase 22 Stage C) ===\n");
    tap_plan(29);

    test_header_unpack();
    test_header_too_short();
    test_build_err_response();
    test_opcode_convention();
    test_struct_size_fits_inline();
    test_header_alignment();
    test_net_query_fields();

    tap_done();
    exit(0);
}
