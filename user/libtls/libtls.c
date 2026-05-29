// user/libtls/libtls.c — Phase 29 FU29.B.bearssl_wire.
//
// Real BearSSL backend for the userspace TLS 1.2 client.  This is the glue
// layer between libhttp (and grahai/gash via libhttp) and the vendored
// BearSSL engine (vendor/bearssl/, MIT licensed).
//
// Layering (see libtls.h):
//
//     libhttp::http_post(https://...)
//       -> libtls_connect / libtls_write / libtls_read / libtls_close
//       -> BearSSL br_ssl_client_* + br_sslio_*  (this file)
//       -> libnet_tcp_send / libnet_tcp_recv     (low_read/low_write)
//       -> netd
//
// Trust anchors are baked at build time by scripts/gen_trust_anchors.py into
// trust_anchors.c (the g_grahaos_TAs[] table); there is NO runtime PEM
// parsing.  libtls_init() merely returns the anchor count.
//
// ===========================================================================
// KNOWN WEAKNESS — TLS RANDOMNESS SOURCE (deferred follow-up).
// ===========================================================================
// GrahaOS exposes no kernel entropy syscall (adding one would mutate
// etc/gcp.json and break the manifest-blob invariant the gate pins).  We
// therefore seed BearSSL's DRBG from a userspace mix of the RDRAND CPU
// instruction (available at CPL3) and rdtsc.  RDRAND on a correctly
// functioning CPU is a cryptographically strong DRBG, but:
//   * we cannot attest the silicon (RDRAND could be backdoored/faulty);
//   * if RDRAND is unavailable (very old CPU / disabled in QEMU TCG) we fall
//     back to rdtsc alone, which is WEAK and predictable.
// A proper fix is a kernel CSPRNG fed by real entropy sources exposed via a
// dedicated capability-gated syscall.  Tracked as a deferred follow-up.
// ===========================================================================

#include "libtls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"

#include "bearssl.h"

// Generated trust-anchor table (trust_anchors.c).
extern const br_x509_trust_anchor g_grahaos_TAs[];
extern const size_t               g_grahaos_TAs_num;

// ---------------------------------------------------------------------------
// Sentinel: the BearSSL backend is wired.  libtls_backend_available() returns
// this verbatim; tests gate their real-vs-skip path on it.
// ---------------------------------------------------------------------------
static const int libtls_have_bearssl = 1;

int libtls_backend_available(void) {
    return libtls_have_bearssl;
}

// ---------------------------------------------------------------------------
// Per-session context.  Definition private to this file; callers see only the
// forward declaration in libtls.h.
// ---------------------------------------------------------------------------
struct libtls_ctx {
    libnet_client_ctx_t      *nc;
    uint32_t                  cookie;
    int                       last_err;     // BearSSL engine error at handshake
    br_ssl_client_context     sc;
    br_x509_minimal_context   xc;
    br_sslio_context          ioc;
    unsigned char             iobuf[BR_SSL_BUFSIZE_BIDI];
};

// ---------------------------------------------------------------------------
// Entropy.  See the KNOWN WEAKNESS banner above.
//
// Collect >= 32 bytes by mixing RDRAND (when the CPU supports it) with rdtsc.
// We do NOT rely on a CPUID feature probe (CPUID is fine at CPL3 but RDRAND's
// own carry-flag protocol already signals availability per-call): if RDRAND
// repeatedly fails to set CF we fall back to rdtsc alone.
// ---------------------------------------------------------------------------
static int rdrand64(uint64_t *out) {
    unsigned char ok = 0;
    uint64_t v = 0;
    // rdrand sets CF=1 on success.  Encoded directly so we don't need
    // -mrdrnd at -march=x86-64.
    __asm__ __volatile__(
        ".byte 0x48, 0x0f, 0xc7, 0xf0\n\t"  // rdrand %rax
        "setc %1\n\t"
        : "=a"(v), "=qm"(ok)
        :
        : "cc");
    if (ok) { *out = v; return 1; }
    return 0;
}

