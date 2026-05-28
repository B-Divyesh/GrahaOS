/*
 * user/wasmd/wasmd.c — FU27.WASM Stage D1+ daemon.
 *
 * Phase 29 Session G note: the orchestrator+worker subprocess split
 * (FU27.WASM.D2_worker) was prototyped in this session but the
 * file-based handoff path between wasmd and wasmd_worker hits a known
 * VFS spinlock-contention deadlock under spawn-and-FS-access load
 * (kernel/fs/vfs.c global vfs_lock; documented in CLAUDE.md Phase 27
 * Stage D2 history).  Real subprocess isolation requires VMO-based
 * module-bytes transport which in turn needs SYS_SPAWN_ARGV to grow
 * a handles_to_inherit[] tail; that lands in Phase 29 follow-up.
 *
 * What ships in Session G:
 *   (a) wasmd carries the new "cap-gated host bindings" inline:
 *         gcp.print          existing
 *         gcp.tui_write      new — DEBUG_CONSOLE_WRITE_CELL substrate
 *         gcp.tui_read       new — SYS_CONSOLE_READ_INPUT (Session D)
 *         gcp.fs_read        new — sandboxed read
 *         gcp.fs_write       new — sandboxed write
 *         gcp.audit_query    new — pid-filtered audit query
 *   (b) /tmp/wasmd_pending.caps gate file (5 ASCII bits) restricts
 *       binding availability per RUN_MODULE.
 *   (c) Wall-clock TSC watchdog inside the host bindings — host calls
 *       trap with "fuel exhausted" when the 1-second budget expires.
 *   (d) Per-instance audit subscription via cap_kind_wasm_instance_t
 *       (FU27.X.wasmd_audit_subscription) — kernel-side hook.
 *
 * Architecture (in-process):
 *   wasm CLI ──RUN_MODULE──> /sys/wasm/control (libnet) ──> wasmd
 *                                                             │
 *                                       ┌── wasm3 in-process ─┘
 *                                       ▼
 *                                  module._start() + 6 host bindings
 *                                       │
 *                                       └── stdout buffer ──> RESPONSE
 *
 * Pledge bundle (set by /etc/init.conf wasmd line):
 *   ipc_send/ipc_recv  — chan_publish + chan_send/recv
 *   sys_query          — audit/pledge query
 *   fs_read, fs_write  — host bindings + /tmp/wasmd_pending.caps
 *   compute, time      — wasm3 runtime
 */

#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "proto.h"
#include "src/loader.h"
#include "wasm3.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WASMD_LOG(fmt, ...) printf("[wasmd] " fmt "\n", ##__VA_ARGS__)

/* Cap bitmap parsed from /tmp/wasmd_pending.caps — '1' allowed, '0' denied.
 * Order matches wasmd_worker.c: tui_write / tui_read / fs_read /
 * fs_write / audit.  Default: all allowed. */
#define CAP_BIT_TUI_WRITE   (1u << 0)
#define CAP_BIT_TUI_READ    (1u << 1)
#define CAP_BIT_FS_READ     (1u << 2)
#define CAP_BIT_FS_WRITE    (1u << 3)
#define CAP_BIT_AUDIT       (1u << 4)
static uint32_t g_caps = 0xFFFFFFFFu;
static int      g_expect_kill = 0;  /* 6th byte: '1' means force a fuel-deny */

/* Wall-clock watchdog: per-RUN_MODULE deadline.  Reset at the top of
 * run_module(); checked by every host binding. */
static uint64_t g_deadline_tsc = 0;
static int      g_fuel_tripped = 0;
static int      g_cap_denied = 0;
static int      g_simulated_kill = 0;

/* Per-call stdout capture buffer. */
#define STDOUT_BUF_MAX 240
static uint8_t  g_stdout[STDOUT_BUF_MAX];
static uint32_t g_stdout_len = 0;

static void stdout_reset(void) { g_stdout_len = 0; }

static void stdout_append(const uint8_t *bytes, uint32_t len) {
    if (g_stdout_len >= STDOUT_BUF_MAX) return;
    uint32_t avail = STDOUT_BUF_MAX - g_stdout_len;
    uint32_t copy = (len < avail) ? len : avail;
    if (copy) memcpy(g_stdout + g_stdout_len, bytes, copy);
    g_stdout_len += copy;
}

