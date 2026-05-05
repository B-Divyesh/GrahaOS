/*
 * user/wasmd/wasmd_worker.c — FU27.WASM Stage D1 per-instance worker.
 *
 * Spawned by wasmd via PLEDGE_FLAG_NARROW_EXEC with a narrowed pledge of
 *   PLEDGE_FS_READ | PLEDGE_FS_WRITE | PLEDGE_COMPUTE | PLEDGE_TIME.
 *
 * Lifecycle:
 *   1. Open /tmp/wasmd_pending.wasm (single-slot v1 staging file).
 *   2. Read up to 1 MiB of bytes.
 *   3. m3_NewEnvironment / NewRuntime / ParseModule / LoadModule.
 *   4. m3_LinkRawFunction("gcp", "print", "v(*i)", host_print).
 *   5. m3_FindFunction("_start") and m3_CallV().
 *   6. Write captured stdout buffer to /tmp/wasmd_output.txt.
 *   7. exit(N) where N reflects which step failed:
 *        0 = OK
 *        1 = load fail (open/read/parse/load)
 *        2 = instantiate fail (link/find_function)
 *        3 = trap (CallV returned non-NULL M3Result)
 *        4 = internal (malloc failure etc.)
 */

#include "../syscalls.h"
#include "proto.h"
#include "wasm3.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Captured-stdout buffer. wasm3 module's `print` import appends here.
 * Sized to fit comfortably in the daemon's response payload (256 - 8 = 248
 * bytes once header is subtracted). We store up to 240 bytes + an overflow
 * marker; truncation guarantees no buffer overrun even if the module
 * prints a megabyte. */
#define WORKER_STDOUT_MAX 240
static uint8_t  g_stdout[WORKER_STDOUT_MAX + 1];
static uint32_t g_stdout_len = 0;

static void stdout_append(const uint8_t *bytes, uint32_t len) {
    if (g_stdout_len >= WORKER_STDOUT_MAX) return;
    uint32_t avail = WORKER_STDOUT_MAX - g_stdout_len;
    uint32_t copy = (len < avail) ? len : avail;
    if (copy) memcpy(g_stdout + g_stdout_len, bytes, copy);
    g_stdout_len += copy;
    g_stdout[g_stdout_len] = '\0';
}

/* Host import: "gcp.print" — signature "v(*i)" — print(ptr, len).
 *
 * Module pushes (ptr, len) onto the stack, calls print. We read the bytes
 * out of wasm linear memory and append them to g_stdout. */
m3ApiRawFunction(host_gcp_print) {
    m3ApiGetArgMem(const uint8_t *, str);
    m3ApiGetArg(uint32_t, len);

    /* Bounds check inside wasm memory. */
    if (len > 1024) len = 1024;  /* sanity cap */
    m3ApiCheckMem(str, len);

    stdout_append(str, len);
    m3ApiSuccess();
}

static int read_pending_module(uint8_t **out_bytes, size_t *out_len) {
    *out_bytes = NULL;
    *out_len = 0;

    int fd = syscall_open(WASMD_PENDING_PATH);
    if (fd < 0) return 1;

    const size_t MAX = 1u << 20;
    uint8_t *buf = malloc(MAX);
    if (!buf) {
        (void)syscall_close(fd);
        return 4;
    }
    size_t total = 0;
    while (total < MAX) {
        long r = syscall_read(fd, buf + total, MAX - total);
        if (r < 0) { free(buf); (void)syscall_close(fd); return 1; }
        if (r == 0) break;
        total += (size_t)r;
    }
    (void)syscall_close(fd);
    *out_bytes = buf;
    *out_len = total;
    return 0;
}

static void write_output_file(void) {
    (void)syscall_create(WASMD_OUTPUT_PATH, 0644);
    int fd = syscall_open(WASMD_OUTPUT_PATH);
    if (fd < 0) return;
    if (g_stdout_len) {
        (void)syscall_write(fd, g_stdout, g_stdout_len);
    }
    (void)syscall_close(fd);
}

void _start(void) {
    uint8_t *mod_bytes = NULL;
    size_t mod_len = 0;
    int rc = read_pending_module(&mod_bytes, &mod_len);
    if (rc != 0) {
        write_output_file();  /* possibly empty */
        syscall_exit(rc);
    }

    /* m3_New* allocate via malloc; on failure exit 4. */
    IM3Environment env = m3_NewEnvironment();
    if (!env) { free(mod_bytes); write_output_file(); syscall_exit(4); }

    /* 64 KiB wasm stack — plenty for hello.wasm; well under our user heap. */
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) {
        m3_FreeEnvironment(env);
        free(mod_bytes); write_output_file(); syscall_exit(4);
    }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, mod_bytes, (uint32_t)mod_len);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); write_output_file(); syscall_exit(1);
    }

    r = m3_LoadModule(rt, mod);
    if (r) {
        /* m3_LoadModule transfers ownership of `mod` to `rt` regardless of
           outcome — don't free `mod` here. */
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); write_output_file(); syscall_exit(1);
    }

    /* Wire host imports. The module's import is "gcp.print" with sig "v(*i)"
       — return void, take (ptr, i32-length). hello.wasm calls this. */
    r = m3_LinkRawFunction(mod, "gcp", "print", "v(*i)", &host_gcp_print);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); write_output_file(); syscall_exit(2);
    }

    /* Find _start and call. */
    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, "_start");
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); write_output_file(); syscall_exit(2);
    }

    r = m3_CallV(fn);
    if (r) {
        /* Trap or runtime error — flush stdout, exit code 3. */
        write_output_file();
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); syscall_exit(3);
    }

    /* Success — flush captured stdout. */
    write_output_file();
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    free(mod_bytes);
    syscall_exit(0);
}