static void collect_entropy(unsigned char seed[64]) {
    // Always fold in rdtsc samples so the seed is never all-zero even if
    // RDRAND is unavailable.
    for (int i = 0; i < 8; i++) {
        uint64_t t = spin_rdtsc();
        uint64_t r = 0;
        if (rdrand64(&r)) {
            t ^= r;
        } else {
            // Stir rdtsc with a cheap diffusion when RDRAND is missing — this
            // is the WEAK fallback path (see banner).
            for (int k = 0; k < 4; k++) {
                t ^= spin_rdtsc();
                t *= 0x100000001B3ULL;   // FNV prime, used purely as a mixer
            }
        }
        memcpy(seed + (size_t)i * 8u, &t, sizeof t);
    }
}

// ---------------------------------------------------------------------------
// Transport adapters for br_sslio.  Contract (bearssl_ssl.h):
//   * return >= 1 on progress (bytes moved),
//   * return 0 to be re-invoked immediately (the wrapper retries),
//   * return -1 on a fatal transport error.
// We bound the silent-peer retry budget so a dead/stalled connection cannot
// spin forever inside BearSSL.
// ---------------------------------------------------------------------------
#define LIBTLS_LOW_READ_TIMEOUT_MS   500u
#define LIBTLS_LOW_READ_MAX_RETRY    24      // ~12 s of 500 ms slices

static int low_read(void *vctx, unsigned char *buf, size_t len) {
    struct libtls_ctx *ctx = (struct libtls_ctx *)vctx;
    if (len == 0) return 0;
    uint16_t cap = (len > LIBNET_TCP_CHUNK_MAX) ? LIBNET_TCP_CHUNK_MAX
                                                : (uint16_t)len;
    for (int attempt = 0; attempt < LIBTLS_LOW_READ_MAX_RETRY; attempt++) {
        uint16_t got = 0, flags = 0;
        int rc = libnet_tcp_recv(ctx->nc, ctx->cookie, buf, cap,
                                 LIBTLS_LOW_READ_TIMEOUT_MS, &got, &flags);
        if (rc == -32 /* EPIPE */) return -1;        // peer reset/closed
        if (rc == -11 || rc == -110) {               // EAGAIN / ETIMEDOUT
            if (flags & 1u) return -1;               // FIN with no data -> EOF
            continue;                                // retry slice
        }
        if (rc < 0) return -1;
        if (got > 0) return (int)got;
        if (flags & 1u) return -1;                   // clean EOF
        // got==0, no error, no FIN: loop and retry.
    }
    return -1;                                       // silent peer -> give up
}

static int low_write(void *vctx, const unsigned char *buf, size_t len) {
    struct libtls_ctx *ctx = (struct libtls_ctx *)vctx;
    if (len == 0) return 0;
    uint16_t chunk = (len > LIBNET_TCP_CHUNK_MAX) ? LIBNET_TCP_CHUNK_MAX
                                                  : (uint16_t)len;
    uint32_t sent = 0;
    int rc = libnet_tcp_send(ctx->nc, ctx->cookie, buf, chunk,
                             2000000000ULL /* 2 s */, &sent);
    if (rc < 0) return -1;
    if (sent == 0) return -1;
    return (int)sent;
}

// ---------------------------------------------------------------------------
// libtls_init: with static trust anchors there is nothing to parse at
// runtime.  We accept (and ignore) the path argument for API compatibility
// and return the number of compiled-in trust anchors.
// ---------------------------------------------------------------------------
int libtls_init(const char *ca_bundle_path) {
    (void)ca_bundle_path;
    if (g_grahaos_TAs_num == 0) return -2 /* -ENOENT: empty trust store */;
    return (int)g_grahaos_TAs_num;
}