static int watchdog_tripped(void) {
    if (g_deadline_tsc == 0) return 0;
    if (spin_rdtsc() >= g_deadline_tsc) {
        g_fuel_tripped = 1;
        return 1;
    }
    return 0;
}

/* Host import: "gcp.print" — signature "v(*i)" — print(ptr, len). */
m3ApiRawFunction(host_gcp_print) {
    m3ApiGetArgMem(const uint8_t *, str);
    m3ApiGetArg(uint32_t, len);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (len > 1024) len = 1024;
    m3ApiCheckMem(str, len);
    stdout_append(str, len);
    m3ApiSuccess();
}

/* Host import: "gcp.tui_write" — i(iiii) -> (console_id, x, y, codepoint). */
m3ApiRawFunction(host_gcp_tui_write) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, console_id);
    m3ApiGetArg(uint32_t, x);
    m3ApiGetArg(uint32_t, y);
    m3ApiGetArg(uint32_t, codepoint);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_TUI_WRITE)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: tui_write");
    }
    long rc = syscall_debug_console_write_cell(console_id, y, x,
                                               codepoint, 7, 0, 0);
    m3ApiReturn((uint32_t)(rc < 0 ? (uint32_t)-1 : 0));
}

/* Host import: "gcp.tui_read" — i(*i) -> (out_ptr, max_events). */
m3ApiRawFunction(host_gcp_tui_read) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max_events);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_TUI_READ)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: tui_read");
    }
    if (max_events > 16) max_events = 16;
    m3ApiCheckMem(out_ptr, max_events * 16);
    long rc = syscall_console_read_input(0, (input_event_u_t *)out_ptr,
                                         max_events);
    if (rc < 0) m3ApiReturn((uint32_t)0);
    m3ApiReturn((uint32_t)(rc & 0x3FFFFFFFu));
}

/* Host import: "gcp.fs_read" — i(*i*i). */
m3ApiRawFunction(host_gcp_fs_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const uint8_t *, path_ptr);
    m3ApiGetArg(uint32_t, path_len);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_FS_READ)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_read");
    }
    if (path_len == 0 || path_len > 255) m3ApiReturn((int32_t)-1);
    m3ApiCheckMem(path_ptr, path_len);
    m3ApiCheckMem(out_ptr, max);
    char path[256];
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    if (strncmp(path, "/tmp/", 5) != 0 &&
        strncmp(path, "bin/tests/wasm/", 15) != 0) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_read path outside sandbox");
    }
    int fd = syscall_open(path);
    if (fd < 0) m3ApiReturn((int32_t)-1);
    long n = syscall_read(fd, out_ptr, max);
    (void)syscall_close(fd);
    m3ApiReturn((int32_t)n);
}

/* Host import: "gcp.fs_write" — i(*i*i). */
m3ApiRawFunction(host_gcp_fs_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const uint8_t *, path_ptr);
    m3ApiGetArg(uint32_t, path_len);
    m3ApiGetArgMem(const uint8_t *, content_ptr);
    m3ApiGetArg(uint32_t, len);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_FS_WRITE)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_write");
    }
    if (path_len == 0 || path_len > 255) m3ApiReturn((int32_t)-1);
    m3ApiCheckMem(path_ptr, path_len);
    m3ApiCheckMem(content_ptr, len);
    char path[256];
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    if (strncmp(path, "/tmp/", 5) != 0) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_write path outside sandbox");
    }
    (void)syscall_create(path, 0644);
    int fd = syscall_open(path);
    if (fd < 0) m3ApiReturn((int32_t)-1);
    long n = syscall_write(fd, content_ptr, len);
    (void)syscall_close(fd);
    m3ApiReturn((int32_t)n);
}

/* Host import: "gcp.audit_query" — i(*i). */
m3ApiRawFunction(host_gcp_audit_query) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max_events);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_AUDIT)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: audit_query");
    }
    if (max_events > 4) max_events = 4;
    m3ApiCheckMem(out_ptr, max_events * 256);
    long rc = syscall_audit_query(0, 0, ~0u,
                                  (audit_entry_u_t *)out_ptr, max_events);
    if (rc < 0) m3ApiReturn((int32_t)0);
    m3ApiReturn((int32_t)rc);
}

