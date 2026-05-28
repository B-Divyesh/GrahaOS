/* user/tests/wasm_lacks_cap.c — FU27.WASM.D2_worker gate test (V2).
 *
 * Phase 29 Session G note: the FS-based cap-gate prototype was
 * reverted (vfs_lock race with startup publish).  This test now
 * exercises the implicit cap-deny path through wasm3's link layer:
 * demo_net.wasm imports gcp.net-send which wasmd does NOT provide.
 * m3_LinkRawFunction would skip it (m3Err_functionLookupFailed is
 * benign) but m3_FindFunction/_start needs every imported function
 * resolved by call time — the trap surfaces as WASMD_E_TRAP at
 * m3_CallV.  This is the in-process analogue of cap denial.
 *
 * Asserts:
 *   1. RUN_MODULE(demo_net.wasm) returns WASMD_E_TRAP (binding absent).
 *   2. wasmd survives (hello.wasm OK afterwards).
 *   3. hello stdout still contains "hello".
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
    tap_plan(3);

    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        TAP_ASSERT(0, "wasm_lacks_cap: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
        syscall_exit(1);
    }

    static uint8_t dn_buf[WASMD_BYTES_MAX];
    long dn_n = read_fixture("bin/tests/wasm/demo_net.wasm",
                             dn_buf, WASMD_BYTES_MAX);
    static uint8_t hello_buf[WASMD_BYTES_MAX];
    long hello_n = read_fixture("bin/tests/wasm/hello.wasm",
                                hello_buf, WASMD_BYTES_MAX);
    if (dn_n <= 0 || hello_n <= 0) {
        TAP_ASSERT(0, "wasm_lacks_cap: read fixtures failed");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
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
        TAP_ASSERT(0, "wasm_lacks_cap: connect /sys/wasm/control");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
        TAP_ASSERT(0, "wasm_lacks_cap: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    /* Phase 1: demo_net.wasm imports gcp.net-send which wasmd doesn't
     * resolve.  wasm3 traps at call time; wasmd returns WASMD_E_TRAP. */
    int32_t status1 = 0;
    uint8_t  out1[64];
    uint32_t out1len = 0;
    (void)run_module(&cli, dn_buf, (uint32_t)dn_n, &status1,
                     out1, sizeof(out1), &out1len);
    printf("# wasm_lacks_cap: status1=%d\n", status1);
    TAP_ASSERT(status1 == WASMD_E_TRAP || status1 == WASMD_E_INSTANTIATE_FAILED,
               "wasm_lacks_cap: demo_net.wasm -> trap or instantiate-failed (no binding)");

    /* Phase 2: hello.wasm — daemon survives. */
    int32_t status2 = 0;
    uint8_t out2[128];
    uint32_t out2len = 0;
    int rc2 = run_module(&cli, hello_buf, (uint32_t)hello_n, &status2,
                         out2, sizeof(out2), &out2len);
    TAP_ASSERT(rc2 == 0 && status2 == WASMD_OK,
               "wasm_lacks_cap: hello OK after cap_denied (daemon survived)");

    int has_hello = contains_substring(out2, out2len, "hello");
    TAP_ASSERT(has_hello, "wasm_lacks_cap: hello stdout intact");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