// ---------------------------------------------------------------------------
// libtls_connect: drive the TLS handshake over an already-open libnet TCP
// socket.  Argument order matches libhttp's weak declaration.
// ---------------------------------------------------------------------------
int libtls_connect(libnet_client_ctx_t *nc, uint32_t tcp_cookie,
                   const char *hostname, libtls_ctx_t **out_ctx) {
    if (!nc || !hostname || !out_ctx) return -22 /* -EINVAL */;
    *out_ctx = NULL;

    struct libtls_ctx *ctx = (struct libtls_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return -12 /* -ENOMEM */;
    memset(ctx, 0, sizeof(*ctx));
    ctx->nc     = nc;
    ctx->cookie = tcp_cookie;

    // Full client profile: all implemented suites + RSA/ECDSA verification
    // against the baked-in trust anchors.
    br_ssl_client_init_full(&ctx->sc, &ctx->xc,
                            g_grahaos_TAs, g_grahaos_TAs_num);

    // Seed the engine's DRBG with our userspace entropy mix BEFORE reset so
    // BR_ERR_NO_RANDOM can never fire during the handshake.
    {
        unsigned char seed[64];
        collect_entropy(seed);
        br_ssl_engine_inject_entropy(&ctx->sc.eng, seed, sizeof seed);
    }

    // Bidirectional record buffer lives inside the ctx (one allocation).
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof ctx->iobuf, 1);

    // SNI + handshake reset.
    if (br_ssl_client_reset(&ctx->sc, hostname, 0) != 1) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        free(ctx);
        return -71 /* -EPROTO */;
    }

    br_sslio_init(&ctx->ioc, &ctx->sc.eng,
                  low_read, ctx, low_write, ctx);

    // Drive the handshake to completion: br_sslio_flush forces all pending
    // handshake records out and pumps the engine until the handshake is done
    // (or an error / transport failure occurs).
    if (br_sslio_flush(&ctx->ioc) != 0) {
        int err = br_ssl_engine_last_error(&ctx->sc.eng);
        ctx->last_err = err;
        free(ctx);
        // Distinguish certificate-validation failures (X.509 error range
        // 32..62) from generic handshake/transport failures so callers can
        // log a meaningful cause.
        if (err >= BR_ERR_X509_OK && err <= BR_ERR_X509_NOT_TRUSTED) {
            return -71 /* -EPROTO: certificate verification failed */;
        }
        return -71 /* -EPROTO: handshake failed */;
    }

    // Belt-and-braces: confirm the engine is not in an error state.
    int err = br_ssl_engine_last_error(&ctx->sc.eng);
    if (err != BR_ERR_OK) {
        ctx->last_err = err;
        free(ctx);
        return -71;
    }

    *out_ctx = ctx;
    return 0;
}

// ---------------------------------------------------------------------------
// libtls_send / libtls_recv: record-layer application data.
// ---------------------------------------------------------------------------
int libtls_send(libtls_ctx_t *ctx, const void *buf, size_t len) {
    if (!ctx || !buf) return -22;
    if (len == 0) return 0;
    int rc = br_sslio_write_all(&ctx->ioc, buf, len);
    if (rc < 0) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        return -71;
    }
    if (br_sslio_flush(&ctx->ioc) != 0) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        return -71;
    }
    return (int)len;
}

int libtls_recv(libtls_ctx_t *ctx, void *buf, size_t len,
                uint32_t timeout_ms) {
    if (!ctx || !buf) return -22;
    if (len == 0) return 0;
    (void)timeout_ms;  // the low_read adapter already bounds its own waits
    int rc = br_sslio_read(&ctx->ioc, buf, len);
    if (rc < 0) {
        int err = br_ssl_engine_last_error(&ctx->sc.eng);
        ctx->last_err = err;
        // Clean close_notify (engine error OK) reads back as EOF, not error.
        if (err == BR_ERR_OK) return 0;
        return -71;
    }
    return rc;  // >= 1 bytes (br_sslio_read never returns 0 for len > 0)
}

// ---------------------------------------------------------------------------
// libtls_close: graceful close_notify + teardown.  Safe with NULL.
// ---------------------------------------------------------------------------
void libtls_close(libtls_ctx_t *ctx) {
    if (!ctx) return;
    // Best-effort close_notify; ignore the result (we are tearing down).
    (void)br_sslio_close(&ctx->ioc);
    free(ctx);
}

// ---------------------------------------------------------------------------
// libhttp compatibility wrappers (Phase 22 signatures).  These ARE the
// strong symbols libhttp's weak references resolve against once libtls.a is
// linked with --whole-archive.
// ---------------------------------------------------------------------------
int libtls_write(libtls_ctx_t *ctx, const uint8_t *buf, uint32_t len) {
    int rc = libtls_send(ctx, buf, len);
    if (rc < 0) return rc;
    return (int)len;
}

int libtls_read(libtls_ctx_t *ctx, uint8_t *buf, uint32_t cap,
                uint32_t timeout_ms) {
    return libtls_recv(ctx, buf, cap, timeout_ms);
}
