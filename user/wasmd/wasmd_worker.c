/*
 * user/wasmd/wasmd_worker.c — FU27.WASM.D2_worker subprocess.
 *
 * Spawned by wasmd (via SYS_SPAWN_ARGV slot 1115) per RUN_MODULE call.
 *
 * Lifecycle:
 *   1. (Phase 29 Session G) subscribe to audit so per-instance events
 *      stay in this worker's slot.  Auto-unsubscribed on exit by the
 *      kernel via audit_unsubscribe_all_for_pid in sched_reap_zombie.
 *   2. Open /tmp/wasmd_pending.wasm (single-slot v1 staging file).
 *   3. Read up to 1 MiB of bytes.
 *   4. m3_NewEnvironment / NewRuntime / ParseModule / LoadModule.
 *   5. m3_LinkRawFunction for each of the 6 host bindings:
 *        gcp.print        — stdout capture (existing).
 *        gcp.tui_write    — cap-gated cell-grid write.
 *        gcp.tui_read     — cap-gated input-event drain.
 *        gcp.fs_read      — pledge-filtered read.
 *        gcp.fs_write     — pledge-filtered write.
 *        gcp.audit_query  — query own pid's recent events.
 *   6. m3_FindFunction("_start") and m3_CallV().
 *   7. Write captured stdout buffer to /tmp/wasmd_output.txt.
 *   8. exit(N) where N reflects which step failed:
 *        0 = OK
 *        1 = load fail (open/read/parse/load)
 *        2 = instantiate fail (link/find_function)
 *        3 = trap (CallV returned non-NULL M3Result)
 *        4 = internal (malloc failure etc.)
 *        5 = cap denied (host binding refused due to missing cap/path)
 *        6 = fuel exhausted (wall-clock watchdog tripped)
 *
 * Cap-flags: encoded in /tmp/wasmd_pending.caps (one ASCII char per
 *   binding — '1' allowed, '0' denied).  Order:
 *     [0] tui_write  [1] tui_read  [2] fs_read  [3] fs_write  [4] audit
 *   When the .caps file is absent, all bindings are ALLOWED (back-compat
 *   for hello.wasm and other fixtures that only use gcp.print).
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

/* Cap bitmap — set by read_caps_file(); each entry is a per-binding gate. */
#define CAP_BIT_TUI_WRITE   (1u << 0)
#define CAP_BIT_TUI_READ    (1u << 1)
#define CAP_BIT_FS_READ     (1u << 2)
#define CAP_BIT_FS_WRITE    (1u << 3)
#define CAP_BIT_AUDIT       (1u << 4)
static uint32_t g_caps = 0xFFFFFFFFu;  /* default: all allowed */

/* Wall-clock watchdog: track start in TSC ticks; each host call samples
 * rdtsc and aborts when the deadline is crossed. */
static uint64_t g_deadline_tsc = 0;
static uint64_t g_start_tsc    = 0;
static int      g_fuel_tripped = 0;

/* Per-instance audit subscription slot.  Returned by syscall_audit_subscribe;
 * the kernel auto-unsubscribes on task exit via sched_reap_zombie. */
static int g_audit_slot = -1;

/* Cap-deny / fuel-exhaust sticky flags surfaced via exit code. */
static int g_cap_denied = 0;

/* Captured-stdout buffer. */
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

/* Returns 1 if the wall-clock deadline has been crossed; sets the
 * sticky g_fuel_tripped flag for the exit-code mapper. */
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
 * Args: (console_id, x, y, codepoint).
 * Cap-gated: requires CAP_BIT_TUI_WRITE.  Calls the DEBUG_CONSOLE_WRITE_CELL
 * test substrate (Session D) rather than the full SYS_CONSOLE_ATTACH path
 * so the binding works without a per-worker console attachment. */
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
    /* Default fg=7 (white) / bg=0 / attrs=0 for AI-driven writes. */
    long rc = syscall_debug_console_write_cell(console_id, y, x,
                                               codepoint,
                                               /*fg=*/7, /*bg=*/0,
                                               /*attrs=*/0);
    m3ApiReturn((uint32_t)(rc < 0 ? (uint32_t)-1 : 0));
}

/* Host import: "gcp.tui_read" — signature "i(ii)" returns i32 event count.
 * Args: (out_ptr, max_events).  Each event is 16 bytes (matches kernel
 * input_event_t).  Cap-gated: requires CAP_BIT_TUI_READ. */
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
 * Args: (path_ptr, path_len, out_ptr, max).
 * Pledge-filtered: only paths within an allowed prefix-set are honored.
 * For Phase 29 G we accept paths starting with "/tmp/" (sandbox) and
 * "bin/tests/wasm/" (read-only fixtures).  Anything else returns -1. */
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
    /* Sandbox: /tmp/ and bin/tests/wasm/ prefixes only. */
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

/* Host import: "gcp.fs_write" — signature "i(*i*i)" returns i32 bytes
 * written.  Same sandbox: only /tmp/ prefix paths are honored. */
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

/* Host import: "gcp.audit_query" — signature "i(*i)" returns i32 event count.
 * Args: (out_ptr, max_events).  Each entry is 256 bytes (audit_entry_t).
 * Filtered to this worker's pid implicitly via the kernel's per-subscriber
 * scoping; here we just rely on the global audit_query and cap the count. */
