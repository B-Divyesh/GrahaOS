/*
 * user/wasmd/wasmd_worker.c — FU27.WASM.D2_worker / FU29.X.wasmd_subprocess.
 *
 * A KILLABLE wasm execution subprocess.  wasmd spawns this (via
 * SYS_SPAWN_ARGV) for any module that must be crash/runaway-isolated — i.e.
 * a module that could loop forever (m3_CallV never returns) and would hang
 * the in-process daemon.  wasmd enforces a wall-clock deadline and SIGKILLs
 * the worker; because the worker holds NO kernel lock at that point (see
 * "TRANSPORT" below), the kill is orphan-free.
 *
 * TRANSPORT (the crux of the orphan-free guarantee):
 *   The module bytes arrive HEX-ENCODED IN argv — NOT via the filesystem.
 *   The Phase 27/29 "vfs spinlock orphan" deadlock happened because the old
 *   file-transport worker read /tmp/wasmd_pending.wasm, holding the global
 *   vfs_lock for SECONDS across a channel-mode blk read; wasmd's deadline
 *   SIGKILL then landed WHILE the worker held vfs_lock → the lock was
 *   orphaned → the next acquirer spun the 5s budget → SPINLOCK PANIC.
 *   By passing the module via argv the worker makes ZERO filesystem syscalls,
 *   so at kill time it is always in a pure-userspace wasm loop (or wasm3
 *   setup) holding no kernel spinlock → reap is safe (kernel kill-safety
 *   analysis verdict (a)).
 *
 *   argv layout (seeded by the kernel SYS_SPAWN_ARGV path):
 *     argv[0] = "bin/wasmd_worker"  (path, by convention)
 *     argv[1] = caps bitmap, ASCII "tcrwa" ('1'=allowed) — 5 chars
 *     argv[2..argc-1] = hex chunks of the module bytes (each ≤240 chars to
 *                       stay under the kernel's 255-char-per-arg limit);
 *                       the worker concatenates + hex-decodes them.
 *
 * Lifecycle:
 *   1. Parse argv → caps + module bytes (no syscalls).
 *   2. m3 NewEnvironment / NewRuntime / ParseModule / LoadModule.
 *   3. m3_LinkRawFunction for the 6 host bindings (gcp.print / tui_write /
 *      tui_read / fs_read / fs_write / audit_query) — cap-gated.
 *   4. m3_FindFunction("_start") and m3_CallV().  A runaway module never
 *      returns here; wasmd's deadline SIGKILL terminates the worker.
 *   5. exit(N) where N reflects which step failed (see WORKER_EXIT_*).
 *
 * The worker writes NOTHING to the filesystem and opens NO channel; its
 * result surfaces purely through its exit code, which wasmd reaps.  For the
 * runaway/fuel fixtures the worker is killed before it can exit, and wasmd
 * maps the kill to WASMD_E_FUEL_EXHAUSTED.
 */

#include "../syscalls.h"
#include "proto.h"
#include "wasm3.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Worker-side exit codes — keep in sync with wasmd.c::map_worker_exit. */
#define WORKER_EXIT_OK          0
#define WORKER_EXIT_LOAD        1
#define WORKER_EXIT_INSTANTIATE 2
#define WORKER_EXIT_TRAP        3
#define WORKER_EXIT_INTERNAL    4
#define WORKER_EXIT_CAP_DENIED  5
#define WORKER_EXIT_FUEL        6
#define WORKER_EXIT_BADARGS     7

/* Cap bitmap — set from argv[1]; each entry is a per-binding gate. */
#define CAP_BIT_TUI_WRITE   (1u << 0)
#define CAP_BIT_TUI_READ    (1u << 1)
#define CAP_BIT_FS_READ     (1u << 2)
#define CAP_BIT_FS_WRITE    (1u << 3)
#define CAP_BIT_AUDIT       (1u << 4)
static uint32_t g_caps = 0xFFFFFFFFu;  /* default: all allowed */

/* Self-watchdog disabled in the subprocess model: wasmd owns the deadline
 * and SIGKILLs us.  Left as 0 so watchdog_tripped() is a cheap no-op (the
 * runaway fixtures make no host calls, so a self-watchdog can't fire anyway). */
static uint64_t g_deadline_tsc = 0;
static int      g_fuel_tripped = 0;

/* Cap-deny sticky flag surfaced via exit code. */
static int g_cap_denied = 0;

/* Captured-stdout buffer.  Not transported anywhere in the subprocess model
 * (the isolated path is for runaway modules whose stdout is irrelevant);
 * retained so the print binding stays well-formed. */
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

/* No-op in the subprocess model (g_deadline_tsc==0): wasmd's external SIGKILL
 * is the runaway-termination mechanism. */
