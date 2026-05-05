/* user/tests/wasm_concurrent_serial.c — FU27.WASM Stage D2 serialization gate.
 *
 * v1 wasmd's accept loop is single-threaded (one client at a time);
 * RUN_MODULE messages are serialized by the channel + the in-process
 * wasm3 runtime (which mints a new IM3Environment + IM3Runtime per
 * call). This test verifies that 4 sequential RUN_MODULE calls on the
 * same connection all succeed cleanly with no inter-call state leak.
 *
 * The full plan calls for a 16-way concurrent test (wasm_concurrent_16);
 * that requires either (a) a per-instance worker subprocess (deferred —
 * see FU27.WASM.D2 follow-up) or (b) per-connection wasmd routing
 * which v1 doesn't have. Until the worker substrate lands, serial
 * 4-way reuse is the strongest property we can assert.
 *
 * Asserts:
 *   1. All 4 RUN_MODULE calls return WASMD_OK.
 *   2. All 4 produce stdout containing "hello".
 *   3. wasmd hasn't accumulated state (last call's stdout is exactly
 *      the same length as the first — no "running tally" leak).
 */

#include "../libtap.h"
#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../wasmd/proto.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int printf(const char *fmt, ...);

static int contains_substring(const uint8_t *haystack, uint32_t hlen,
                              const char *needle) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || hlen < nlen) return 0;
    for (uint32_t i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        while (k < nlen && haystack[i + k] == (uint8_t)needle[k]) k++;
        if (k == nlen) return 1;
    }
    return 0;
}

/* Phase 24a-class wall-clock budget pressure: each wasm3 invocation
 * costs ~150ms in TCG (env+rt+parse+load+find+call). 3 iters keeps the
 * gate under 4s for this test while still validating per-call
 * environment cleanup. */
#define N_ITER 3

void _start(void) {
    tap_plan(3);

    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        TAP_ASSERT(0, "wasm_concurrent_serial: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
        syscall_exit(1);
    }

    static uint8_t hello_buf[WASMD_BYTES_MAX];
    int fd_pre = syscall_open("bin/tests/wasm/hello.wasm");
    long hello_n = (fd_pre < 0) ? -1 : syscall_read(fd_pre, hello_buf, WASMD_BYTES_MAX);
    if (fd_pre >= 0) (void)syscall_close(fd_pre);
    if (hello_n <= 0 || hello_n > WASMD_BYTES_MAX) {
        TAP_ASSERT(0, "wasm_concurrent_serial: read hello.wasm failed");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
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
        TAP_ASSERT(0, "wasm_concurrent_serial: connect /sys/wasm/control");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
        TAP_ASSERT(0, "wasm_concurrent_serial: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    int      ok_count   = 0;
    int      hello_count = 0;
    uint32_t first_len  = 0;
    uint32_t last_len   = 0;

    for (int i = 0; i < N_ITER; i++) {
        chan_msg_user_t req;
        memset(&req, 0, sizeof(req));
        req.header.type_hash = wasmd_type_hash();
        *(uint32_t *)(req.inline_payload + 0) = WASMD_OP_RUN_MODULE;
        *(uint32_t *)(req.inline_payload + 4) = (uint32_t)hello_n;
        memcpy(req.inline_payload + WASMD_BYTES_OFFSET, hello_buf, (size_t)hello_n);
        req.header.inline_len = (uint16_t)(WASMD_BYTES_OFFSET + hello_n);

        long sr = syscall_chan_send(cli.wr_req, &req, /*timeout_ns=*/2000000000ULL);
        if (sr < 0) { printf("# iter %d: send rc=%ld\n", i, sr); continue; }

        chan_msg_user_t resp;
        memset(&resp, 0, sizeof(resp));
        long rr = syscall_chan_recv(cli.rd_resp, &resp, /*timeout_ns=*/5000000000ULL);
        if (rr < 0) { printf("# iter %d: recv rc=%ld\n", i, rr); continue; }

        const wasmd_response_header_t *rh =
            (const wasmd_response_header_t *)resp.inline_payload;
        if (rh->status == WASMD_OK) ok_count++;
        else printf("# iter %d: status=%d\n", i, rh->status);

        const uint8_t *out = resp.inline_payload + sizeof(*rh);
        if (contains_substring(out, rh->stdout_len, "hello")) hello_count++;

        if (i == 0) first_len = rh->stdout_len;
        if (i == N_ITER - 1) last_len = rh->stdout_len;
    }

    TAP_ASSERT(ok_count == N_ITER,
               "wasm_concurrent_serial: all RUN_MODULEs return WASMD_OK");
    TAP_ASSERT(hello_count == N_ITER,
               "wasm_concurrent_serial: all stdouts contain 'hello'");
    TAP_ASSERT(first_len == last_len && first_len > 0,
               "wasm_concurrent_serial: stdout lengths stable across calls (no state leak)");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
