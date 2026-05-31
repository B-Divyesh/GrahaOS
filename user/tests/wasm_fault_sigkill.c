/* user/tests/wasm_fault_sigkill.c — FU29.X.wasmd_subprocess gate test.
 *
 * sigkill.wasm prints a marker then enters an infinite loop — it makes ONE
 * host call (print) and then never returns from m3_CallV.  This proves that
 * wasmd's KILLABLE worker subprocess isolates a runaway module that has
 * already executed a host binding:
 *
 *   - RUN_MODULE_ISOLATED(sigkill) spawns bin/wasmd_worker, which runs the
 *     module via argv-hex transport (NO filesystem → no vfs_lock held).  The
 *     module prints (host_gcp_print just appends to a worker-local buffer —
 *     no syscall) then spins forever.  wasmd's deadline fires and SIGKILLs
 *     the worker while it is in a pure-userspace loop (orphan-free), reaps it,
 *     and returns WASMD_E_FUEL_EXHAUSTED.
 *   - wasmd SURVIVES the worker's death (crash isolation).
 *   - A subsequent in-process hello.wasm still works.
 *
 * Asserts (3):
 *   1. RUN_MODULE_ISOLATED(sigkill) returns WASMD_E_FUEL_EXHAUSTED
 *      (daemon force-killed + reaped the stuck worker).
 *   2. wasmd survives (hello.wasm OK afterwards).
 *   3. hello stdout still contains "hello" — buffer not corrupted.
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

static int run_module_op(libnet_client_ctx_t *cli, uint32_t op,
                         const uint8_t *mod_bytes, uint32_t mod_len,
                         int32_t *out_status,
                         uint8_t *stdout_buf, uint32_t stdout_cap,
                         uint32_t *out_stdout_len) {
    chan_msg_user_t req;
    memset(&req, 0, sizeof(req));
    req.header.type_hash = wasmd_type_hash();
    *(uint32_t *)(req.inline_payload + 0) = op;
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
        TAP_ASSERT(0, "wasm_fault_sigkill: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
        syscall_exit(1);
    }

    static uint8_t sk_buf[WASMD_BYTES_MAX];
    long sk_n = read_fixture("bin/tests/wasm/sigkill.wasm",
                             sk_buf, WASMD_BYTES_MAX);
    static uint8_t hello_buf[WASMD_BYTES_MAX];
    long hello_n = read_fixture("bin/tests/wasm/hello.wasm",
                                hello_buf, WASMD_BYTES_MAX);
    if (sk_n <= 0 || hello_n <= 0) {
        TAP_ASSERT(0, "wasm_fault_sigkill: read fixtures failed");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
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
        TAP_ASSERT(0, "wasm_fault_sigkill: connect failed");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
        TAP_ASSERT(0, "wasm_fault_sigkill: skip");
        if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
        syscall_exit(1);
    }

    /* Phase 1: sigkill.wasm (print-then-loop) via the ISOLATED worker path.
     * wasmd's deadline SIGKILLs the stuck worker; the daemon survives. */
    int32_t  status1 = 0;
    uint8_t  out1[64];
    uint32_t out1len = 0;
    int rc1 = run_module_op(&cli, WASMD_OP_RUN_MODULE_ISOLATED,
                            sk_buf, (uint32_t)sk_n,
                            &status1, out1, sizeof(out1), &out1len);
    printf("# wasm_fault_sigkill: rc1=%d status1=%d (expected %d)\n",
           rc1, status1, WASMD_E_FUEL_EXHAUSTED);
    TAP_ASSERT(rc1 == 0 && status1 == WASMD_E_FUEL_EXHAUSTED,
               "wasm_fault_sigkill: stuck worker SIGKILLed + reaped "
               "(WASMD_E_FUEL_EXHAUSTED), daemon survives");

    /* Phase 2: hello.wasm in-process — daemon survived. */
    int32_t  status2 = 0;
    uint8_t  out2[128];
    uint32_t out2len = 0;
    int rc2 = run_module_op(&cli, WASMD_OP_RUN_MODULE,
                            hello_buf, (uint32_t)hello_n,
                            &status2, out2, sizeof(out2), &out2len);
    TAP_ASSERT(rc2 == 0 && status2 == WASMD_OK,
               "wasm_fault_sigkill: hello OK after sigkill (daemon survived)");

    int has_hello = contains_substring(out2, out2len, "hello");
    TAP_ASSERT(has_hello,
               "wasm_fault_sigkill: hello stdout intact after sigkill");

    if (wasmd_pid > 0) (void)syscall_kill(wasmd_pid, /*SIGKILL=*/2);
    syscall_exit(0);
}
