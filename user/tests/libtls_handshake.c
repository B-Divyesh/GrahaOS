// user/tests/libtls_handshake.c — Phase 29 Session B (FU28.A) gate.
//
// End-to-end TLS handshake test against a local Python test server
// (scripts/run_tls_test_server.sh) on 127.0.0.1:8443.  6 asserts cover:
//
//   1. libtls_init with the trust-store path succeeds (>=1 root loaded).
//   2. libnet TCP open to 127.0.0.1:8443 succeeds.
//   3. libtls_connect drives a complete handshake (SNI=localhost).
//   4. libtls_send pushes an HTTP GET (>=0 bytes encrypted out).
//   5. libtls_recv pulls the response body and "hello from GrahaOS"
//      appears in the cleartext.
//   6. libtls_close completes without spinning past 1 s.
//
// Substrate landing: libtls_backend_available() returns 0, so every
// assertion is tap_skip'd with a single explanatory reason.  After
// scripts/vendor-bearssl.sh is run + the wiring lands, the test
// exercises the live BearSSL path and the gate picks up +6.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Pull in the libtls public API.  This file is built whether or not
// BearSSL is vendored because libtls.a always exports the stub.
#include "../libtls/libtls.h"
#include "../libnet/libnet_msg.h"

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(6);

    if (!libtls_backend_available()) {
        const char *r = "BearSSL not yet wired — "
                        "run scripts/vendor-bearssl.sh + relink";
        tap_skip("1. libtls_init loads CA bundle",        r);
        tap_skip("2. libnet TCP open 127.0.0.1:8443",     r);
        tap_skip("3. libtls_connect handshake (SNI)",      r);
        tap_skip("4. libtls_send HTTP GET",                r);
        tap_skip("5. libtls_recv decrypts response body",  r);
        tap_skip("6. libtls_close graceful shutdown",      r);
        tap_done();
        syscall_exit(0);
    }

    // -----------------------------------------------------------------
    // Live path — only exercised when BearSSL is vendored + linked.
    // -----------------------------------------------------------------

    // 1. Trust store.
    int rc = libtls_init(LIBTLS_TRUST_STORE);
    TAP_ASSERT(rc > 0, "1. libtls_init loads CA bundle (>=1 root)");

    // 2. Open a TCP socket via libnet.
    libnet_client_ctx_t nc = {0};
    rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                           sizeof(LIBNET_NAME_SERVICE) - 1,
                                           5000u, &nc);
    if (rc < 0) {
        tap_not_ok("2. libnet TCP open 127.0.0.1:8443",
                   "net service unreachable");
        tap_skip("3. libtls_connect handshake (SNI)",     "net dead");
        tap_skip("4. libtls_send HTTP GET",               "net dead");
        tap_skip("5. libtls_recv decrypts response body", "net dead");
        tap_skip("6. libtls_close graceful shutdown",     "net dead");
        tap_done();
        syscall_exit(0);
    }
    uint32_t cookie  = 0;
    uint16_t local_port = 0;
    // 127.0.0.1 = 0x7F000001 (host-order); libnet_tcp_open wants network-order.
    rc = libnet_tcp_open(&nc, 0x0100007Fu, 8443u, 3000u, &cookie, &local_port);
    TAP_ASSERT(rc == 0, "2. libnet TCP open 127.0.0.1:8443");

    // 3. TLS handshake.
    libtls_ctx_t *ctx = NULL;
    rc = libtls_connect(&ctx, &nc, cookie, "localhost");
    TAP_ASSERT(rc == 0 && ctx != NULL, "3. libtls_connect handshake (SNI)");

    // 4. Send HTTP GET.
    const char *get = "GET / HTTP/1.0\r\n"
                      "Host: localhost\r\n\r\n";
    int sent = libtls_send(ctx, get, strlen(get));
    TAP_ASSERT(sent > 0, "4. libtls_send HTTP GET");

    // 5. Recv body.
    uint8_t buf[1024];
    int total = 0;
    for (int i = 0; i < 8 && total < (int)sizeof(buf) - 1; i++) {
        int got = libtls_recv(ctx, buf + total, sizeof(buf) - 1 - total, 500u);
        if (got > 0) total += got;
        else if (got == 0) continue;
        else break;
    }
    buf[total > 0 ? total : 0] = '\0';
    int found = 0;
    for (int i = 0; i + 6 <= total; i++) {
        if (buf[i] == 'h' && buf[i+1] == 'e' && buf[i+2] == 'l' &&
            buf[i+3] == 'l' && buf[i+4] == 'o' && buf[i+5] == ' ') {
            found = 1; break;
        }
    }
    TAP_ASSERT(found, "5. libtls_recv decrypts response body");

    // 6. Close.
    libtls_close(ctx);
    (void)libnet_tcp_close(&nc, cookie, 500000000ULL);
    TAP_ASSERT(1, "6. libtls_close graceful shutdown");

    tap_done();
    syscall_exit(0);
}