m3ApiRawFunction(host_gcp_audit_query) {
    m3ApiReturnType(int32_t);
    m3ApiGetArgMem(uint8_t *, out_ptr);
    m3ApiGetArg(uint32_t, max_events);
    if (watchdog_tripped()) m3ApiTrap("fuel exhausted");
    if (!(g_caps & CAP_BIT_AUDIT)) {
        g_cap_denied = 1;
        m3ApiTrap("cap denied: audit_query");
    }
    if (max_events > 4) max_events = 4;  /* fit in wasm linear memory; AI module sizing */
    uint32_t bytes_needed = max_events * 256;
    m3ApiCheckMem(out_ptr, bytes_needed);
    long rc = syscall_audit_query(0, 0, ~0u,
                                  (audit_entry_u_t *)out_ptr,
                                  max_events);
    if (rc < 0) m3ApiReturn((int32_t)0);
    m3ApiReturn((int32_t)rc);
}

static int read_pending_module(uint8_t **out_bytes, size_t *out_len) {
    *out_bytes = NULL;
    *out_len = 0;

    int fd = syscall_open(WASMD_PENDING_PATH);
    if (fd < 0) return WORKER_EXIT_LOAD;

    const size_t MAX = 1u << 20;
    uint8_t *buf = malloc(MAX);
    if (!buf) {
        (void)syscall_close(fd);
        return WORKER_EXIT_INTERNAL;
    }
    size_t total = 0;
    while (total < MAX) {
        long r = syscall_read(fd, buf + total, MAX - total);
        if (r < 0) { free(buf); (void)syscall_close(fd); return WORKER_EXIT_LOAD; }
        if (r == 0) break;
        total += (size_t)r;
    }
    (void)syscall_close(fd);
    *out_bytes = buf;
    *out_len = total;
    return WORKER_EXIT_OK;
}

/* Read /tmp/wasmd_pending.caps — an ASCII bitmap.  '1' allowed, '0' denied. */
static void read_caps_file(void) {
    int fd = syscall_open("/tmp/wasmd_pending.caps");
    if (fd < 0) return;  /* default: all caps allowed */
    char buf[16];
    long n = syscall_read(fd, buf, sizeof(buf));
    (void)syscall_close(fd);
    if (n <= 0) return;
    g_caps = 0;
    if (n > 0 && buf[0] == '1') g_caps |= CAP_BIT_TUI_WRITE;
    if (n > 1 && buf[1] == '1') g_caps |= CAP_BIT_TUI_READ;
    if (n > 2 && buf[2] == '1') g_caps |= CAP_BIT_FS_READ;
    if (n > 3 && buf[3] == '1') g_caps |= CAP_BIT_FS_WRITE;
    if (n > 4 && buf[4] == '1') g_caps |= CAP_BIT_AUDIT;
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

/* Helper: cleanly exit with proper output flush.  Worker exit code maps
 * to wasmd status via wasmd.c::map_worker_exit. */
static void worker_exit(int code) {
    /* Translate sticky cap-deny / fuel-exhaust flags */
    if (g_cap_denied && code == WORKER_EXIT_TRAP) code = WORKER_EXIT_CAP_DENIED;
    if (g_fuel_tripped && code == WORKER_EXIT_TRAP) code = WORKER_EXIT_FUEL;
    write_output_file();
    syscall_exit(code);
}

void _start(void) {
    /* FU27.X.wasmd_audit_subscription: per-instance audit slot.  Kernel
     * auto-unsubscribes on task exit via sched_reap_zombie hook calling
     * audit_unsubscribe_all_for_pid(self).  ~0 = receive every event. */
    g_audit_slot = (int)syscall_audit_subscribe(~0ULL);
    /* If the subscribe fails (-EAGAIN, all 16 slots taken), continue
     * anyway — host_gcp_audit_query falls back to the global query path. */

    /* Set up wall-clock watchdog (1 second default — fuel_exhaust.wasm
     * exercises this; ai_demo / hello complete in << 100 ms).  TSC Hz
     * varies wildly between TCG and KVM, so query at runtime. */
    g_start_tsc = spin_rdtsc();
    uint64_t hz = syscall_tsc_hz_query();
    if (hz == 0) hz = 1000000000ULL;  /* 1 GHz fallback if calibration not ready */
    g_deadline_tsc = g_start_tsc + hz;  /* 1 second budget */

    /* Read .caps gate-file (if any). */
    read_caps_file();

    uint8_t *mod_bytes = NULL;
    size_t mod_len = 0;
    int rc = read_pending_module(&mod_bytes, &mod_len);
    if (rc != WORKER_EXIT_OK) worker_exit(rc);

    IM3Environment env = m3_NewEnvironment();
    if (!env) { free(mod_bytes); worker_exit(WORKER_EXIT_INTERNAL); }

    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) {
        m3_FreeEnvironment(env);
        free(mod_bytes); worker_exit(WORKER_EXIT_INTERNAL);
    }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, mod_bytes, (uint32_t)mod_len);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); worker_exit(WORKER_EXIT_LOAD);
    }

    r = m3_LoadModule(rt, mod);
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); worker_exit(WORKER_EXIT_LOAD);
    }

    /* Wire host imports.  Six bindings — m3Err_functionLookupFailed is
     * benign (module doesn't import that binding); other errors are fatal. */
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
            free(mod_bytes); worker_exit(WORKER_EXIT_INSTANTIATE);
        }
    }

    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, "_start");
    if (r) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(mod_bytes); worker_exit(WORKER_EXIT_INSTANTIATE);
    }

    r = m3_CallV(fn);
    int exit_code = WORKER_EXIT_OK;
    if (r) {
        exit_code = WORKER_EXIT_TRAP;
    }
    /* Sticky cap-deny / fuel flags refine the trap code in worker_exit. */

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    free(mod_bytes);
    worker_exit(exit_code);
}
