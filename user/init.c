// user/init.c — Phase 21 init / supervisor.
//
// Becomes PID 1 when the kernel boots with cmdline `autorun=init`. Reads
// `/etc/init.conf` (a tiny key=value text file) to learn which daemons to
// spawn before the interactive shell. Each daemon line:
//
//     daemon=<binary>              # spawn this binary
//     daemon=<binary>:<pledge_csv> # spawn with explicit pledge bitmap
//
// The single non-daemon line is:
//
//     autorun=<binary>             # spawn this as the interactive child;
//                                  # init exits when it exits (which triggers
//                                  # kernel_shutdown via autorun_on_init_exit)
//
// Backward-compat: if /etc/init.conf is absent, init defaults to spawning
// `/bin/gash` as the interactive child with no daemons.
//
// Crash-loop policy: a daemon that exits 3 times in 30 seconds is delayed
// 5 seconds before each subsequent respawn, to keep init from burning CPU
// on a permanently broken daemon.

#include "syscalls.h"
#include "libc/include/stdio.h"
#include "libc/include/string.h"
#include "libc/include/stdlib.h"

#define MAX_DAEMONS    8
#define INIT_CONF_PATH "etc/init.conf"
#define DEFAULT_AUTORUN_BIN "bin/gash"

typedef struct {
    char     path[64];        // e.g., "bin/e1000d"
    uint16_t pledge_mask;     // bitmap from CSV (0 = inherit parent's PLEDGE_ALL)
    int      pid;             // current child pid (-1 if dead)
    int      crash_count;     // crashes in current 30 s window
    uint64_t window_start_ms; // start of current 30 s window
    uint64_t backoff_until_ms;// don't respawn before this absolute ms
} daemon_t;

static daemon_t s_daemons[MAX_DAEMONS];
static int      s_daemon_count = 0;
static char     s_autorun_bin[64] = DEFAULT_AUTORUN_BIN;
static int      s_autorun_pid = -1;

// Crude wall-clock ms approximation. We don't have a SYS_GETTICKS userspace
// wrapper exposed yet (it's in SYS_GET_SYSTEM_STATE per Phase 8a). For
// init's coarse 30-second crash window we approximate by counting our own
// supervisor loop iterations: each wait blocks at least 100 ms so we
// increment a logical clock by 100 ms per loop. Good enough for backoff.
static uint64_t s_logical_ms = 0;

// ---------------------------------------------------------------------------
// Pledge name → bit mapping. Mirrors kernel/cap/pledge.h.
// ---------------------------------------------------------------------------
static uint16_t parse_pledge_csv(const char *csv) {
    uint16_t mask = 0;
    const char *p = csv;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        if (len == 7 && !memcmp(start, "fs_read", 7))     mask |= PLEDGE_FS_READ;
        else if (len == 8 && !memcmp(start, "fs_write", 8)) mask |= PLEDGE_FS_WRITE;
        else if (len == 10 && !memcmp(start, "net_client", 10)) mask |= PLEDGE_NET_CLIENT;
        else if (len == 10 && !memcmp(start, "net_server", 10)) mask |= PLEDGE_NET_SERVER;
        else if (len == 5 && !memcmp(start, "spawn", 5))   mask |= PLEDGE_SPAWN;
        else if (len == 8 && !memcmp(start, "ipc_send", 8)) mask |= PLEDGE_IPC_SEND;
        else if (len == 8 && !memcmp(start, "ipc_recv", 8)) mask |= PLEDGE_IPC_RECV;
        else if (len == 9 && !memcmp(start, "sys_query", 9)) mask |= PLEDGE_SYS_QUERY;
        else if (len == 11 && !memcmp(start, "sys_control", 11)) mask |= PLEDGE_SYS_CONTROL;
        else if (len == 7 && !memcmp(start, "ai_call", 7))  mask |= PLEDGE_AI_CALL;
        else if (len == 7 && !memcmp(start, "compute", 7))  mask |= PLEDGE_COMPUTE;
        else if (len == 4 && !memcmp(start, "time", 4))     mask |= PLEDGE_TIME;
        else if (len == 14 && !memcmp(start, "storage_server", 14)) mask |= PLEDGE_STORAGE_SERVER;
        else if (len == 12 && !memcmp(start, "input_server", 12))   mask |= PLEDGE_INPUT_SERVER;
        if (*p == ',') p++;
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Parse a single config line. Modifies the line buffer in place.
// ---------------------------------------------------------------------------
static void process_config_line(char *line) {
    // Strip leading whitespace + skip blanks/comments.
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#' || *line == '\n') return;
    // Strip trailing newline.
    char *end = line;
    while (*end && *end != '\n' && *end != '\r') end++;
    *end = '\0';

    if (!memcmp(line, "autorun=", 8)) {
        const char *val = line + 8;
        size_t i = 0;
        while (val[i] && i < sizeof(s_autorun_bin) - 1) {
            s_autorun_bin[i] = val[i]; i++;
        }
        s_autorun_bin[i] = '\0';
    } else if (!memcmp(line, "daemon=", 7)) {
        if (s_daemon_count >= MAX_DAEMONS) return;
        const char *val = line + 7;
        // Split on optional ':' to separate path from pledge CSV.
        const char *colon = val;
        while (*colon && *colon != ':') colon++;
        size_t path_len = (size_t)(colon - val);
        if (path_len >= sizeof(s_daemons[0].path)) return;
        daemon_t *d = &s_daemons[s_daemon_count++];
        for (size_t i = 0; i < path_len; i++) d->path[i] = val[i];
        d->path[path_len] = '\0';
        d->pledge_mask = (*colon == ':') ? parse_pledge_csv(colon + 1) : 0;
        d->pid = -1;
        d->crash_count = 0;
        d->window_start_ms = 0;
        d->backoff_until_ms = 0;
    }
}

// ---------------------------------------------------------------------------
// Read /etc/init.conf into memory and parse line-by-line.
// ---------------------------------------------------------------------------
static void load_config(void) {
    int fd = syscall_open(INIT_CONF_PATH);
    if (fd < 0) {
        // No config — sane defaults: just spawn gash.
        return;
    }
    static char buf[2048];
    ssize_t n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line_start = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            process_config_line(line_start);
            line_start = p + 1;
        }
    }
    if (line_start < buf + n) process_config_line(line_start);
}

