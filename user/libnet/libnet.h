// user/libnet/libnet.h — Phase 22 Stage A.
//
// Minimal userspace helpers around the new named-channel registry
// (SYS_CHAN_PUBLISH / SYS_CHAN_CONNECT backed by kernel/net/rawnet.c). Daemons
// (e1000d, netd, future storaged) use `libnet_publish_service` to register a
// named endpoint; clients (netd, user programs, gash builtins) use
// `libnet_connect_service_with_retry` to attach with exponential back-off
// while the publisher is still starting up.
//
// Phase 22 Stage C will grow this file with the full GCP message-type layer
// (net_query_req/resp, tcp_open_req/resp, dns_query_req/resp, etc.). Stage A
// keeps it intentionally tiny.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../syscalls.h"

// Phase 22 reserved names. Mirrors kernel/net/rawnet.h constants so user
// code doesn't depend on the kernel header directly.
#define LIBNET_NAME_RAWFRAME   "/sys/net/rawframe"
#define LIBNET_NAME_SERVICE    "/sys/net/service"

// Per-connection ring capacity. Matches kernel RAWNET_CONN_CAPACITY.
#define LIBNET_CONN_CAPACITY   64u

// ---------------------------------------------------------------------------
// Per-service context held by a publisher. Opaque to callers outside libnet.
//
// Layout:
//   accept_rd : read-end of the accept channel the kernel writes to on each
//               successful connect. The publisher drains this in its accept
//               loop.
//   accept_wr : write-end handle passed to the kernel via SYS_CHAN_PUBLISH.
//               Kept alive for the lifetime of the publisher so the
//               registry's cached pointer remains valid.
// ---------------------------------------------------------------------------
typedef struct libnet_service_ctx {
    cap_token_u_t  accept_rd;
    cap_token_u_t  accept_wr;
    uint64_t       payload_type_hash;
    char           name[65];   // NUL-terminated copy of the published name
    uint32_t       name_len;
} libnet_service_ctx_t;

// Per-connection client context returned by libnet_connect_service.
typedef struct libnet_client_ctx {
    cap_token_u_t wr_req;   // client -> server direction (write end)
    cap_token_u_t rd_resp;  // server -> client direction (read end)
} libnet_client_ctx_t;

// Per-connection server-side context, produced by libnet_service_accept when
// the publisher drains an accept message. Analogue of Unix accept(2).
typedef struct libnet_server_ctx {
    cap_token_u_t rd_req;   // client -> server (read end)
    cap_token_u_t wr_resp;  // server -> client (write end)
    int32_t       connector_pid;
    uint32_t      connection_id;
} libnet_server_ctx_t;

// ---------------------------------------------------------------------------
// Publisher-side API.
// ---------------------------------------------------------------------------

// Create the accept channel and register the given name. Returns 0 on
// success (populates *ctx); on failure returns a negative kernel errno and
// leaves *ctx untouched.
//
//   name               — e.g. "/sys/net/rawframe". Must start with '/'
//                        and be 1..64 bytes.
//   payload_type_hash  — FNV-1a hash of the type name each per-connection
//                        channel will carry (e.g., grahaos.net.frame.v1).
int libnet_publish_service(libnet_service_ctx_t *ctx,
                           const char *name,
                           uint64_t payload_type_hash);

// Non-blocking poll of the accept channel. Returns 1 if a new client was
// accepted (populates *out), 0 on "no client yet", negative errno on
// kernel failure.
int libnet_service_accept(libnet_service_ctx_t *ctx,
                          libnet_server_ctx_t *out,
                          uint64_t timeout_ns);

// ---------------------------------------------------------------------------
// Client-side API.
// ---------------------------------------------------------------------------

// Attempt a SYS_CHAN_CONNECT with exponential back-off (500 ms → 1 s → 2 s
// → 4 s, capped at the total_timeout_ms ceiling). This is the primary tool
// daemons and libraries use to rendezvous with a publisher whose boot path
// may race with our own.
//
// Returns 0 on success (populates *out), negative kernel errno otherwise.
//   total_timeout_ms = 0 falls back to a single attempt (no retry).
int libnet_connect_service_with_retry(const char *name,
                                      uint32_t name_len,
                                      uint32_t total_timeout_ms,
                                      libnet_client_ctx_t *out);
