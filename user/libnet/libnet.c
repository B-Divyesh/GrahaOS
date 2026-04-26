// user/libnet/libnet.c — Phase 22 Stage A.
//
// Thin userspace veneer over the rawnet named-channel registry. See
// libnet.h for docs.

#include "libnet.h"

#include <stdint.h>
#include <string.h>

#include "../syscalls.h"

// FNV-1a 64-bit helper — userspace mirrors the kernel's same algorithm used
// by manifest_init + the channel type-hash gate. Kept local so libnet has no
// other userspace dependencies beyond libc (memcpy) + syscalls.h.
static uint64_t libnet_fnv1a_hash64(const char *s, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;   // FNV offset basis (64-bit)
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(uint8_t)s[i];
        h *= 0x100000001b3ULL;             // FNV prime (64-bit)
    }
    return h;
}

int libnet_publish_service(libnet_service_ctx_t *ctx,
                           const char *name,
                           uint64_t payload_type_hash) {
    if (!ctx || !name) return -5;  // -EINVAL

    // Measure name length (caller gives NUL-terminated string; cap 64).
    uint32_t name_len = 0;
    while (name[name_len] && name_len <= 64) name_len++;
    if (name_len == 0 || name_len > 64) return -5;

    // Create the accept channel. Its payload type is grahaos.net.accept.v1
    // regardless of the per-connection payload type.
    uint64_t accept_hash = libnet_fnv1a_hash64("grahaos.net.accept.v1", 21);

    cap_token_u_t accept_wr = {.raw = 0};
    long rc = syscall_chan_create(accept_hash, CHAN_MODE_BLOCKING, 16, &accept_wr);
    if (rc <= 0) {
        // chan_create returns rd.raw on success; 0 or negative on failure.
        return (rc == 0) ? -3 : (int)rc;
    }
    cap_token_u_t accept_rd = {.raw = (uint64_t)rc};

    // Register the name.
    long prc = syscall_chan_publish(name, name_len, payload_type_hash,
                                    accept_wr);
    if (prc != 0) {
        // Cleanup: close accept channel's handles. With no close-handle
        // syscall wired yet, the endpoints will reap at task death, which
        // is acceptable for the failure path.
        return (int)prc;
    }

    ctx->accept_rd          = accept_rd;
    ctx->accept_wr          = accept_wr;
    ctx->payload_type_hash  = payload_type_hash;
    ctx->name_len           = name_len;
    memcpy(ctx->name, name, name_len);
    ctx->name[name_len]     = '\0';
    return 0;
}

int libnet_service_accept(libnet_service_ctx_t *ctx,
                          libnet_server_ctx_t *out,
                          uint64_t timeout_ns) {
    if (!ctx || !out) return -5;

    chan_msg_user_t msg;
    memset(&msg, 0, sizeof(msg));
    long bytes = syscall_chan_recv(ctx->accept_rd, &msg, timeout_ns);
    if (bytes == -11 /* -EAGAIN */ || bytes == -110 /* -ETIMEDOUT */) {
        return 0;   // No client right now.
    }
    if (bytes < 0) return (int)bytes;
    if (msg.header.nhandles < 2) return -5;
    if (bytes < (long)sizeof(int32_t) + (long)sizeof(uint32_t)) return -5;

    out->rd_req.raw   = msg.handles[0];
    out->wr_resp.raw  = msg.handles[1];

    // Inline payload shape mirrors kernel/net/rawnet.h rawnet_accept_info_t
    // (int32_t connector_pid, uint32_t connection_id, uint64_t _reserved).
    int32_t  cpid = 0;
    uint32_t cid  = 0;
    memcpy(&cpid, &msg.inline_payload[0], sizeof(cpid));
    memcpy(&cid,  &msg.inline_payload[4], sizeof(cid));
    out->connector_pid = cpid;
    out->connection_id = cid;
    return 1;
}

int libnet_connect_service_with_retry(const char *name,
                                      uint32_t name_len,
                                      uint32_t total_timeout_ms,
                                      libnet_client_ctx_t *out) {
    if (!name || !out || name_len == 0 || name_len > 64) return -5;

    uint32_t elapsed_ms = 0;
    uint32_t backoff_ms = 250;
    for (;;) {
        cap_token_u_t wr = {.raw = 0}, rd = {.raw = 0};
        long rc = syscall_chan_connect(name, name_len, &wr, &rd);
        if (rc == 0) {
            out->wr_req  = wr;
            out->rd_resp = rd;
            return 0;
        }
        // -EBADF from rawnet means "name not published yet" — retry.
        // Anything else is a hard failure.
        if (rc != -9 /* CAP_V2_EBADF */) {
            return (int)rc;
        }
        if (total_timeout_ms == 0 || elapsed_ms >= total_timeout_ms) {
            return (int)rc;
        }

        // Back-off sleep via syscall_yield loop (we don't have nanosleep).
        // We approximate by yielding in a tight loop; under the scheduler
        // the caller will resume ~10 ms later. Multiply yields by the
        // backoff budget.
        uint32_t wait_ms = backoff_ms;
        if (wait_ms > total_timeout_ms - elapsed_ms) {
            wait_ms = total_timeout_ms - elapsed_ms;
        }
        for (uint32_t i = 0; i < wait_ms * 100; i++) {
            // pause instruction is a decent spinlock hint; an alternative
            // here would be syscall_sleep(1ms), but the kernel doesn't
            // expose nanosecond sleeps to userspace.  Phase 22 stays
            // portable by using a busy loop; replace with sys_sleep() in
            // a later phase.
            __asm__ __volatile__("pause" ::: "memory");
        }
        elapsed_ms += wait_ms;
        if (backoff_ms < 2000) backoff_ms *= 2;
    }
}
