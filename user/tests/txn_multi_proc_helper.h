// user/tests/txn_multi_proc_helper.h — Phase 29 Session H (FU25.C).
//
// Shared infrastructure for the 8 external-peer multi-process txn tests:
//   txn_replay_order, txn_abort_drops, txn_exit_cleanup, txn_commit_retry,
//   txn_concurrent_abort, txn_buffer_overflow, txn_child_abort_parent_commit,
//   txn_fault_during_replay.
//
// Pattern (argv-driven self-as-helper, matches spawn_argv.tap):
//
//   PARENT entry (argc == 0):
//     1. txn_mp_publish(name) — chan_create accept channel +
//        chan_publish(name, accept_wr).  Returns accept_rd.
//     2. syscall_spawn_argv(self_path, 2, {self_path, "child"}) — child
//        sees argc >= 2 and runs as helper.
//     3. txn_mp_accept(accept_rd) — recv the accept message; extract
//        server_rd_req + server_wr_resp (the handles to use).
//     4. ... do the txn-specific test logic ...
//     5. syscall_wait(&child_status).
//
//   CHILD entry (argc >= 2):
//     1. txn_mp_connect(name) → returns client_wr_req + client_rd_resp.
//     2. ... do the child-side test logic (recv messages or signal) ...
//     3. syscall_exit(0).
//
// All names: /test/txn-<short>  (matches chantest_named's pattern).

#ifndef TXN_MP_HELPER_H
#define TXN_MP_HELPER_H

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>

extern int printf(const char *fmt, ...);

// Well-known GCP types from kernel/ipc/manifest.c.
static inline uint64_t txn_mp_H_accept(void) { return gcp_type_hash("grahaos.net.accept.v1"); }
static inline uint64_t txn_mp_H_socket(void) { return gcp_type_hash("grahaos.net.socket.v1"); }

static inline void txn_mp_zero_msg(chan_msg_user_t *m) {
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
}

// Spawn the same binary as child with a single "child" argv token.  The
// child detects argc >= 2 in _start(argc, argv) and dispatches as helper.
// Faster + more robust than FS-sentinel role detection (FU24.A class
// FS coherence issues bite the sentinel path under load).
//
// Note on a kernel-side race: sched_spawn_process_argv sets the child's
// regs.rdi AFTER sched_spawn_process makes the task runnable.  On 4-CPU
// SMP the child can start running on another CPU before the seeding
// completes, in which case argc == 0 in the child.  Callers MUST treat
// argc==0 as "I might be a parent or a stale child" and ALSO check
// txn_mp_should_act_as_child() (chan_connect probe) for robustness.
static inline int txn_mp_spawn_child(const char *binary_path) {
    char *argv[2] = { (char *)binary_path, (char *)"child" };
    return syscall_spawn_argv(binary_path, 2, argv);
}

// Child-role probe: try to connect to CHAN_NAME with a small timeout.  If
// it succeeds, we're a helper.  If it fails, we're the parent that needs
// to publish first.
//
// Workaround for the argc==0-race-on-spawn_argv kernel bug: when argc is
// 0 we still want to honour the spawn_argv intent.  Probe-by-connect
// disambiguates without trusting argv.
static inline int txn_mp_probe_child(const char *name, uint32_t name_len,
                                     cap_token_u_t *out_wr_req,
                                     cap_token_u_t *out_rd_resp) {
    out_wr_req->raw  = 0;
    out_rd_resp->raw = 0;
    long rc = syscall_chan_connect(name, name_len, out_wr_req, out_rd_resp);
    return (rc == 0) ? 0 : -1;
}

// Parent: chan_create accept channel + chan_publish.  Returns accept_rd handle
// (read endpoint we'll recv on).  Stores accept_wr internally (kept alive
// inside the publish slot — kernel holds reference).  On error returns 0.
static inline cap_token_u_t txn_mp_publish(const char *name, uint32_t name_len) {
    cap_token_u_t accept_rd = { .raw = 0 };
    cap_token_u_t accept_wr = { .raw = 0 };
    long rc = syscall_chan_create(txn_mp_H_accept(), CHAN_MODE_BLOCKING, 8,
                                  &accept_wr);
    if (rc <= 0 || accept_wr.raw == 0) return accept_rd;
    accept_rd.raw = (uint64_t)rc;
    long prc = syscall_chan_publish(name, name_len, txn_mp_H_socket(), accept_wr);
    if (prc != 0) {
        accept_rd.raw = 0;
        return accept_rd;
    }
    return accept_rd;
}

// Parent: receive the accept message after child connects.  Extracts the
// server-side request-read endpoint (handles[0]) and response-write endpoint
// (handles[1]).  Returns 0 on success.  On failure both endpoints are zeroed.
static inline int txn_mp_accept(cap_token_u_t accept_rd,
                                cap_token_u_t *out_rd_req,
                                cap_token_u_t *out_wr_resp) {
    out_rd_req->raw  = 0;
    out_wr_resp->raw = 0;
    chan_msg_user_t amsg;
    txn_mp_zero_msg(&amsg);
    long abytes = syscall_chan_recv(accept_rd, &amsg, 5000000000ULL);
    if (abytes < 0 || amsg.header.nhandles < 2) return -1;
    out_rd_req->raw  = amsg.handles[0];
    out_wr_resp->raw = amsg.handles[1];
    return 0;
}

// Child: chan_connect to the published name; returns 0 on success.
static inline int txn_mp_connect(const char *name, uint32_t name_len,
                                 cap_token_u_t *out_wr_req,
                                 cap_token_u_t *out_rd_resp) {
    long rc = syscall_chan_connect(name, name_len, out_wr_req, out_rd_resp);
    if (rc != 0) return -1;
    return 0;
}

// Helper: send a 4-byte payload with first byte = tag.
static inline int txn_mp_send_tag(cap_token_u_t wr, uint8_t tag) {
    chan_msg_user_t m;
    txn_mp_zero_msg(&m);
    m.header.type_hash  = txn_mp_H_socket();
    m.header.inline_len = 4;
    m.inline_payload[0] = tag;
    m.inline_payload[1] = 0xAB;
    m.inline_payload[2] = 0xCD;
    m.inline_payload[3] = 0xEF;
    return (int)syscall_chan_send(wr, &m, 1000000000ULL);
}

// Helper: try to recv one message with deadline; returns >0 = bytes,
// 0 = timeout, <0 = error.
static inline long txn_mp_recv(cap_token_u_t rd, chan_msg_user_t *m,
                               uint64_t timeout_ns) {
    txn_mp_zero_msg(m);
    return syscall_chan_recv(rd, m, timeout_ns);
}

#endif  // TXN_MP_HELPER_H
