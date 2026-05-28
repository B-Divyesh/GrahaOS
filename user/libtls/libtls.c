// user/libtls/libtls.c — Phase 29 Session B (FU28.A).
//
// Substrate landing.  This file is the glue layer between libhttp and a
// BearSSL backend (MIT-licensed, vendored under vendor/bearssl/).  In this
// session we ship:
//
//   * The vendor/bearssl/ tree (real source, scripts/vendor-bearssl.sh
//     re-fetches on demand).
//   * The audit codes (kernel/audit.h codes 55 + 56).
//   * The trust store at /etc/tls/trust.pem (real Mozilla CA bundle).
//   * The test scaffolding (libtls_handshake.tap, libtls.tap).
//
// What we DO NOT yet ship is the BearSSL <-> libnet wiring.  The handshake
// driver, X.509 verification path, and record-layer pumping are sized at
// ~400 LOC and need a focused session of their own to land alongside a
// concrete test server (scripts/run_tls_test_server.sh).
//
// Until that lands, every public entry point either returns a -ENOSYS-class
// errno (so callers degrade gracefully) or — for libtls_close — is a safe
// no-op.  libhttp's HTTPS path is left untouched: it keeps using the Phase
// 22 libtls-mg shim (linker resolves libtls_connect against whichever
// archive ships first; libtls-mg still wins in the current Makefile order),
// so grahai HTTPS to Gemini continues to work today.
//
// Sentinel observable from tests:
//
//     int libtls_backend_available(void);
//     // Returns 0 in this substrate landing; flips to 1 when BearSSL
//     // wiring lands.
//
// Tests use the sentinel to choose between tap_skip + real assertions.

#include "libtls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../syscalls.h"

// Internal opaque ctx. Definition lives in this file; tests + libhttp only
// see the forward declaration in libtls.h.
struct libtls_ctx {
    libnet_client_ctx_t *nc;
    uint32_t             cookie;
    uint8_t              handshake_done;
    char                 sni[LIBTLS_MAX_SNI_LEN + 1];
};

// ---------------------------------------------------------------------------
// Sentinel: 0 in substrate landing, flips to 1 when BearSSL is wired up.
// libtls_backend_available() returns this value verbatim.
// ---------------------------------------------------------------------------
static const int libtls_have_bearssl = 0;

int libtls_backend_available(void) {
    return libtls_have_bearssl;
}

// ---------------------------------------------------------------------------
// libtls_init: read the trust store + drive BearSSL X.509 parser.
// ---------------------------------------------------------------------------
int libtls_init(const char *ca_bundle_path) {
    (void)ca_bundle_path;
    if (!libtls_have_bearssl) {
        return -38; // -ENOSYS
    }
    // Real path (once BearSSL is wired):
    //   1. open(ca_bundle_path ? ca_bundle_path : LIBTLS_TRUST_STORE)
    //   2. read into kmalloc'd buffer
    //   3. feed to br_pem_decoder + br_x509_decoder
    //   4. populate static br_x509_trust_anchor table
    //   5. return n_anchors on success.
    return -38;
}

// ---------------------------------------------------------------------------
// libtls_connect: handshake driver.
// ---------------------------------------------------------------------------
int libtls_connect(libtls_ctx_t **out_ctx, libnet_client_ctx_t *nc,
                   uint32_t tcp_cookie, const char *hostname) {
    (void)tcp_cookie;
    if (!out_ctx || !nc || !hostname) return -22; // -EINVAL
    if (!libtls_have_bearssl) {
        return -38;
    }
    // Real path (once BearSSL is wired):
    //   1. allocate libtls_ctx_t + br_ssl_client_context.
    //   2. br_ssl_client_init_full(...).
    //   3. attach SNI: br_ssl_client_reset(eng, hostname, 0).
    //   4. pump br_ssl_engine_*_buf {sendrec,recvrec} <-> libnet_tcp_*.
    //   5. on handshake fail: audit_write_tls_handshake_fail.
    //   6. on cert verify fail: audit_write_tls_cert_verify_fail.
    return -38;
}

// ---------------------------------------------------------------------------
// libtls_send / libtls_recv: record-layer pump.
// ---------------------------------------------------------------------------
int libtls_send(libtls_ctx_t *ctx, const void *buf, size_t len) {
    if (!ctx || !buf) return -22;
    if (!ctx->handshake_done) return -107; // -ENOTCONN
    (void)len;
    return -38;
}

int libtls_recv(libtls_ctx_t *ctx, void *buf, size_t len,
                uint32_t timeout_ms) {
    if (!ctx || !buf) return -22;
    if (!ctx->handshake_done) return -107;
    (void)len; (void)timeout_ms;
    return -38;
}

// ---------------------------------------------------------------------------
// libtls_close: graceful close_notify + teardown.  Safe with NULL.
// ---------------------------------------------------------------------------
void libtls_close(libtls_ctx_t *ctx) {
    if (!ctx) return;
    // Real path: send close_notify alert through ctx->nc/cookie, free ctx.
    // Substrate: nothing was allocated, so just bail.
    (void)ctx;
}

// ---------------------------------------------------------------------------
// libhttp compatibility wrappers (Phase 22 signatures).
//
// The real libtls.a in this substrate landing is NOT linked into libhttp
// (libtls-mg.a still wins because the Makefile keeps the old order).  When
// the BearSSL wiring lands, these wrappers ARE the symbols libhttp calls
// and `libtls-mg` is deleted.
// ---------------------------------------------------------------------------
int libtls_write(libtls_ctx_t *ctx, const uint8_t *buf, uint32_t len) {
    int rc = libtls_send(ctx, buf, len);
    if (rc < 0) return rc;
    return (int)len;
}

int libtls_read(libtls_ctx_t *ctx, uint8_t *buf, uint32_t cap,
                uint32_t timeout_ms) {
    int rc = libtls_recv(ctx, buf, cap, timeout_ms);
    return rc;  // libtls_recv already returns -errno or bytes-read.
}
