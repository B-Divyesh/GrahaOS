// user/ulimit.c
//
// Phase 20 — `ulimit get <pid> <resource>` / `ulimit set <pid> <resource>
// <value>` CLI.
//
// Wraps SYS_SETRLIMIT / SYS_GETRLIMIT. An unprivileged user (no
// PLEDGE_SYS_CONTROL) gets -EPLEDGE on `set` even for their own PID — limits
// are not self-manageable; only init-tier processes can mutate them.
//
// Resource names accepted: "mem" (pages), "cpu" (ns per 1s epoch), "io"
// (bytes per second).

#include "syscalls.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>

static void print(const char *s) {
    while (*s) syscall_putc(*s++);
}

static void print_u64(uint64_t v) {
    char buf[32]; int n = 0;
    if (v == 0) { print("0"); return; }
    while (v > 0 && n < 31) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; --i) { char s[2] = { buf[i], 0 }; print(s); }
}

static int parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s) return -1;
    uint64_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

static int parse_resource(const char *name) {
    if (!name) return -1;
    if (name[0] == 'm' && name[1] == 'e' && name[2] == 'm' && !name[3]) return RLIMIT_MEM;
    if (name[0] == 'c' && name[1] == 'p' && name[2] == 'u' && !name[3]) return RLIMIT_CPU;
    if (name[0] == 'i' && name[1] == 'o' && !name[2]) return RLIMIT_IO;
    return -1;
}

static const char *resource_name(unsigned res) {
    switch (res) {
    case RLIMIT_MEM: return "mem (pages)";
    case RLIMIT_CPU: return "cpu (ns/1s)";
    case RLIMIT_IO:  return "io (bytes/s)";
    default:         return "?";
    }
}

static void usage(void) {
    print("usage:\n"
          "  ulimit get <pid> <mem|cpu|io>\n"
          "  ulimit set <pid> <mem|cpu|io> <value>\n"
          "  pid=0 means self.\n");
}

// Minimal argv — gash passes args on the stack à la _start(argc, argv[]).
// We follow the same convention as other /bin utilities.
int main(int argc, char **argv);

void _start(int argc, char **argv) {
    int rc = main(argc, argv);
    syscall_exit(rc);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    uint64_t pid_u = 0;
    if (parse_u64(argv[2], &pid_u) != 0) {
        print("ulimit: bad pid\n");
        return 1;
    }
    int resource = parse_resource(argv[3]);
    if (resource < 0) {
        print("ulimit: unknown resource (expected mem|cpu|io)\n");
        return 1;
    }

    if (cmd[0] == 'g' && cmd[1] == 'e' && cmd[2] == 't' && !cmd[3]) {
        unsigned long out = 0;
        long r = syscall_getrlimit((unsigned)pid_u, (unsigned)resource, &out);
        if (r < 0) {
            print("ulimit get failed: errno=");
            print_u64((uint64_t)(-r));
            print("\n");
            return 1;
        }
        print("pid=");
        print_u64(pid_u);
        print(" ");
        print(resource_name((unsigned)resource));
        print(" = ");
        print_u64((uint64_t)out);
        print("\n");
        return 0;
    } else if (cmd[0] == 's' && cmd[1] == 'e' && cmd[2] == 't' && !cmd[3]) {
        if (argc < 5) {
            usage();
            return 1;
        }
        uint64_t value = 0;
        if (parse_u64(argv[4], &value) != 0) {
            print("ulimit: bad value\n");
            return 1;
        }
        long r = syscall_setrlimit((unsigned)pid_u, (unsigned)resource, value);
        if (r < 0) {
            if (r == -1) {
                // pledge denial surfaces as -1 in the legacy syscall path;
                // current errno convention is -EPLEDGE = -115 but some
                // syscalls round to -1. Print a user-friendly message.
                print("ulimit set: permission denied (need PLEDGE_SYS_CONTROL)\n");
            } else {
                print("ulimit set failed: errno=");
                print_u64((uint64_t)(-r));
                print("\n");
            }
            return 1;
        }
        print("ok\n");
        return 0;
    }

    usage();
    return 1;
}