// ---------------------------------------------------------------------------
// Spawn a daemon with optional pledge bitmap. We use SYS_SPAWN_EX with
// pledge_subset narrowing applied via spawn_rlimits_t (Phase 20 added the
// rlimit override; Phase 21 reuses the same syscall path — pledge inheritance
// is binary AND with parent which is already PLEDGE_ALL for init, so any
// subset is granted).
//
// For Phase 21 MVP: spawn_rlimits_t doesn't carry a pledge_subset field
// (Phase 20 didn't add one). We just use SYS_SPAWN — the daemon inherits
// PLEDGE_ALL from init and can call SYS_PLEDGE itself to narrow at startup.
// Future work: extend spawn_rlimits_t with a pledge_set field so init can
// narrow at spawn time.
// ---------------------------------------------------------------------------
static int spawn_daemon(daemon_t *d) {
    int pid = syscall_spawn(d->path);
    if (pid > 0) {
        d->pid = pid;
        printf("[init] spawn %s pid=%d pledge=0x%04x\n",
               d->path, pid, (unsigned)d->pledge_mask);
    } else {
        printf("[init] spawn %s FAILED rc=%d\n", d->path, pid);
    }
    return pid;
}

// ---------------------------------------------------------------------------
// Crash-loop check: if a daemon has crashed 3+ times in the last 30 s, set
// backoff_until_ms = now + 5s. If still in backoff, return false (don't
// respawn yet); else clear backoff and return true.
// ---------------------------------------------------------------------------
static int should_respawn(daemon_t *d) {
    if (s_logical_ms < d->backoff_until_ms) return 0;
    // Sliding 30 s window.
    if (s_logical_ms - d->window_start_ms > 30000) {
        d->window_start_ms = s_logical_ms;
        d->crash_count = 0;
    }
    d->crash_count++;
    if (d->crash_count > 3) {
        d->backoff_until_ms = s_logical_ms + 5000;
        printf("[init] daemon %s crash-loop detected; backoff 5s\n", d->path);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Main loop. _start runs through:
//   1. Load /etc/init.conf
//   2. Spawn each declared daemon
//   3. Spawn the interactive autorun binary
//   4. Loop on syscall_wait(); on child exit:
//        - If autorun child died → exit init (kernel triggers shutdown)
//        - If a daemon died → respawn with crash-loop backoff
// ---------------------------------------------------------------------------
void _start(void) {
    printf("[init] Phase 21 supervisor up; pid=1\n");
    load_config();

    for (int i = 0; i < s_daemon_count; i++) {
        spawn_daemon(&s_daemons[i]);
    }

    s_autorun_pid = syscall_spawn(s_autorun_bin);
    if (s_autorun_pid <= 0) {
        printf("[init] FATAL: autorun=%s failed (rc=%d); exiting\n",
               s_autorun_bin, s_autorun_pid);
        syscall_exit(1);
    }
    printf("[init] autorun=%s pid=%d\n", s_autorun_bin, s_autorun_pid);

    // Supervision loop.
    while (1) {
        int status = 0;
        int wpid = syscall_wait(&status);
        s_logical_ms += 100;  // approximate clock — 1 wait ≈ 100 ms

        if (wpid <= 0) {
            // No children left; sleep briefly to avoid tight loop.
            // (syscall_wait returns -1 when no children remain.)
            for (volatile int j = 0; j < 5000000; j++) {}
            continue;
        }

        if (wpid == s_autorun_pid) {
            printf("[init] autorun pid=%d exited (status=%d); shutting down\n",
                   wpid, status);
            syscall_exit(status);
        }

        // Find the matching daemon and respawn.
        for (int i = 0; i < s_daemon_count; i++) {
            daemon_t *d = &s_daemons[i];
            if (d->pid == wpid) {
                printf("[init] daemon %s pid=%d exited (status=%d)\n",
                       d->path, wpid, status);
                d->pid = -1;
                if (should_respawn(d)) {
                    spawn_daemon(d);
                }
                break;
            }
        }
    }
}
