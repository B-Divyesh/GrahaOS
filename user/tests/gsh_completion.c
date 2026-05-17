// user/tests/gsh_completion.c
//
// Phase 28 Session G.4.b — gsh tab-completion gate.  Five asserts:
//   1. sentinel /.gsh-script written successfully
//   2. spawn bin/gsh returns positive PID
//   3. gsh exit status is 0 (script mode completed cleanly)
//   4. /gsh_complete_out contains "cap-list" (built-in completion)
//   5. /gsh_complete_out contains "SYS_CHAN_CREATE" (manifest completion)

#include "../libtap.h"
#include "../syscalls.h"
#include <string.h>

extern int printf(const char *fmt, ...);

#define SENTINEL_PATH "/.gsh-script"
#define OUT_PATH      "/gsh_complete_out"

static int write_file(const char *path, const char *content, int len) {
    // FU24.A class flake mitigation: retry up to 3× on partial write.
    // Channel-mode FS in TCG occasionally returns short writes when
    // kheap is under load; the kernel handles it correctly, but the
    // user-side ssize_t == len check is over-strict.
    for (int attempt = 0; attempt < 3; attempt++) {
        syscall_create(path, 0);
        int fd = syscall_open(path);
        if (fd < 0) continue;
        (void)syscall_truncate(fd);
        ssize_t n = syscall_write(fd, content, (size_t)len);
        syscall_close(fd);
        if (n == len) return 0;
    }
    return -1;
}

static int file_contains(const char *path, const char *needle) {
    int fd = syscall_open(path);
    if (fd < 0) return 0;
    static char buf[4096];
    ssize_t n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    int nlen = (int)strlen(needle);
    for (int i = 0; i + nlen <= n; i++) {
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            if (buf[i + j] != needle[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

void _start(void) {
    tap_plan(5);

    // Clean any stale state.
    {
        int fd = syscall_open(SENTINEL_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
        fd = syscall_open(OUT_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
    }

    // Two completion probes — one builtin ("cap-l\t" → "cap-list"),
    // one syscall name from manifest ("SYS_STREAM_C\t" → "SYS_STREAM_CREATE").
    // No trailing `exit` — gsh's script mode exits naturally on EOF AFTER
    // flushing the completion buffer.  A literal `exit` would short-
    // circuit syscall_exit BEFORE the flush.
    const char script[] = "cap-l\t\nSYS_STREAM_C\t\n";
    int wr = write_file(SENTINEL_PATH, script, (int)sizeof(script) - 1);
    TAP_ASSERT(wr == 0, "1. sentinel /.gsh-script staged");

    int pid = syscall_spawn("bin/gsh");
    if (pid <= 0) printf("# spawn bin/gsh rc=%d\n", pid);
    TAP_ASSERT(pid > 0, "2. spawn bin/gsh returns positive PID");

    int status = -1;
    syscall_wait(&status);
    if (status != 0) printf("# gsh exit status=%d\n", status);
    TAP_ASSERT(status == 0, "3. gsh exits cleanly after script mode");

    int has_cap_list = file_contains(OUT_PATH, "cap-list");
    if (!has_cap_list) printf("# /gsh_complete_out missing 'cap-list'\n");
    TAP_ASSERT(has_cap_list,
               "4. completion output contains 'cap-list' (builtin)");

    int has_stream_create = file_contains(OUT_PATH, "SYS_STREAM_CREATE");
    if (!has_stream_create) printf("# /gsh_complete_out missing 'SYS_STREAM_CREATE'\n");
    TAP_ASSERT(has_stream_create,
               "5. completion output contains 'SYS_STREAM_CREATE' (manifest)");

    tap_done();
    syscall_exit(0);
}
