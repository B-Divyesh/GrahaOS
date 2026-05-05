/* user/tests/wasm_fault_trap.c — FU27.WASM Stage D2 trap-path gate test.
 *
 * Sends oopsie.wasm (37-byte fixture containing a single `unreachable`
 * opcode in _start) to wasmd via /sys/wasm/control. wasm3's interpreter
 * catches the trap and m3_CallV returns m3Err_trapUnreachable; wasmd
 * maps that to WASMD_E_TRAP (-6). Because wasm3 v1 runs in-process, the
 * trap stays inside wasmd's call frame — no CPU fault, no daemon
 * crash. We follow the trap with a hello.wasm RUN_MODULE on the same
 * wasmd to prove the daemon survives.
 *
 * Asserts:
 *   1. RUN_MODULE(oopsie) returns WASMD_E_TRAP (-6)
 *   2. wasmd is still responsive afterwards (RUN_MODULE(hello) returns OK)
 *   3. hello's stdout still contains "hello" (output buffer wasn't
 *      corrupted by the prior trap)
 *
 * No syscall_wait; trap detection is via the synchronous response.
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

/* Read a fixture into wasm_buf. Returns byte count on success, <=0 on
 * failure. */
static long read_fixture(const char *path, uint8_t *buf, uint32_t cap) {
    int fd = syscall_open(path);
    if (fd < 0) return fd;
    long n = syscall_read(fd, buf, cap);
    (void)syscall_close(fd);
    return n;
}

/* Send RUN_MODULE with mod_bytes/mod_len, recv the response, copy
 * status + stdout_len into out_status/out_stdout_len. Stdout bytes are
 * copied into stdout_buf (cap=stdout_cap). Returns 0 on send/recv OK,
 * negative on transport failure. The wasmd status is conveyed via
 * out_status, NOT this function's return. */
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
    long rr = syscall_chan_recv(cli->rd_resp, &resp, /*timeout_ns=*/5000000000ULL);
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
    tap_plan(3);

    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        TAP_ASSERT(0, "wasm_fault_trap: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        syscall_exit(1);
    }

    static uint8_t oopsie_buf[WASMD_BYTES_MAX];
    long oopsie_n = read_fixture("bin/tests/wasm/oopsie.wasm",
                                 oopsie_buf, WASMD_BYTES_MAX);
    if (oopsie_n <= 0 || oopsie_n > WASMD_BYTES_MAX) {
        TAP_ASSERT(0, "wasm_fault_trap: read oopsie.wasm failed");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(2);
    }

    static uint8_t hello_buf[WASMD_BYTES_MAX];
    long hello_n = read_fixture("bin/tests/wasm/hello.wasm",
                                hello_buf, WASMD_BYTES_MAX);
    if (hello_n <= 0 || hello_n > WASMD_BYTES_MAX) {
        TAP_ASSERT(0, "wasm_fault_trap: read hello.wasm failed");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
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
        TAP_ASSERT(0, "wasm_fault_trap: connect /sys/wasm/control");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        TAP_ASSERT(0, "wasm_fault_trap: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    /* Phase 1: oopsie.wasm — expect WASMD_E_TRAP. */
    int32_t  status1 = 0;
    uint8_t  out1[64];
    uint32_t out1len = 0;
    int rc1 = run_module(&cli, oopsie_buf, (uint32_t)oopsie_n,
                         &status1, out1, sizeof(out1), &out1len);
    if (rc1 != 0) {
        printf("# wasm_fault_trap: oopsie transport rc=%d\n", rc1);
    }
    TAP_ASSERT(status1 == WASMD_E_TRAP,
               "wasm_fault_trap: oopsie.wasm returns WASMD_E_TRAP");

    /* Phase 2: hello.wasm — daemon should still respond. */
    int32_t  status2 = 0;
    uint8_t  out2[128];
    uint32_t out2len = 0;
    int rc2 = run_module(&cli, hello_buf, (uint32_t)hello_n,
                         &status2, out2, sizeof(out2), &out2len);
    if (rc2 != 0) {
        printf("# wasm_fault_trap: hello transport rc=%d (daemon may be dead)\n",
               rc2);
    }
    TAP_ASSERT(rc2 == 0 && status2 == WASMD_OK,
               "wasm_fault_trap: hello.wasm OK after trap (daemon survived)");

    int has_hello = contains_substring(out2, out2len, "hello");
    TAP_ASSERT(has_hello,
               "wasm_fault_trap: hello.wasm stdout intact after prior trap");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
