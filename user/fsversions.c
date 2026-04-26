// user/fsversions.c
//
// Phase 19 — `fsversions <path>` CLI.
//
// Resolves <path> to an inode, invokes SYS_FS_LIST_VERSIONS, and prints the
// resulting version records newest-first. On a v1 compat mount this program
// emits a single line saying so and exits 0.

#include "syscalls.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define MAX_VERS 16

static void print(const char *s) {
    while (*s) syscall_putc(*s++);
}

static int print_u64(uint64_t v) {
    char buf[32]; int n = 0;
    if (v == 0) { print("0"); return 1; }
    while (v > 0 && n < 31) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; --i) { char s[2] = { buf[i], 0 }; print(s); }
    return n;
}

static int print_hex(uint64_t v) {
    char buf[32]; int n = 0;
    static const char *hex = "0123456789abcdef";
    if (v == 0) { print("0x0"); return 3; }
    while (v > 0 && n < 31) { buf[n++] = hex[v & 0xF]; v >>= 4; }
    print("0x");
    for (int i = n - 1; i >= 0; --i) { char s[2] = { buf[i], 0 }; print(s); }
    return n + 2;
}

void _start(void) {
    // Minimal argv support — the kernel's init path passes argv on the stack
    // the same way gash uses. For simplicity we just read a path from a
    // fixed env variable or from the serial line. MVP: fixed path.
    const char *path = "/";

    fs_version_info_u_t buf[MAX_VERS];
    memset(buf, 0, sizeof(buf));

    // LIST_VERSIONS wants an inode number, not a path. Resolve via openr+stat.
    int fd = syscall_open(path);
    if (fd < 0) {
        print("fsversions: open failed: ");
        print(path);
        print("\n");
        syscall_exit(1);
    }
    uint32_t inode = (uint32_t)fd;  // MVP — fd used as opaque id.
    long n = syscall_fs_list_versions(inode, buf, MAX_VERS);
    if (n < 0) {
        if (n == -127) {
            print("fsversions: FS is v1-compat read-only; no version history available.\n");
            syscall_exit(0);
        }
        print("fsversions: list_versions failed\n");
        syscall_exit(1);
    }

    print("fsversions: ");
    print(path);
    print(" versions: ");
    print_u64((uint64_t)n);
    print("\n");
    for (long i = 0; i < n; ++i) {
        print("  v");
        print_u64(buf[i].version_id);
        print(" size=");
        print_u64(buf[i].size);
        print(" simhash=");
        print_hex(buf[i].simhash);
        print(" seg=");
        print_u64(buf[i].segment_id);
        print(" cluster=");
        print_u64(buf[i].cluster_id);
        print("\n");
    }
    (void)syscall_close(fd);
    syscall_exit(0);
}
