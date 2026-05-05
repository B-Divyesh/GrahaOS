// user/wasm.c — Phase 26 Stage G operator CLI: `wasm run <path>`.
//
// Loads a WebAssembly module from the GrahaFS filesystem, parses its
// header + import section, and reports validation status. Substrate v1
// does NOT execute bytecode (Stage B verdict deferred Wasmtime; a Phase
// 27 follow-up vendors wasm3 or a custom interpreter).
//
// Usage:
//   wasm run <path>                       — load + validate; print outcome.
//   wasm run <path> --cap CSV             — additionally cross-reference
//                                            the module's imports against
//                                            CSV and reject missing-cap.
//   wasm version                          — print wasmd substrate version.
//
// Exit codes:
//   0  success (load + validate OK)
//   1  unknown arg
//   2  load error (bad magic / version / truncated / too many imports)
//   3  missing-cap (one or more imports outside CSV-allowed set)

#include "syscalls.h"
#include "wasmd/src/loader.h"
#include "wasmd/proto.h"
#include "libnet/libnet.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int  printf(const char *fmt, ...);

/* Pre-Stage-D1: held a 1 MiB local buffer + load_file/split_csv helpers
 * for in-process parsing. Stage D1 moves all parsing into wasmd, so this
 * client is now a thin libnet caller. */

static int s_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* load_file() and split_csv() removed in FU27.WASM Stage D1 — bin/wasmd now
 * handles file loading and import-cap validation server-side. Pre-D1
 * commits had local helpers here; see git history. */

static int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        printf("wasm run: missing <path>\n");
        return 1;
    }
    const char *path = argv[0];

    /* FU27.WASM Stage D1: connect to wasmd over /sys/wasm/control and
       request RUN_MODULE. wasmd does the heavy lifting (parse, narrow-
       exec a worker, capture stdout) and sends back a status + output
       payload. Pre-Stage-D1 stub used to call wasm_load locally; that's
       gone now (the loader still runs server-side inside wasmd). */
    libnet_client_ctx_t cli;
    int rc = libnet_connect_service_with_retry(WASMD_SERVICE_NAME,
                                               WASMD_SERVICE_NAME_LEN,
                                               /*total_timeout_ms=*/4000,
                                               &cli);
    if (rc < 0) {
        printf("wasm: cannot connect to %s (rc=%d). Is wasmd running?\n",
               WASMD_SERVICE_NAME, rc);
        return 1;
    }

    /* Build RUN_MODULE message:
         header.type_hash = channel-payload type (matches publisher)
         inline_payload[0..3] = WASMD_OP_RUN_MODULE
         inline_payload[4..]  = path string (zero-terminated) */
    /* Read module bytes locally; forward inline via channel. wasmd v1
       doesn't open files (sidesteps vfs lock contention). */
    int fd = syscall_open(path);
    if (fd < 0) {
        printf("wasm: cannot open '%s' (rc=%d)\n", path, fd);
        return 2;
    }
    static uint8_t wasm_buf[WASMD_BYTES_MAX];
    long n = syscall_read(fd, wasm_buf, WASMD_BYTES_MAX);
    (void)syscall_close(fd);
    if (n <= 0) {
        printf("wasm: read returned %ld\n", n);
        return 2;
    }
    if (n == WASMD_BYTES_MAX) {
        printf("wasm: module too large for inline transport (cap=%u). "
               "VMO-based transfer arrives in FU27.WASM Stage D2.\n", WASMD_BYTES_MAX);
        /* Continue — wasm3 may still parse the prefix as a fragment if
           it is a complete module. */
    }

    chan_msg_user_t req;
    memset(&req, 0, sizeof(req));
    req.header.type_hash = wasmd_type_hash();
    *(uint32_t *)(req.inline_payload + 0) = WASMD_OP_RUN_MODULE;
    *(uint32_t *)(req.inline_payload + 4) = (uint32_t)n;
    memcpy(req.inline_payload + WASMD_BYTES_OFFSET, wasm_buf, (size_t)n);
    req.header.inline_len = (uint16_t)(WASMD_BYTES_OFFSET + n);

    long sr = syscall_chan_send(cli.wr_req, &req, /*timeout_ns=*/2000000000ULL);
    if (sr < 0) {
        printf("wasm: chan_send -> %ld\n", sr);
        return 1;
    }

    chan_msg_user_t resp;
    memset(&resp, 0, sizeof(resp));
    long rr = syscall_chan_recv(cli.rd_resp, &resp, /*timeout_ns=*/30000000000ULL);
    if (rr < 0) {
        printf("wasm: chan_recv -> %ld (wasmd timeout?)\n", rr);
        return 1;
    }

    const wasmd_response_header_t *rh =
        (const wasmd_response_header_t *)resp.inline_payload;
    int32_t status = rh->status;
    uint32_t outlen = rh->stdout_len;
    uint32_t outmax = (uint32_t)(sizeof(resp.inline_payload) - sizeof(*rh));
    if (outlen > outmax) outlen = outmax;

    /* Print captured stdout (stored after the response header). */
    const char *buf = (const char *)(resp.inline_payload + sizeof(*rh));
    if (outlen > 0) {
        for (uint32_t i = 0; i < outlen; i++) {
            syscall_putc(buf[i]);
        }
        if (buf[outlen-1] != '\n') {
            syscall_putc('\n');
        }
    }

    if (status == WASMD_OK) {
        return 0;
    }
    printf("wasm: wasmd reported status=%d\n", status);
    /* Map wasmd status to a useful exit code for the test harness. */
    switch (status) {
        case WASMD_E_OPEN_FAILED:
        case WASMD_E_READ_FAILED:
        case WASMD_E_LOAD_FAILED:
            return 2;
        case WASMD_E_INSTANTIATE_FAILED:
            return 3;
        case WASMD_E_TRAP:
            return 4;
        default:
            return 5;
    }
}

static int cmd_version(void) {
    printf("wasm: substrate v1 (Phase 26 Stage E; wasm3+C verdict)\n");
    printf("      loader: WebAssembly 1.0 header + import section\n");
    printf("      execution: NOT IMPLEMENTED in v1 (Phase 27+ scope)\n");
    return 0;
}

static int cmd_help(void) {
    printf("usage: wasm <verb> [args]\n");
    printf("verbs:\n");
    printf("  run <path> [--cap CSV]   load + validate a .wasm module\n");
    printf("  version                  print substrate version\n");
    printf("  help                     this message\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return cmd_help();
    }
    const char *verb = argv[1];
    if (s_strcmp(verb, "run") == 0)     return cmd_run(argc - 2, argv + 2);
    if (s_strcmp(verb, "version") == 0) return cmd_version();
    if (s_strcmp(verb, "help") == 0)    return cmd_help();
    printf("wasm: unknown verb '%s'\n", verb);
    return 1;
}

void _start(int argc, char **argv) {
    int rc = main(argc, argv);
    syscall_exit(rc);
}
