// user/libtls-mg/libtls_shim.c — Phase 22 Stage D U18.
//
// The shim adapts Mongoose's TLS core (extracted into mongoose_tls_core.c
// by `user/libtls-mg/extract.py`) onto libnet's TCP primitives. The
// Mongoose TLS driver assumes a `struct mg_connection` containing two
// `mg_iobuf` members (`send` and `rtls`) plus a handful of state flags.
// The shim provides a minimal `mg_connection` stand-in and funnels
// mg_tls_send / mg_tls_recv through libnet_tcp_send / libnet_tcp_recv.
//
// DELIVERY NOTE: this file is the architectural scaffolding. It compiles
// cleanly in isolation today; once the Mongoose extraction lands (see
// extract.py), the weak-linkage `_mg_*` symbols resolve to the extracted
// core and the shim drives real TLS handshakes. Until then, `libtls_connect`
// returns -ENOSYS so libhttp's HTTPS path falls back to the same error code
// it emits when libtls-mg.a is absent entirely.

#include "libtls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"

// ---------------------------------------------------------------------------
// Minimal mg_iobuf replica — drained by the shim after every handshake
// flight to push encrypted bytes onto the libnet TCP socket; filled by
// the shim from libnet_tcp_recv before each extraction-code recv call.
// ---------------------------------------------------------------------------
typedef struct mg_iobuf {
    uint8_t  *buf;
    uint32_t  len;     // Bytes currently in buf
    uint32_t  size;    // Allocated bytes (>= len)
    uint32_t  align;
} mg_iobuf_t;

static int iobuf_reserve(mg_iobuf_t *io, uint32_t need) {
    if (io->size >= need) return 0;
    uint32_t new_size = io->size ? io->size * 2u : 4096u;
    while (new_size < need) new_size *= 2u;
    uint8_t *nb = (uint8_t *)malloc(new_size);
    if (!nb) return -12 /* ENOMEM */;
    if (io->len) memcpy(nb, io->buf, io->len);
    if (io->buf) free(io->buf);
    io->buf  = nb;
    io->size = new_size;
    return 0;
}

static void iobuf_free(mg_iobuf_t *io) {
    if (io->buf) free(io->buf);
    io->buf = NULL;
    io->len = 0;
    io->size = 0;
}

static int iobuf_append(mg_iobuf_t *io, const uint8_t *src, uint32_t n) {
    if (iobuf_reserve(io, io->len + n) < 0) return -12;
    if (n) memcpy(io->buf + io->len, src, n);
    io->len += n;
    return 0;
}

static uint32_t iobuf_consume(mg_iobuf_t *io, uint32_t n) {
    if (n >= io->len) { uint32_t had = io->len; io->len = 0; return had; }
    memmove(io->buf, io->buf + n, io->len - n);
    io->len -= n;
    return n;
}

// ---------------------------------------------------------------------------
// libtls_ctx — opaque handle paired with libtls.h.
// ---------------------------------------------------------------------------
struct libtls_ctx {
    libnet_client_ctx_t *nc;
    uint32_t            cookie;       // libnet TCP socket cookie
    uint8_t             handshake_done;
    uint8_t             peer_closed;
    uint8_t             _pad[2];
    char                sni[LIBTLS_MAX_SNI_LEN + 1];
    mg_iobuf_t          enc_out;      // Encrypted bytes queued for TCP send
    mg_iobuf_t          enc_in;       // Encrypted bytes freshly read from TCP
    mg_iobuf_t          app_in;       // Decrypted app data ready for libtls_read
};

// ---------------------------------------------------------------------------
// Weak hooks exposed by the Mongoose extraction. When mongoose_tls_core.c
// is present, these resolve to the real extracted handshake driver. In
// the current "scaffolding-only" landing they stay NULL and libtls_connect
// errors out with -ENOSYS.
//
// Contract:
//   _mgtls_drive_handshake(ctx)
//     * Feed encrypted bytes from ctx->enc_in into the extracted state
//       machine; the extracted code appends to ctx->enc_out.
//     * Return 0 on completion, -EINPROGRESS while more flights are needed,
//       negative errno on failure.
//   _mgtls_encrypt_app(ctx, data, len)
//     * Encrypt `len` bytes, append cipher to ctx->enc_out.
//   _mgtls_decrypt_app(ctx)
//     * Consume ctx->enc_in records, append cleartext to ctx->app_in.
// ---------------------------------------------------------------------------
__attribute__((weak))
int _mgtls_drive_handshake(struct libtls_ctx *ctx);

__attribute__((weak))
int _mgtls_encrypt_app(struct libtls_ctx *ctx,
                       const uint8_t *data, uint32_t len);

__attribute__((weak))
int _mgtls_decrypt_app(struct libtls_ctx *ctx);

// ---------------------------------------------------------------------------
// TCP plumbing — every send/recv from the extracted core funnels through
// these helpers so the shim can see and log the raw TLS byte stream.
// ---------------------------------------------------------------------------
static int flush_encrypted(struct libtls_ctx *ctx) {
    if (ctx->enc_out.len == 0) return 0;
    uint32_t offset = 0;
    while (offset < ctx->enc_out.len) {
        uint32_t chunk = ctx->enc_out.len - offset;
        if (chunk > LIBNET_TCP_CHUNK_MAX) chunk = LIBNET_TCP_CHUNK_MAX;
        uint32_t sent = 0;
        int rc = libnet_tcp_send(ctx->nc, ctx->cookie,
                                 ctx->enc_out.buf + offset, (uint16_t)chunk,
                                 2000000000ULL, &sent);
        if (rc < 0) return rc;
        if (sent == 0) return -32 /* EPIPE */;
        offset += sent;
    }
    (void)iobuf_consume(&ctx->enc_out, offset);
    return 0;
}

