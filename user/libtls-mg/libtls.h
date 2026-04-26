// user/libtls-mg/libtls.h — Phase 22 Stage D U18/U19.
//
// Thin userspace TLS 1.3 client wrapper. The Stage D implementation is
// backed by the vendored Mongoose TLS subtree extracted from
// `kernel/net/mongoose.c` via `user/libtls-mg/extract.py`; the shim in
// libtls_shim.c adapts Mongoose's `mg_connection`-shaped context onto a
// libnet TCP socket so the extracted code can drive sessions without
// pulling in Mongoose's event loop.
//
// Callers don't see anything Mongoose-flavoured — the API exposes
// `struct libtls_ctx` as an opaque handle. libhttp links weakly against
// libtls_connect/read/write/close so plaintext HTTP/1.1 works even when
// libtls-mg.a is absent (HTTPS paths return -EPROTO in that mode).
//
// ROADMAP NOTE: the initial landing ships the public API + shim skeleton +
// extraction script. The extracted `mongoose_tls_core.c` is deferred to a
// dedicated follow-up session because safely vendoring ~8 kLOC of Mongoose
// TLS + re-wiring its `mg_iobuf` dependencies is a several-hour exercise
// that needs compiler-driven iteration. Until then, `libtls_connect`
// returns -ENOSYS so aitest/grahai HTTPS gates are documented as manual-
// verification rather than fully automated.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../libnet/libnet.h"

#define LIBTLS_MAX_SNI_LEN   255u
#define LIBTLS_TRUST_STORE   "/etc/tls/trust.pem"

struct libtls_ctx;   // Opaque. Defined in libtls_shim.c.

// Establish a TLS 1.3 session over an existing libnet TCP socket.
//   nc         : already-connected libnet service channel (for tcp_send/recv)
//   tcp_cookie : cookie returned by libnet_tcp_open
//   sni        : NUL-terminated Server Name Indication (cert CN match)
// On success populates *out_ctx and returns 0. Caller owns the ctx and must
// call libtls_close to release it.
int libtls_connect(libnet_client_ctx_t *nc, uint32_t tcp_cookie,
                   const char *sni, struct libtls_ctx **out_ctx);

// Push cleartext bytes through the TLS encryptor. Returns bytes written on
// success, negative errno otherwise.
int libtls_write(struct libtls_ctx *ctx, const uint8_t *buf, uint32_t len);

// Pull cleartext bytes up to `cap`. Returns bytes read (0 = no data yet,
// < 0 = error). Blocks up to `timeout_ms` (0 = non-blocking).
int libtls_read(struct libtls_ctx *ctx, uint8_t *buf, uint32_t cap,
                uint32_t timeout_ms);

// Tear down the session. Sends TLS close_notify if the context is alive.
// Safe to call with NULL.
void libtls_close(struct libtls_ctx *ctx);

// Load the PEM-encoded trust store from `path`. Returns the number of
// certificates loaded on success, negative errno on I/O or parse error.
// Callers that skip this step default to the system bundle at
// LIBTLS_TRUST_STORE (loaded lazily on first libtls_connect).
int libtls_load_trust_store(const char *path);