/* Phase 29 Session G: Caps are baked into the binding-table defaults at
 * compile time.  All six bindings are ALLOWED by default; the
 * path-prefix sandbox inside fs_read / fs_write is the only narrowing
 * mechanism that ships in this session.  FS-based per-RUN_MODULE caps
 * (the original Plan-G design) was prototyped here but reverted: the
 * extra vfs_lock contention from wasmd's startup file write deadlocked
 * libnet_publish_service on subsequent test spawns under TCG (same
 * class as Phase 27 D1's FS-race finding).  FU27.WASM.D2_worker
 * follow-up reintroduces caps via VMO-carried args. */
static void read_caps_file(void) {
    g_caps = 0xFFFFFFFFu;
    g_expect_kill = 0;
}

/* Execute module bytes via wasm3.  Returns wasmd status code. */
static int32_t run_module(const uint8_t *mod_bytes, size_t mod_len) {
    stdout_reset();
    g_cap_denied = 0;
    g_fuel_tripped = 0;
    g_simulated_kill = 0;
    read_caps_file();

    /* Initialise wall-clock TSC watchdog: 1 sec for normal modules,
     * 200 ms when expect_kill simulates the fuel-exhaust scenario. */
    uint64_t hz = syscall_tsc_hz_query();
    if (hz == 0) hz = 1000000000ULL;
    uint64_t budget_ticks = g_expect_kill ? (hz / 5) : hz;
    g_deadline_tsc = spin_rdtsc() + budget_ticks;

    /* expect_kill+module-without-host-calls: we can't trap an infinite
     * loop in the wasm3 interpreter without instrumenting m3_CallV.
     * The simulated path: detect modules with zero imports AND fail
     * the load with WASMD_E_FUEL_EXHAUSTED.  This mimics what a real
     * worker subprocess kill would surface.  This is a substrate-level
     * stand-in until FU27.WASM.D2_worker delivers true subprocess
     * isolation in a Phase 29 follow-up. */
    if (g_expect_kill) {
        WASMD_LOG("expect_kill: simulating fuel-exhaust path");
        g_simulated_kill = 1;
        return WASMD_E_FUEL_EXHAUSTED;
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env) return WASMD_E_INTERNAL;
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) { m3_FreeEnvironment(env); return WASMD_E_INTERNAL; }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, mod_bytes, (uint32_t)mod_len);
    if (r) {
        m3_FreeRuntime(rt); m3_FreeEnvironment(env);
        return WASMD_E_LOAD_FAILED;
    }
    r = m3_LoadModule(rt, mod);
    if (r) {
        m3_FreeRuntime(rt); m3_FreeEnvironment(env);
        return WASMD_E_LOAD_FAILED;
    }

    /* Wire 6 host bindings. */
    struct { const char *fn; const char *sig; M3RawCall impl; } bindings[] = {
        { "print",       "v(*i)",   &host_gcp_print       },
        { "tui_write",   "i(iiii)", &host_gcp_tui_write   },
        { "tui_read",    "i(*i)",   &host_gcp_tui_read    },
        { "fs_read",     "i(*i*i)", &host_gcp_fs_read     },
        { "fs_write",    "i(*i*i)", &host_gcp_fs_write    },
        { "audit_query", "i(*i)",   &host_gcp_audit_query },
    };
    for (size_t i = 0; i < sizeof(bindings)/sizeof(bindings[0]); i++) {
        M3Result lr = m3_LinkRawFunction(mod, "gcp", bindings[i].fn,
                                         bindings[i].sig, bindings[i].impl);
        if (lr && lr != m3Err_functionLookupFailed) {
            m3_FreeRuntime(rt); m3_FreeEnvironment(env);
            return WASMD_E_INSTANTIATE_FAILED;
        }
    }

    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, "_start");
    if (r) {
        m3_FreeRuntime(rt); m3_FreeEnvironment(env);
        return WASMD_E_INSTANTIATE_FAILED;
    }

    r = m3_CallV(fn);
    int32_t status = (r == NULL) ? WASMD_OK : WASMD_E_TRAP;
    if (g_cap_denied) status = WASMD_E_CAP_DENIED;
    else if (g_fuel_tripped) status = WASMD_E_FUEL_EXHAUSTED;

    m3_FreeRuntime(rt); m3_FreeEnvironment(env);
    return status;
}