static int pull_encrypted(struct libtls_ctx *ctx, uint32_t timeout_ms) {
    uint8_t  tmp[LIBNET_TCP_CHUNK_MAX];
    uint16_t got = 0;
    uint16_t flags = 0;
    int rc = libnet_tcp_recv(ctx->nc, ctx->cookie, tmp, sizeof(tmp),
                             timeout_ms, &got, &flags);
    if (rc == -32 /* EPIPE */) { ctx->peer_closed = 1; return 0; }
    if (rc == -11 || rc == -110) return 0;   // EAGAIN / ETIMEDOUT
    if (rc < 0) return rc;
    if (flags & 1u) ctx->peer_closed = 1;
    if (got == 0) return 0;
    return iobuf_append(&ctx->enc_in, tmp, got);
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------
int libtls_connect(libnet_client_ctx_t *nc, uint32_t tcp_cookie,
                   const char *sni, struct libtls_ctx **out_ctx) {
    if (!nc || !sni || !out_ctx) return -5;
    if (!_mgtls_drive_handshake) {
        // Extraction not yet landed — caller should treat as "no TLS".
        return -6 /* ENOSYS */;
    }

    struct libtls_ctx *ctx = (struct libtls_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return -12;
    memset(ctx, 0, sizeof(*ctx));
    ctx->nc     = nc;
    ctx->cookie = tcp_cookie;
    size_t sni_len = strlen(sni);
    if (sni_len > LIBTLS_MAX_SNI_LEN) sni_len = LIBTLS_MAX_SNI_LEN;
    memcpy(ctx->sni, sni, sni_len);
    ctx->sni[sni_len] = '\0';

    // Pump the handshake state machine to completion. Each iteration:
    //   1. Drive the extracted core with any new encrypted bytes.
    //   2. Flush any TLS records it appended to enc_out.
    //   3. Pull more encrypted bytes from the socket if the core isn't done.
    for (int iter = 0; iter < 32; iter++) {
        int rc = _mgtls_drive_handshake(ctx);
        int fl = flush_encrypted(ctx);
        if (fl < 0) { libtls_close(ctx); return fl; }
        if (rc == 0) {
            ctx->handshake_done = 1;
            *out_ctx = ctx;
            return 0;
        }
        if (rc != -115 /* -EINPROGRESS */) {
            libtls_close(ctx);
            return rc;
        }
        int pr = pull_encrypted(ctx, 500u);
        if (pr < 0) { libtls_close(ctx); return pr; }
        if (ctx->peer_closed) { libtls_close(ctx); return -32; }
    }
    libtls_close(ctx);
    return -110 /* ETIMEDOUT */;
}

int libtls_write(struct libtls_ctx *ctx, const uint8_t *buf, uint32_t len) {
    if (!ctx || !buf) return -5;
    if (!ctx->handshake_done) return -107 /* ENOTCONN */;
    if (!_mgtls_encrypt_app) return -6;
    int rc = _mgtls_encrypt_app(ctx, buf, len);
    if (rc < 0) return rc;
    int fl = flush_encrypted(ctx);
    if (fl < 0) return fl;
    return (int)len;
}

int libtls_read(struct libtls_ctx *ctx, uint8_t *buf, uint32_t cap,
                uint32_t timeout_ms) {
    if (!ctx || !buf) return -5;
    if (!ctx->handshake_done) return -107;
    if (!_mgtls_decrypt_app) return -6;

    // Satisfy from already-decrypted cleartext first.
    if (ctx->app_in.len > 0) {
        uint32_t take = ctx->app_in.len < cap ? ctx->app_in.len : cap;
        memcpy(buf, ctx->app_in.buf, take);
        (void)iobuf_consume(&ctx->app_in, take);
        return (int)take;
    }
    if (ctx->peer_closed) return 0;

    // Pull encrypted bytes and drive the decryptor.
    int pr = pull_encrypted(ctx, timeout_ms);
    if (pr < 0) return pr;
    int dr = _mgtls_decrypt_app(ctx);
    if (dr < 0) return dr;

    if (ctx->app_in.len == 0) return 0;
    uint32_t take = ctx->app_in.len < cap ? ctx->app_in.len : cap;
    memcpy(buf, ctx->app_in.buf, take);
    (void)iobuf_consume(&ctx->app_in, take);
    return (int)take;
}

void libtls_close(struct libtls_ctx *ctx) {
    if (!ctx) return;
    iobuf_free(&ctx->enc_out);
    iobuf_free(&ctx->enc_in);
    iobuf_free(&ctx->app_in);
    free(ctx);
}

int libtls_load_trust_store(const char *path) {
    // Stub: returns 0. Real implementation parses PEM certs into Mongoose's
    // cert-chain verification tables via the extracted X.509 code. Deferred.
    (void)path;
    return 0;
}
