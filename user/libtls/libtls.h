// user/libtls/libtls.h — Phase 29 Session B (FU28.A).
//
// Userspace TLS 1.2/1.3 client glue.  Phase 29 introduces this library as
// a structural replacement for the Phase 22 libtls-mg (Mongoose-derived)
// path: same public API, but the future implementation links against
// BearSSL (MIT licensed; vendored under vendor/bearssl/).
//
// Layering:
//
//     application  -> libhttp::http_post(https://...)
//                  -> libtls_connect / write / read / close
//                  -> BearSSL (vendor/bearssl/)
//                  -> libnet TCP socket (libnet_tcp_*)
//                  -> netd
//
// libtls does NOT speak to the kernel directly; it reuses libnet's TCP
// socket layer.  All crypto + record-layer state stays in userspace.
//
// Substrate-only landing (Phase 29 Session B):
//   - vendor/bearssl/ is in tree (real source; run scripts/vendor-bearssl.sh
//     to refresh).
//   - This shim returns -ENOSYS until the BearSSL wiring lands.  libhttp
//     keeps calling the Phase 22 libtls-mg implementation in the meantime
//     so grahai HTTPS to Gemini continues to work.
//   - When BearSSL wiring lands, the linker will resolve libtls_*
//     symbols against this archive in preference to libtls-mg.a.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../libnet/libnet.h"

// Path of the PEM-encoded trust store consulted by libtls_init().  Maintained
// by scripts/fetch-trust-pem.sh.  Default is /etc/tls/trust.pem (initrd).
#define LIBTLS_TRUST_STORE   "/etc/tls/trust.pem"

// Maximum SNI hostname length the implementation will accept (RFC 1035 limits
// hostnames to 253 bytes; we round up).
#define LIBTLS_MAX_SNI_LEN   255u

// Opaque per-session context.  Allocated by libtls_connect, freed by
// libtls_close.  Carries the BearSSL engine state + libnet socket handle.
typedef struct libtls_ctx libtls_ctx_t;

// ---------------------------------------------------------------------------
// Phase 29 Session B public API.
//
// libtls_init() — load a PEM trust store from disk into the per-process
// CA list.  Idempotent: re-invocations replace the loaded list.  Returns
// the number of certificates loaded on success, or a negative kernel errno
// (-ENOENT/-EIO/-EINVAL) on failure.
//
// If `ca_bundle_path` is NULL the default LIBTLS_TRUST_STORE path is used.
//
// libtls_connect() — drive the TLS handshake on top of an already-connected
// libnet TCP socket.  On success populates *out_ctx and returns 0.  The
// caller owns the ctx and must call libtls_close to release it.
//
// libtls_send() / libtls_recv() — record-layer wrappers that encrypt /
// decrypt application data.  Both reuse the underlying libnet TCP socket
// for transport and return bytes processed on success or a negative
// errno on failure.
//
// libtls_close() — graceful close_notify + teardown.  Safe to call with
// NULL.
//
// Substrate landing returns:
//   libtls_init      -> -38 (-ENOSYS)
//   libtls_connect   -> -38
//   libtls_send/recv -> -107 (-ENOTCONN)
//   libtls_close     -> no-op
// ---------------------------------------------------------------------------

int      libtls_init(const char *ca_bundle_path);

// libtls_connect — argument order MUST match libhttp.c's weak declaration
// (nc, tcp_cookie, sni, out_ctx) so the strong symbol in libtls.a resolves
// libhttp's weak reference without touching libhttp.  On success populates
// *out_ctx with a heap-allocated session handle and returns 0; on failure
// returns a negative errno and leaves *out_ctx NULL.
int      libtls_connect(libnet_client_ctx_t *nc, uint32_t tcp_cookie,
                        const char *hostname, libtls_ctx_t **out_ctx);

int      libtls_send(libtls_ctx_t *ctx, const void *buf, size_t len);

int      libtls_recv(libtls_ctx_t *ctx, void *buf, size_t len,
                     uint32_t timeout_ms);

void     libtls_close(libtls_ctx_t *ctx);

// ---------------------------------------------------------------------------
// libhttp compatibility shims.
//
// libhttp.c calls the Phase 22 names (libtls_write / libtls_read with the
// (nc, cookie) pair packed into the ctx).  Until libhttp is swapped to the
// new API, expose the legacy names as thin wrappers so the archive can be
// linked in interchangeably with libtls-mg.a.
//
// NOTE: The legacy connect signature is preserved as `libtls_connect_legacy`
// so callers can compile against either header.  libtls-mg.a still owns the
// linker symbol named `libtls_connect`; libhttp uses weak references so the
// linker picks whichever archive ships first.
// ---------------------------------------------------------------------------

int      libtls_write(libtls_ctx_t *ctx, const uint8_t *buf, uint32_t len);
int      libtls_read(libtls_ctx_t *ctx, uint8_t *buf, uint32_t cap,
                     uint32_t timeout_ms);

// Returns 1 if the underlying BearSSL backend is available (vendored +
// linked), 0 otherwise.  Tests use this to skip cleanly when libtls is
// stubbed.
int      libtls_backend_available(void);
