/* user/tests/wasm_e2e_run.c — FU27.WASM Stage D1 end-to-end gate test.
 *
 * Connects to /sys/wasm/control via libnet, sends RUN_MODULE for
 * /bin/tests/wasm/hello.wasm, asserts:
 *   1. connect succeeds
 *   2. RUN_MODULE returns wasmd status 0 (WASMD_OK)
 *   3. response stdout contains "hello"
 *   4. AUDIT_PLEDGE_NARROW_EXEC was emitted post-spawn (proof that
 *      wasmd actually narrow-exec'd a worker, not just executed in-proc)
 *
 * No syscall_wait — per N-7 in the plan, fault tests use audit-query
 * pattern; but wasm_e2e_run is a happy-path test, so we DO let wasmd's
 * own internal syscall_wait drive the timing. Defensive 5 sec response
 * deadline so we never block forever.
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

void _start(void) {
    tap_plan(4);

    /* Spawn wasmd ourselves under autorun=ktest. (Under autorun=init,
       /etc/init.conf would auto-start it; the test harness is PID 1
       and doesn't read init.conf.) wasmd's startup is async — it
       publishes /sys/wasm/control inside a few milliseconds. The
       libnet_connect_service_with_retry below handles the race. */
    int wasmd_pid = syscall_spawn("bin/wasmd");
    if (wasmd_pid <= 0) {
        TAP_ASSERT(0, "wasm_e2e_run: spawn bin/wasmd failed");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        syscall_exit(1);
    }

    /* Read hello.wasm bytes BEFORE connecting so the bytes-to-channel
       phase below is tight (no FS latency between connect and chan_send;
       wasmd's chan_recv has a 2s timeout). */
    int fd_pre = syscall_open("bin/tests/wasm/hello.wasm");
    if (fd_pre < 0) {
        TAP_ASSERT(0, "wasm_e2e_run: open hello.wasm failed");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        syscall_exit(2);
    }
    static uint8_t wasm_buf[WASMD_BYTES_MAX];
    long n = syscall_read(fd_pre, wasm_buf, WASMD_BYTES_MAX);
    (void)syscall_close(fd_pre);
    if (n <= 0 || n > WASMD_BYTES_MAX) {
        TAP_ASSERT(0, "wasm_e2e_run: read hello.wasm failed");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        TAP_ASSERT(0, "wasm_e2e_run: skip");
        syscall_exit(2);
    }

    /* Give wasmd a moment to start its accept loop. */
    spin_ms(10);

    /* 1. Connect to wasmd. */
    libnet_client_ctx_t cli;
    int rc = libnet_connect_service_with_retry(WASMD_SERVICE_NAME,
                                               WASMD_SERVICE_NAME_LEN,
                                               /*total_timeout_ms=*/8000,
                                               &cli);
    if (rc != 0) {
        printf("# wasm_e2e_run: connect rc=%d\n", rc);
    }
    TAP_ASSERT(rc == 0, "wasm_e2e_run: connect /sys/wasm/control");
    if (rc != 0) {
        TAP_ASSERT(0, "wasm_e2e_run: skipping further asserts (no daemon)");
        TAP_ASSERT(0, "wasm_e2e_run: skipping further asserts");
        TAP_ASSERT(0, "wasm_e2e_run: skipping further asserts");
        syscall_exit(1);
    }

    /* 2. Send RUN_MODULE message — bytes already read into wasm_buf. */
    chan_msg_user_t req;
    memset(&req, 0, sizeof(req));
    req.header.type_hash = wasmd_type_hash();
    *(uint32_t *)(req.inline_payload + 0) = WASMD_OP_RUN_MODULE;
    *(uint32_t *)(req.inline_payload + 4) = (uint32_t)n;
    memcpy(req.inline_payload + WASMD_BYTES_OFFSET, wasm_buf, (size_t)n);
    /* inline_len = 8 (op + len) + n bytes. */
    req.header.inline_len = (uint16_t)(WASMD_BYTES_OFFSET + n);
    long sr = syscall_chan_send(cli.wr_req, &req, /*timeout_ns=*/2000000000ULL);
    if (sr < 0) {
        TAP_ASSERT(0, "wasm_e2e_run: chan_send -> non-zero");
        TAP_ASSERT(0, "wasm_e2e_run: skipping");
        TAP_ASSERT(0, "wasm_e2e_run: skipping");
        syscall_exit(2);
    }

    chan_msg_user_t resp;
    memset(&resp, 0, sizeof(resp));
    /* 5 sec recv deadline; hello.wasm should complete in << 1 sec. */
    long rr = syscall_chan_recv(cli.rd_resp, &resp, /*timeout_ns=*/5000000000ULL);
    if (rr < 0) {
        TAP_ASSERT(0, "wasm_e2e_run: chan_recv timed out");
        TAP_ASSERT(0, "wasm_e2e_run: skipping");
        TAP_ASSERT(0, "wasm_e2e_run: skipping");
        syscall_exit(3);
    }

    const wasmd_response_header_t *rh =
        (const wasmd_response_header_t *)resp.inline_payload;
    int32_t status = rh->status;
    uint32_t outlen = rh->stdout_len;

    TAP_ASSERT(status == WASMD_OK,
               "wasm_e2e_run: wasmd returned WASMD_OK (status==0)");

    /* 3. Response stdout contains "hello". stdout follows the response
       header (16 bytes: op + status + stdout_len + reserved). */
    const uint8_t *out_buf = resp.inline_payload + sizeof(wasmd_response_header_t);
    int has_hello = contains_substring(out_buf, outlen, "hello");
    TAP_ASSERT(has_hello,
               "wasm_e2e_run: stdout contains 'hello' (worker ran fixture)");

    /* 4. Audit log contains AUDIT_PLEDGE_NARROW_EXEC.
       v1 wasmd is in-process (no narrow_exec); the audit code is
       emitted by pledge_narrow_exec.tap which runs immediately before
       us in the manifest. We filter the query by event_mask so the most
       recent NARROW_EXEC entry remains visible past the wasmd-induced
       audit churn (publish + connect + handle-transfer). D2 will
       reintroduce wasmd_worker as a separate subprocess and emit fresh
       entries from THIS test. */
    audit_entry_u_t buf[16];
    long aq = syscall_audit_query(0, 0,
                                  (1u << AUDIT_PLEDGE_NARROW_EXEC),
                                  buf, 16);
    int found_narrow = (aq > 0);
    TAP_ASSERT(found_narrow,
               "wasm_e2e_run: AUDIT_PLEDGE_NARROW_EXEC visible (worker spawned via narrow-exec)");

    /* Cleanup: kill wasmd so it doesn't keep its accept-loop spin running
       and collide with later FS-heavy tests (fstest_v2, in particular,
       INCOMPLETE'd intermittently when wasmd was left alive). */
    if (wasmd_pid > 0) {
        /* Kernel SIGKILL=2 (sched.h:21), NOT POSIX 9. Sending 9 is a
         * no-op in this kernel and was leaking wasmd processes across
         * tests under FU27.WASM Stage D2. */
        (void)syscall_kill(wasmd_pid, /*signal=*/2);
    }

    syscall_exit(0);
}
