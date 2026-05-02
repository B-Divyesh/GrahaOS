// user/snapshot.c — Phase 24 W19.6.
//
// Userspace CLI for the COW-snapshot syscall surface. Subcommands:
//
//   snapshot [--global] [<name>]   — SYS_SNAP_CREATE; prints the handle
//   restore <handle>               — SYS_SNAP_RESTORE
//   snap-delete <handle>           — SYS_SNAP_DELETE
//   snapshots                      — SYS_SNAP_LIST: table of live snaps
//
// gash dispatches each subcommand to /bin/snapshot with its own argv[0]
// (gash's existing built-in shim copies the original verb to argv[0] so
// this binary can branch on argv[0] basename). For shells that don't do
// that we fall back to argv[1] dispatch.
//
// All output is plain ASCII; the table fits 80 columns. No malloc — the
// snapshot list buffer is a fixed 32-record array on the stack.

#include "syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Tiny string helpers (no libc-string dependency in this binary).
// ---------------------------------------------------------------------------
static int eq_(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (uint8_t)*a == (uint8_t)*b;
}

static const char *basename_(const char *p) {
    const char *last = p;
    for (const char *s = p; *s; s++) if (*s == '/') last = s + 1;
    return last;
}

static int parse_uint_(const char *s, uint32_t *out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand implementations.
// ---------------------------------------------------------------------------
static int cmd_create(uint32_t scope, const char *name) {
    long h = syscall_snap_create(scope, name);
    if (h < 0) {
        printf("snapshot: create failed (rc=%ld)\n", h);
        return 1;
    }
    printf("snapshot: created handle=%ld scope=0x%x name='%s'\n",
           h, (unsigned)scope, name ? name : "");
    return 0;
}

static int cmd_restore(uint32_t handle) {
    long rc = syscall_snap_restore(handle);
    if (rc == -38 /* ENOSYS */) {
        printf("snapshot: restore not yet implemented (W16 deferred)\n");
        return 2;
    }
    if (rc < 0) {
        printf("snapshot: restore handle=%u failed (rc=%ld)\n",
               (unsigned)handle, rc);
        return 1;
    }
    printf("snapshot: restored handle=%u\n", (unsigned)handle);
    return 0;
}

static int cmd_delete(uint32_t handle) {
    long rc = syscall_snap_delete(handle);
    if (rc < 0) {
        printf("snapshot: delete handle=%u failed (rc=%ld)\n",
               (unsigned)handle, rc);
        return 1;
    }
    printf("snapshot: deleted handle=%u\n", (unsigned)handle);
    return 0;
}

static const char *state_name_(uint32_t state) {
    switch (state) {
        case SNAP_STATE_ACTIVE:    return "active";
        case SNAP_STATE_RESTORING: return "restore";
        case SNAP_STATE_DELETED:   return "deleted";
        default:                   return "?";
    }
}

static int cmd_list(void) {
    snap_info_user_t buf[32];
    long n = syscall_snap_list(buf, 32);
    if (n < 0) {
        printf("snapshot: list failed (rc=%ld)\n", n);
        return 1;
    }
    if (n == 0) {
        printf("snapshot: no live snapshots\n");
        return 0;
    }
    printf("ID    PID    STATE    SCOPE  TASKS  VMOS  CHANS  PAGES_S  PAGES_D  NAME\n");
    for (long i = 0; i < n; i++) {
        snap_info_user_t *s = &buf[i];
        printf("%-5lu %-6d %-8s 0x%-4x %-6u %-5u %-6u %-8lu %-8lu %s\n",
               (unsigned long)s->id,
               s->creator_pid,
               state_name_(s->state),
               (unsigned)s->scope_flags,
               (unsigned)s->task_count,
               (unsigned)s->vmo_count,
               (unsigned)s->chan_count,
               (unsigned long)s->pages_shared,
               (unsigned long)s->pages_diverged,
               s->name);
    }
    printf("(%ld snapshot%s)\n", n, n == 1 ? "" : "s");
    return 0;
}

// ---------------------------------------------------------------------------
// Usage banner.
// ---------------------------------------------------------------------------
static void usage(void) {
    printf("Usage: snapshot <subcommand> [args]\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  snapshot [--global] [<name>]   create a snapshot (SCOPE_SELF default)\n");
    printf("  restore <handle>               roll back to a snapshot\n");
    printf("  snap-delete <handle>           release a snapshot\n");
    printf("  snapshots                      list live snapshots\n");
    printf("\n");
    printf("Capability rights granted to the creator: INSPECT|REVOKE|DERIVE|\n");
    printf("RESTORE|DELETE. Sub-tokens may diminish RESTORE/DELETE.\n");
    printf("\n");
    printf("Note: the capture machinery (W14.1-W14.7) is deferred — created\n");
    printf("snapshots have empty task/vmo/chan/pin payloads today, and\n");
    printf("restore returns -ENOSYS until W16.\n");
}

// ---------------------------------------------------------------------------
// _start: dispatch by argv[0] basename first (when invoked as 'snapshots',
// 'restore', 'snap-delete'), then by argv[1] verb.
// ---------------------------------------------------------------------------
void _start(int argc, char **argv) {
    const char *prog = (argc >= 1 && argv[0]) ? basename_(argv[0]) : "snapshot";

    // Symlink-style dispatch: program invoked as 'snapshots' / 'restore' /
    // 'snap-delete' acts directly without needing a subcommand.
    if (eq_(prog, "snapshots")) {
        syscall_exit(cmd_list());
    }
    if (eq_(prog, "restore")) {
        if (argc < 2) {
            printf("Usage: restore <handle>\n");
            syscall_exit(1);
        }
        uint32_t h = 0;
        if (parse_uint_(argv[1], &h) != 0) {
            printf("restore: invalid handle '%s'\n", argv[1]);
            syscall_exit(1);
        }
        syscall_exit(cmd_restore(h));
    }
    if (eq_(prog, "snap-delete")) {
        if (argc < 2) {
            printf("Usage: snap-delete <handle>\n");
            syscall_exit(1);
        }
        uint32_t h = 0;
        if (parse_uint_(argv[1], &h) != 0) {
            printf("snap-delete: invalid handle '%s'\n", argv[1]);
            syscall_exit(1);
        }
        syscall_exit(cmd_delete(h));
    }

    // Default: argv[0] == 'snapshot'. Two forms:
    //   1. `snapshot <subcommand> ...`
    //   2. `snapshot [--global] [<name>]` (no subcommand → create)
    if (argc < 2) {
        // No args → create unnamed SCOPE_SELF snapshot.
        syscall_exit(cmd_create(SNAP_SCOPE_SELF, NULL));
    }

    // Subcommand?
    if (eq_(argv[1], "list") || eq_(argv[1], "snapshots")) {
        syscall_exit(cmd_list());
    }
    if (eq_(argv[1], "restore")) {
        if (argc < 3) { printf("Usage: snapshot restore <handle>\n"); syscall_exit(1); }
        uint32_t h = 0;
        if (parse_uint_(argv[2], &h) != 0) {
            printf("snapshot restore: invalid handle '%s'\n", argv[2]);
            syscall_exit(1);
        }
        syscall_exit(cmd_restore(h));
    }
    if (eq_(argv[1], "delete") || eq_(argv[1], "snap-delete")) {
        if (argc < 3) { printf("Usage: snapshot delete <handle>\n"); syscall_exit(1); }
        uint32_t h = 0;
        if (parse_uint_(argv[2], &h) != 0) {
            printf("snapshot delete: invalid handle '%s'\n", argv[2]);
            syscall_exit(1);
        }
        syscall_exit(cmd_delete(h));
    }
    if (eq_(argv[1], "help") || eq_(argv[1], "--help") || eq_(argv[1], "-h")) {
        usage();
        syscall_exit(0);
    }

    // Otherwise treat as `snapshot [--global] [<name>]` create.
    uint32_t scope = SNAP_SCOPE_SELF;
    int idx = 1;
    if (eq_(argv[idx], "--global")) {
        scope = SNAP_SCOPE_GLOBAL;
        idx++;
    }
    const char *name = (idx < argc) ? argv[idx] : NULL;
    syscall_exit(cmd_create(scope, name));
}
