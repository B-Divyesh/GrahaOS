/*
 * user/wasmd/wasmd.c — FU27.WASM Stage D1 daemon main loop.
 *
 * Architecture (V1, simplified):
 *   wasm CLI ──RUN_MODULE──> /sys/wasm/control (libnet) ──> wasmd
 *                                                             │
 *                                       ┌── wasm3 in-process ─┘
 *                                       ▼
 *                                  module._start()
 *                                       │
 *                                       └── stdout buffer ──> RESPONSE
 *
 * V1 (this stage) runs wasm3 inside wasmd directly. Fault isolation via
 * PLEDGE_FLAG_NARROW_EXEC (Phase 26 substrate, gate-tested 5/5 in
 * pledge_narrow_exec.tap) is exercised by Stage D2 fault tests when they
 * land. The trade-off is that a wasm trap in v1 takes wasmd down with
 * it; D2 will reintroduce wasmd_worker as a separate subprocess so a
 * trap stays inside the worker.
 *
 * Pledge bundle (set by /etc/init.conf wasmd line):
 *   ipc_send/ipc_recv  — chan_publish + chan_send/recv
 *   sys_query          — audit/pledge query
 *   fs_read            — load module bytes
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

/* Per-call stdout capture buffer. wasm3 module's "gcp.print" host import
 * appends here. Sized to fit the channel response payload (256 - 16-byte
 * header = 240 bytes max). The buffer is reset at the start of each
 * RUN_MODULE; in-process v1 means each invocation is serialized so there
 * is no concurrent-writer concern. */
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

/* Host import: "gcp.print" — signature "v(*i)" — print(ptr, len). */
m3ApiRawFunction(host_gcp_print) {
    m3ApiGetArgMem(const uint8_t *, str);
    m3ApiGetArg(uint32_t, len);
    WASMD_LOG("host_gcp_print: str=%p len=%u mem=%p", (void *)str, len, _mem);
    if (len > 1024) len = 1024;  /* sanity cap */
    m3ApiCheckMem(str, len);
    stdout_append(str, len);
    WASMD_LOG("host_gcp_print: appended %u bytes; first byte=0x%x", len, len > 0 ? str[0] : 0);
    m3ApiSuccess();
}

/* read_module_bytes() removed — wasmd no longer opens files. The
 * client sends the module bytes inline through the channel; wasmd reads
 * them from req->inline_payload. See handle_run_module(). Pre-D1 wasmd
 * tried syscall_open + syscall_read but hit a kernel cross-CPU vfs
 * spinlock race under fast spawn-and-execute load. */