static int watchdog_tripped(void) {
    if (g_deadline_tsc == 0) return 0;
    if (spin_rdtsc() >= g_deadline_tsc) {
        g_fuel_tripped = 1;
        return 1;
    }
    return 0;
}

/* Host import: "gcp.print" — signature "v(*i)" — print(ptr, len). */
m3ApiRawFunction(host_gcp_print) {
    m3ApiGetArgMem(const uint8_t *, str);
    m3ApiGetArg(uint32_t, len);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (len > 1024) len = 1024;
    m3ApiCheckMem(str, len);
    stdout_append(str, len);
    m3ApiSuccess();
}

/* Host import: "gcp.tui_write" — signature "i(iiii)" returns i32 status.
 * Args: (console_id, x, y, codepoint).  Cap-gated: CAP_BIT_TUI_WRITE. */
m3ApiRawFunction(host_gcp_tui_write) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, console_id);
    m3ApiGetArg(uint32_t, x);
    m3ApiGetArg(uint32_t, y);
    m3ApiGetArg(uint32_t, codepoint);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_TUI_WRITE)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: tui_write");
    }
    long rc = syscall_debug_console_write_cell(console_id, y, x,
                                               codepoint,
                                               /*fg=*/7, /*bg=*/0,
                                               /*attrs=*/0);
    m3ApiReturn((uint32_t)(rc < 0 ? (uint32_t)-1 : 0));
}

/* Host import: "gcp.tui_read" — signature "i(*i)" returns i32 event count. */
m3ApiRawFunction(host_gcp_tui_read) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max_events);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_TUI_READ)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: tui_read");
    }
    if (max_events > 16) max_events = 16;
    uint32_t bytes_needed = max_events * 16;
    m3ApiCheckMem(out_ptr, bytes_needed);
    long rc = syscall_console_read_input(/*console_id=*/0,
                                         (input_event_u_t *)out_ptr,
                                         max_events);
    if (rc < 0) m3ApiReturn((uint32_t)0);
    uint32_t count = (uint32_t)(rc & 0x3FFFFFFFu);
    m3ApiReturn(count);
}

/* Host import: "gcp.fs_read" — signature "i(*i*i)" returns i32 bytes read.
 * NOTE: this touches the vfs.  It is only reachable for NON-runaway modules
 * deliberately routed through the worker; the runaway fixtures wasmd sends
 * here make NO host calls, so this path is never exercised mid-kill. */
m3ApiRawFunction(host_gcp_fs_read) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const uint8_t *, path_ptr);
    m3ApiGetArg(uint32_t, path_len);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_FS_READ)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_read");
    }
    if (path_len == 0 || path_len > 255) m3ApiReturn((int32_t)-1);
    m3ApiCheckMem(path_ptr, path_len);
    m3ApiCheckMem(out_ptr, max);
    char path[256];
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    if (strncmp(path, "/tmp/", 5) != 0 &&
        strncmp(path, "bin/tests/wasm/", 15) != 0) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_read path outside sandbox");
    }
    int fd = syscall_open(path);
    if (fd < 0) m3ApiReturn((int32_t)-1);
    long n = syscall_read(fd, out_ptr, max);
    (void)syscall_close(fd);
    m3ApiReturn((int32_t)n);
}

/* Host import: "gcp.fs_write" — signature "i(*i*i)" returns i32 bytes written. */
m3ApiRawFunction(host_gcp_fs_write) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(const uint8_t *, path_ptr);
    m3ApiGetArg(uint32_t, path_len);
    m3ApiGetArgMem(const uint8_t *, content_ptr);
    m3ApiGetArg(uint32_t, len);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_FS_WRITE)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_write");
    }
    if (path_len == 0 || path_len > 255) m3ApiReturn((int32_t)-1);
    m3ApiCheckMem(path_ptr, path_len);
    m3ApiCheckMem(content_ptr, len);
    char path[256];
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    if (strncmp(path, "/tmp/", 5) != 0) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: fs_write path outside sandbox");
    }
    (void)syscall_create(path, 0644);
    int fd = syscall_open(path);
    if (fd < 0) m3ApiReturn((int32_t)-1);
    long n = syscall_write(fd, content_ptr, len);
    (void)syscall_close(fd);
    m3ApiReturn((int32_t)n);
}

/* Host import: "gcp.audit_query" — signature "i(*i)" returns i32 event count. */
m3ApiRawFunction(host_gcp_audit_query) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max_events);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_AUDIT)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: audit_query");
    }
    if (max_events > 4) max_events = 4;
    uint32_t bytes_needed = max_events * 256;
    m3ApiCheckMem(out_ptr, bytes_needed);
    long rc = syscall_audit_query(0, 0, ~0u,
                                  (audit_entry_u_t *)out_ptr,
                                  max_events);
    if (rc < 0) m3ApiReturn((int32_t)0);
    m3ApiReturn((int32_t)rc);
}

