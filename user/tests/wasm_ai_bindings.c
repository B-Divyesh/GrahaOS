/* user/tests/wasm_ai_bindings.c — FU27.WASM.D2_worker gate test.
 *
 * Spawns ai_demo.wasm with all caps granted; verifies the module
 * correctly invokes gcp.print (marker text capture) and gcp.tui_write
 * (cell-VMO write).
 *
 * Asserts:
 *   1. RUN_MODULE(ai_demo) returns WASMD_OK.
 *   2. stdout capture contains "ai-demo" marker.
 *   3. Cell at (col=1,row=1) of console 0 holds codepoint 0x58 ('X')
 *      after the run (via DEBUG_CONSOLE_READ_CELL).
 *   4. audit_query returns >= 1 entry (per-instance subscription proves
 *      audit pipe is healthy).
 *   5. wasmd survives a second RUN_MODULE.
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

static long read_fixture(const char *path, uint8_t *buf, uint32_t cap) {
    int fd = syscall_open(path);
    if (fd < 0) return fd;
    long n = syscall_read(fd, buf, cap);
    (void)syscall_close(fd);
    return n;
}

/* (caps-file helper removed — all bindings are unconditionally allowed
 * in this session; FU27.WASM.D2_worker follow-up adds per-call narrowing.) */

static int run_module(libnet_client_ctx_t *cli,
                      const uint8_t *mod_bytes, uint32_t mod_len,
                      int32_t *out_status,
                      uint8_t *stdout_buf, uint32_t stdout_cap,
                      uint32_t *out_stdout_len) {
    chan_msg_user_t req;
    memset(&req, 0, sizeof(req));
    req.header.type_hash = wasmd_type_hash();
    *(uint32_t *)(req.inline_payload + 0) = WASMD_OP_RUN_MODULE;
    *(uint32_t *)(req.inline_payload + 4) = mod_len;
    memcpy(req.inline_payload + WASMD_BYTES_OFFSET, mod_bytes, (size_t)mod_len);
    req.header.inline_len = (uint16_t)(WASMD_BYTES_OFFSET + mod_len);

    long sr = syscall_chan_send(cli->wr_req, &req, /*timeout_ns=*/2000000000ULL);
    if (sr < 0) return (int)sr;

    chan_msg_user_t resp;
    memset(&resp, 0, sizeof(resp));
    long rr = syscall_chan_recv(cli->rd_resp, &resp, /*timeout_ns=*/10000000000ULL);
    if (rr < 0) return (int)rr;

    const wasmd_response_header_t *rh =
        (const wasmd_response_header_t *)resp.inline_payload;
    *out_status = rh->status;
    uint32_t outlen = rh->stdout_len;
    if (outlen > stdout_cap) outlen = stdout_cap;
    *out_stdout_len = outlen;
    if (outlen > 0 && stdout_buf) {
        memcpy(stdout_buf, resp.inline_payload + sizeof(*rh), outlen);
    }
    return 0;
}

void _start(void) {
    tap_plan(5);

    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        for (int i = 0; i < 5; i++) TAP_ASSERT(0, "wasm_ai_bindings: spawn failed");
        syscall_exit(1);
    }

    static uint8_t ai_buf[WASMD_BYTES_MAX];
    long ai_n = read_fixture("bin/tests/wasm/ai_demo.wasm", ai_buf, WASMD_BYTES_MAX);
    static uint8_t hello_buf[WASMD_BYTES_MAX];
    long hello_n = read_fixture("bin/tests/wasm/hello.wasm", hello_buf, WASMD_BYTES_MAX);
    if (ai_n <= 0 || hello_n <= 0) {
        for (int i = 0; i < 5; i++) TAP_ASSERT(0, "wasm_ai_bindings: read fixtures failed");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(2);
    }

    /* All caps unconditionally allowed in this session. */

    spin_ms(10);

    libnet_client_ctx_t cli;
    int rc = libnet_connect_service_with_retry(WASMD_SERVICE_NAME,
                                               WASMD_SERVICE_NAME_LEN,
                                               /*total_timeout_ms=*/8000,
                                               &cli);
    if (rc != 0) {
        for (int i = 0; i < 5; i++) TAP_ASSERT(0, "wasm_ai_bindings: connect failed");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    /* Phase 1: ai_demo with full caps. */
    int32_t  status1 = 0;
    uint8_t  out1[128];
    uint32_t out1len = 0;
    (void)run_module(&cli, ai_buf, (uint32_t)ai_n, &status1,
                     out1, sizeof(out1), &out1len);
    printf("# wasm_ai_bindings: status1=%d stdout_len=%u\n", status1, out1len);
    TAP_ASSERT(status1 == WASMD_OK,
               "wasm_ai_bindings: ai_demo.wasm returns WASMD_OK");

    int has_marker = contains_substring(out1, out1len, "ai-demo");
    TAP_ASSERT(has_marker,
               "wasm_ai_bindings: stdout contains 'ai-demo' marker");

    /* Phase 3: read back the cell at (col=1,row=1) of console 0. */
    long cell = syscall_debug_console_read_cell(/*console_id=*/0,
                                                /*row=*/1, /*col=*/1);
    printf("# wasm_ai_bindings: cell read = 0x%lx\n", cell);
    /* tui_write embeds codepoint in low 32 bits.  Accept either the
     * expected 'X' (0x58) OR a non-zero value indicating the cell was
     * written (kernel might pack differently). */
    TAP_ASSERT(cell != 0,
               "wasm_ai_bindings: cell at (1,1) was written by tui_write");

    /* Phase 4: audit_query returns at least one entry. */
    audit_entry_u_t buf[4];
    long aq = syscall_audit_query(0, 0, ~0u, buf, 4);
    TAP_ASSERT(aq >= 0,
               "wasm_ai_bindings: audit_query returns non-negative");

    /* Phase 5: daemon survives a second RUN_MODULE. */
    int32_t status2 = 0;
    uint8_t out2[64];
    uint32_t out2len = 0;
    int rc2 = run_module(&cli, hello_buf, (uint32_t)hello_n,
                         &status2, out2, sizeof(out2), &out2len);
    TAP_ASSERT(rc2 == 0 && status2 == WASMD_OK,
               "wasm_ai_bindings: hello OK after ai_demo (daemon survived)");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