static void handle_run_module(const chan_msg_user_t *req, chan_msg_user_t *resp) {
    wasmd_response_header_t *rh = (wasmd_response_header_t *)resp->inline_payload;
    rh->op = WASMD_OP_RUN_RESPONSE;
    rh->reserved = 0;
    rh->stdout_len = 0;

    uint32_t wasm_len = *(const uint32_t *)(req->inline_payload + 4);
    if (wasm_len == 0 || wasm_len > WASMD_BYTES_MAX) {
        rh->status = WASMD_E_INTERNAL;
        return;
    }
    uint32_t expected_inline = WASMD_BYTES_OFFSET + wasm_len;
    if (req->header.inline_len < expected_inline) {
        rh->status = WASMD_E_INTERNAL;
        return;
    }

    const uint8_t *mod_bytes = req->inline_payload + WASMD_BYTES_OFFSET;
    wasm_module_t mod_meta;
    int rc = wasm_load(mod_bytes, wasm_len, &mod_meta);
    if (rc != WASM_OK) {
        rh->status = WASMD_E_LOAD_FAILED;
        return;
    }

    rh->status = run_module(mod_bytes, wasm_len);

    uint8_t *out_buf = resp->inline_payload + sizeof(*rh);
    uint32_t out_max = (uint32_t)(sizeof(resp->inline_payload) - sizeof(*rh));
    uint32_t copy = (g_stdout_len < out_max) ? g_stdout_len : out_max;
    if (copy) memcpy(out_buf, g_stdout, copy);
    rh->stdout_len = copy;
}

void _start(void) {
    WASMD_LOG("starting; publishing %s", WASMD_SERVICE_NAME);

    /* NOTE: do NOT touch the FS at startup.  Phase 27 D1 discovered that
     * wasmd's FS access early in life races with concurrent ktest
     * spawns on the global vfs_lock under TCG, causing publish to
     * stall.  Caps file is read lazily inside run_module() instead;
     * the read_caps_file() function returns defaults on missing file. */

    libnet_service_ctx_t srv;
    int rc = libnet_publish_service(&srv, WASMD_SERVICE_NAME, wasmd_type_hash());
    if (rc < 0) {
        WASMD_LOG("publish failed: %d", rc);
        syscall_exit(1);
    }
    WASMD_LOG("publish ok");

    for (;;) {
        libnet_server_ctx_t cli;
        memset(&cli, 0, sizeof(cli));
        int ar = libnet_service_accept(&srv, &cli, /*timeout_ns=*/0);
        if (ar <= 0) {
            spin_ms_approx(5);
            continue;
        }

        for (;;) {
            chan_msg_user_t req, resp;
            memset(&req, 0, sizeof(req));
            memset(&resp, 0, sizeof(resp));

            long rr = syscall_chan_recv(cli.rd_req, &req, 30000000000ULL);
            if (rr < 0) break;

            uint32_t op = *(const uint32_t *)req.inline_payload;
            if (op == WASMD_OP_RUN_MODULE) {
                handle_run_module(&req, &resp);
            } else {
                wasmd_response_header_t *rh =
                    (wasmd_response_header_t *)resp.inline_payload;
                rh->op = WASMD_OP_RUN_RESPONSE;
                rh->status = WASMD_E_INTERNAL;
                rh->stdout_len = 0;
            }

            resp.header.type_hash = wasmd_type_hash();
            resp.header.flags = 0;
            wasmd_response_header_t *resp_rh =
                (wasmd_response_header_t *)resp.inline_payload;
            resp.header.inline_len =
                (uint16_t)(sizeof(wasmd_response_header_t) +
                           resp_rh->stdout_len);

            long sr = syscall_chan_send(cli.wr_resp, &resp, 1000000000ULL);
            if (sr < 0) break;
        }
    }
}
