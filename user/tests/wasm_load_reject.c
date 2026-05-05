/* user/tests/wasm_load_reject.c — FU27.WASM Stage D2 load-reject gate.
 *
 * Hand-crafts two malformed wasm payloads inline (no fixture file
 * needed) and asserts wasmd rejects each with WASMD_E_LOAD_FAILED:
 *
 *   1. bad-magic: first 4 bytes are not "\0asm" — wasm3's
 *      m3_ParseModule returns m3Err_wasmMalformed.
 *   2. truncated: just the 8-byte WASM header with no sections —
 *      wasmd's pre-validate (Phase 26 wasm_load) rejects with
 *      WASM_ERR_TRUNCATED, mapped to WASMD_E_LOAD_FAILED.
 *
 * Test 3 sends a valid hello.wasm afterwards to prove wasmd survives
 * malformed input (no slow leak / corrupt state).
 *
 * No FS access except for hello.wasm (loaded BEFORE connect to keep
 * the chan_recv path off the FS-lock-contention path).
 */

#include "../libtap.h"
#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../wasmd/proto.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int printf(const char *fmt, ...);

static int run_bytes(libnet_client_ctx_t *cli,
                     const uint8_t *bytes, uint32_t len,
                     int32_t *out_status) {
    chan_msg_user_t req;
    memset(&req, 0, sizeof(req));
    req.header.type_hash = wasmd_type_hash();
    *(uint32_t *)(req.inline_payload + 0) = WASMD_OP_RUN_MODULE;
    *(uint32_t *)(req.inline_payload + 4) = len;
    memcpy(req.inline_payload + WASMD_BYTES_OFFSET, bytes, (size_t)len);
    req.header.inline_len = (uint16_t)(WASMD_BYTES_OFFSET + len);

    long sr = syscall_chan_send(cli->wr_req, &req, /*timeout_ns=*/2000000000ULL);
    if (sr < 0) return (int)sr;

    chan_msg_user_t resp;
    memset(&resp, 0, sizeof(resp));
    long rr = syscall_chan_recv(cli->rd_resp, &resp, /*timeout_ns=*/5000000000ULL);
    if (rr < 0) return (int)rr;

    const wasmd_response_header_t *rh =
        (const wasmd_response_header_t *)resp.inline_payload;
    *out_status = rh->status;
    return 0;
}

/* Valid 8-byte WASM header for "truncated" test (header-only module
 * with no sections — wasm3's parser walks past the header looking for
 * sections and reaches EOF; load fails). */
static const uint8_t TRUNCATED_BYTES[] = {
    0x00, 0x61, 0x73, 0x6d,   /* magic: \0asm */
    0x01, 0x00, 0x00, 0x00,   /* version: 1 */
};

/* Bad magic: 0xDEADBEEF + version. Parser should reject on first read. */
static const uint8_t BAD_MAGIC_BYTES[] = {
    0xDE, 0xAD, 0xBE, 0xEF,   /* not \0asm */
    0x01, 0x00, 0x00, 0x00,
    /* enough bytes to look like a real module shape; payload after
       magic is irrelevant since magic check is first */
    0x00, 0x00, 0x00, 0x00,
};

void _start(void) {
    tap_plan(3);

    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        TAP_ASSERT(0, "wasm_load_reject: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        syscall_exit(1);
    }

    /* Pre-load hello.wasm for the survival test. */
    static uint8_t hello_buf[WASMD_BYTES_MAX];
    int fd_pre = syscall_open("bin/tests/wasm/hello.wasm");
    long hello_n = (fd_pre < 0) ? -1 : syscall_read(fd_pre, hello_buf, WASMD_BYTES_MAX);
    if (fd_pre >= 0) (void)syscall_close(fd_pre);
    if (hello_n <= 0 || hello_n > WASMD_BYTES_MAX) {
        TAP_ASSERT(0, "wasm_load_reject: read hello.wasm failed");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(2);
    }

    spin_ms(10);

    libnet_client_ctx_t cli;
    int rc = libnet_connect_service_with_retry(WASMD_SERVICE_NAME,
                                               WASMD_SERVICE_NAME_LEN,
                                               /*total_timeout_ms=*/8000,
                                               &cli);
    if (rc != 0) {
        TAP_ASSERT(0, "wasm_load_reject: connect /sys/wasm/control");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        TAP_ASSERT(0, "wasm_load_reject: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    /* 1. bad magic — wasmd's loader.c (wasm_load) rejects on magic mismatch
       before wasm3 parser even runs. */
    int32_t status_bad = 0;
    int trc1 = run_bytes(&cli, BAD_MAGIC_BYTES, sizeof(BAD_MAGIC_BYTES),
                         &status_bad);
    if (trc1 != 0) {
        printf("# wasm_load_reject: bad-magic transport rc=%d\n", trc1);
    }
    TAP_ASSERT(status_bad == WASMD_E_LOAD_FAILED,
               "wasm_load_reject: bad-magic returns WASMD_E_LOAD_FAILED");

    /* 2. truncated — header-only is technically a *valid* but empty
       wasm module per the spec (wasm3 parses it cleanly), so the
       rejection moves downstream: FindFunction("_start") fails →
       WASMD_E_INSTANTIATE_FAILED. Either error code counts as
       "module rejected"; both prove the daemon protected itself. */
    int32_t status_trunc = 0;
    int trc2 = run_bytes(&cli, TRUNCATED_BYTES, sizeof(TRUNCATED_BYTES),
                         &status_trunc);
    if (trc2 != 0) {
        printf("# wasm_load_reject: truncated transport rc=%d\n", trc2);
    }
    int trunc_rejected = (status_trunc == WASMD_E_LOAD_FAILED ||
                          status_trunc == WASMD_E_INSTANTIATE_FAILED);
    TAP_ASSERT(trunc_rejected,
               "wasm_load_reject: truncated returns load/instantiate error");

    /* 3. survival — hello.wasm still works after two malformed inputs. */
    int32_t status_hello = 0;
    int trc3 = run_bytes(&cli, hello_buf, (uint32_t)hello_n, &status_hello);
    if (trc3 != 0) {
        printf("# wasm_load_reject: post-reject hello rc=%d\n", trc3);
    }
    TAP_ASSERT(trc3 == 0 && status_hello == WASMD_OK,
               "wasm_load_reject: hello.wasm OK after rejects (daemon survived)");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