/* ----- argv transport: caps + hex-encoded module bytes (no FS) ----- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse caps argv string "tcrwa" ('1'=allow).  Absent/short → all allowed. */
static void parse_caps_arg(const char *s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    g_caps = 0;
    if (n > 0 && s[0] == '1') g_caps |= CAP_BIT_TUI_WRITE;
    if (n > 1 && s[1] == '1') g_caps |= CAP_BIT_TUI_READ;
    if (n > 2 && s[2] == '1') g_caps |= CAP_BIT_FS_READ;
    if (n > 3 && s[3] == '1') g_caps |= CAP_BIT_FS_WRITE;
    if (n > 4 && s[4] == '1') g_caps |= CAP_BIT_AUDIT;
}

/* Decode the concatenation of argv[2..argc-1] (hex) into *out.  Returns the
 * decoded byte count, or -1 on a malformed (odd-length / non-hex) stream. */
static long decode_hex_argv(int argc, char **argv, uint8_t *out, uint32_t out_cap) {
    uint32_t produced = 0;
    int have_hi = 0;
    int hi = 0;
    for (int a = 2; a < argc; a++) {
        const char *s = argv[a];
        if (!s) continue;
        for (uint32_t i = 0; s[i]; i++) {
            int v = hex_nibble(s[i]);
            if (v < 0) return -1;
            if (!have_hi) { hi = v; have_hi = 1; }
            else {
                if (produced >= out_cap) return -1;
                out[produced++] = (uint8_t)((hi << 4) | v);
                have_hi = 0;
            }
        }
    }
    if (have_hi) return -1;  /* odd number of hex digits */
    return (long)produced;
}

static void worker_exit(int code) {
    if (g_cap_denied && code == WORKER_EXIT_TRAP) code = WORKER_EXIT_CAP_DENIED;
    if (g_fuel_tripped && code == WORKER_EXIT_TRAP) code = WORKER_EXIT_FUEL;
    syscall_exit(code);
}

void _start(int argc, char **argv) {
    /* The kernel seeds argc in RDI, argv in RSI for SYS_SPAWN_ARGV children.
     * We require at least: argv[0]=path, argv[1]=caps, argv[2..]=hex bytes. */
    if (argc < 3) {
        syscall_exit(WORKER_EXIT_BADARGS);
    }

    parse_caps_arg(argv[1]);

    static uint8_t mod_bytes[WASMD_BYTES_MAX + 8];
    long mod_len = decode_hex_argv(argc, argv, mod_bytes, sizeof(mod_bytes));
    if (mod_len <= 0) {
        syscall_exit(WORKER_EXIT_BADARGS);
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env) worker_exit(WORKER_EXIT_INTERNAL);

    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) {
        m3_FreeEnvironment(env);
        worker_exit(WORKER_EXIT_INTERNAL);
    }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, mod_bytes, (uint32_t)mod_len);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        worker_exit(WORKER_EXIT_LOAD);
    }

    r = m3_LoadModule(rt, mod);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        worker_exit(WORKER_EXIT_LOAD);
    }

    struct binding {
        const char *mod;
        const char *fn;
        const char *sig;
        M3RawCall   impl;
    } bindings[] = {
        { "gcp", "print",       "v(*i)",   &host_gcp_print       },
        { "gcp", "tui_write",   "i(iiii)", &host_gcp_tui_write   },
        { "gcp", "tui_read",    "i(*i)",   &host_gcp_tui_read    },
        { "gcp", "fs_read",     "i(*i*i)", &host_gcp_fs_read     },
        { "gcp", "fs_write",    "i(*i*i)", &host_gcp_fs_write    },
        { "gcp", "audit_query", "i(*i)",   &host_gcp_audit_query },
    };
    for (size_t i = 0; i < sizeof(bindings)/sizeof(bindings[0]); i++) {
        M3Result lr = m3_LinkRawFunction(mod, bindings[i].mod,
                                         bindings[i].fn, bindings[i].sig,
                                         bindings[i].impl);
        if (lr && lr != m3Err_functionLookupFailed) {
            m3_FreeRuntime(rt);
            m3_FreeEnvironment(env);
            worker_exit(WORKER_EXIT_INSTANTIATE);
        }
    }

    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, "_start");
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        worker_exit(WORKER_EXIT_INSTANTIATE);
    }

    /* A runaway module never returns from m3_CallV — wasmd's deadline SIGKILL
     * terminates us here, in a pure-userspace loop holding no kernel lock. */
    r = m3_CallV(fn);
    int exit_code = (r == NULL) ? WORKER_EXIT_OK : WORKER_EXIT_TRAP;

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    worker_exit(exit_code);
}