/* Execute module bytes via wasm3. Returns wasmd status code. */
static int32_t run_module(const uint8_t *mod_bytes, size_t mod_len) {
    stdout_reset();

    WASMD_LOG("wasm3: NewEnvironment...");
    IM3Environment env = m3_NewEnvironment();
    if (!env) { WASMD_LOG("wasm3: NewEnvironment FAILED"); return WASMD_E_INTERNAL; }

    WASMD_LOG("wasm3: NewRuntime...");
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) {
        WASMD_LOG("wasm3: NewRuntime FAILED");
        m3_FreeEnvironment(env);
        return WASMD_E_INTERNAL;
    }

    WASMD_LOG("wasm3: ParseModule (len=%zu)...", mod_len);
    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, mod_bytes, (uint32_t)mod_len);
    if (r) {
        WASMD_LOG("ParseModule: %s", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return WASMD_E_LOAD_FAILED;
    }

    WASMD_LOG("wasm3: LoadModule...");
    r = m3_LoadModule(rt, mod);
    if (r) {
        WASMD_LOG("LoadModule: %s", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return WASMD_E_LOAD_FAILED;
    }

    WASMD_LOG("wasm3: LinkRawFunction(gcp.print)...");
    r = m3_LinkRawFunction(mod, "gcp", "print", "v(*i)", &host_gcp_print);
    if (r) {
        WASMD_LOG("LinkRawFunction(gcp.print): %s", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return WASMD_E_INSTANTIATE_FAILED;
    }

    WASMD_LOG("wasm3: FindFunction(_start)...");
    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, "_start");
    if (r) {
        WASMD_LOG("FindFunction(_start): %s", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return WASMD_E_INSTANTIATE_FAILED;
    }

    WASMD_LOG("wasm3: CallV...");
    r = m3_CallV(fn);
    int32_t status = (r == NULL) ? WASMD_OK : WASMD_E_TRAP;
    if (r) WASMD_LOG("CallV: %s", r);
    else   WASMD_LOG("wasm3: CallV returned (status=OK, stdout_len=%u)", g_stdout_len);

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return status;
}

/* Process one RUN_MODULE message; populate response.
 * Inbound:
 *   inline_payload[0..3]   op = WASMD_OP_RUN_MODULE
 *   inline_payload[4..7]   uint32_t wasm_len
 *   inline_payload[8..]    wasm module bytes (up to WASMD_BYTES_MAX)
 * Outbound:
 *   inline_payload = wasmd_response_header_t + stdout bytes
 */
static void handle_run_module(const chan_msg_user_t *req, chan_msg_user_t *resp) {
    wasmd_response_header_t *rh = (wasmd_response_header_t *)resp->inline_payload;
    rh->op = WASMD_OP_RUN_RESPONSE;
    rh->reserved = 0;
    rh->stdout_len = 0;

    /* Read inline length + bytes. Total inline_len must accommodate the
       header (8 bytes: op + len) plus the payload. */
    uint32_t wasm_len = *(const uint32_t *)(req->inline_payload + 4);
    if (wasm_len == 0 || wasm_len > WASMD_BYTES_MAX) {
        WASMD_LOG("RUN_MODULE: bad wasm_len=%u", wasm_len);
        rh->status = WASMD_E_INTERNAL;
        return;
    }
    uint32_t expected_inline = WASMD_BYTES_OFFSET + wasm_len;
    if (req->header.inline_len < expected_inline) {
        WASMD_LOG("RUN_MODULE: inline_len=%u < expected=%u",
                  req->header.inline_len, expected_inline);
        rh->status = WASMD_E_INTERNAL;
        return;
    }

    const uint8_t *mod_bytes = req->inline_payload + WASMD_BYTES_OFFSET;
    WASMD_LOG("RUN_MODULE wasm_len=%u (inlined)", wasm_len);

    /* Pre-validate via Phase 26 loader. */
    wasm_module_t mod_meta;
    int rc = wasm_load(mod_bytes, wasm_len, &mod_meta);
    if (rc != WASM_OK) {
        WASMD_LOG("wasm_load -> %d", rc);
        rh->status = WASMD_E_LOAD_FAILED;
        return;
    }
    WASMD_LOG("loader ok: %u imports", mod_meta.n_imports);

    /* Execute via wasm3 in-process. */
    rh->status = run_module(mod_bytes, wasm_len);

    /* Copy captured stdout into response payload (after the 16-byte header). */
    uint8_t *out_buf = resp->inline_payload + sizeof(*rh);
    uint32_t out_max = (uint32_t)(sizeof(resp->inline_payload) - sizeof(*rh));
    uint32_t copy = (g_stdout_len < out_max) ? g_stdout_len : out_max;
    if (copy) memcpy(out_buf, g_stdout, copy);
    rh->stdout_len = copy;
    WASMD_LOG("response: status=%d stdout_len=%u", rh->status, rh->stdout_len);
}

void _start(void) {
    WASMD_LOG("starting; publishing %s", WASMD_SERVICE_NAME);

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
            spin_ms(5);
            continue;
        }
        WASMD_LOG("accept: client=%d", cli.connector_pid);

        chan_msg_user_t req, resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        long rr = syscall_chan_recv(cli.rd_req, &req, /*timeout_ns=*/2000000000ULL);
        if (rr < 0) {
            WASMD_LOG("chan_recv -> %ld; closing client", rr);
            continue;
        }

        uint32_t op = *(const uint32_t *)req.inline_payload;
        if (op == WASMD_OP_RUN_MODULE) {
            handle_run_module(&req, &resp);
        } else {
            WASMD_LOG("unknown op 0x%x", op);
            wasmd_response_header_t *rh = (wasmd_response_header_t *)resp.inline_payload;
            rh->op = WASMD_OP_RUN_RESPONSE;
            rh->status = WASMD_E_INTERNAL;
            rh->stdout_len = 0;
        }

        resp.header.type_hash = wasmd_type_hash();
        resp.header.flags = 0;
        wasmd_response_header_t *resp_rh =
            (wasmd_response_header_t *)resp.inline_payload;
        resp.header.inline_len = (uint16_t)(sizeof(wasmd_response_header_t) +
                                            resp_rh->stdout_len);

        long sr = syscall_chan_send(cli.wr_resp, &resp, /*timeout_ns=*/1000000000ULL);
        if (sr < 0) {
            WASMD_LOG("chan_send response -> %ld", sr);
        }
    }
}
