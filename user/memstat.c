// user/memstat.c — Phase 14: /bin/memstat allocator stats reader.
//
// Calls SYS_KHEAP_STATS, formats each entry as a human-readable or
// --json NDJSON line. Columns:
//   cache=<name> obj=<n>B in_use=<k> free=<m> pages=<p> subsys=<top-3>
// where "top-3" is the three subsystem ids with the highest in-use
// counters (enough for a quick read; full list available via --json).

#include <stdint.h>
#include "syscalls.h"
#include "../libc/include/stdio.h"
#include "../libc/include/string.h"

#define MEMSTAT_BUF_MAX 64

static const char *subsys_name(unsigned s) {
    switch (s) {
    case 0: return "core";
    case 1: return "mm";
    case 2: return "sched";
    case 3: return "syscall";
    case 4: return "vfs";
    case 5: return "fs";
    case 6: return "net";
    case 7: return "cap";
    case 8: return "drv";
    case 9: return "test";
    default: return "user";
    }
}

static void print_entry_human(const kheap_stats_entry_u_t *e) {
    printf("cache=%-16s obj=%-6uB in_use=%-5llu free=%-5llu pages=%-3u subsys=",
           e->name,
           (unsigned)e->object_size,
           (unsigned long long)e->in_use,
           (unsigned long long)e->free,
           (unsigned)e->pages);
    int first = 1;
    for (unsigned s = 0; s < 16; ++s) {
        if (e->subsys_counters[s] == 0) continue;
        if (!first) printf(",");
        printf("%s:%u", subsys_name(s), (unsigned)e->subsys_counters[s]);
        first = 0;
    }
    if (first) printf("-");
    printf("\n");
}

static void print_entry_json(const kheap_stats_entry_u_t *e) {
    printf("{\"name\":\"%s\",\"obj_size\":%u,\"in_use\":%llu,\"free\":%llu,\"pages\":%u,\"subsys\":{",
           e->name, (unsigned)e->object_size,
           (unsigned long long)e->in_use,
           (unsigned long long)e->free,
           (unsigned)e->pages);
    int first = 1;
    for (unsigned s = 0; s < 16; ++s) {
        if (e->subsys_counters[s] == 0) continue;
        if (!first) printf(",");
        printf("\"%s\":%u", subsys_name(s), (unsigned)e->subsys_counters[s]);
        first = 0;
    }
    printf("}}\n");
}

void _start(void) {
    int json = 0;
    int argc = 0;
    char **argv = (char **)0;  // argv passing not yet wired; flags via env TBD.
    (void)argc; (void)argv;

    // Minimal argv parser once spawn-with-argv is available. For now,
    // a single-byte flag file at /tmp/memstat.json could flip the mode;
    // defaulting to human-readable is the common path.

    static kheap_stats_entry_u_t buf[MEMSTAT_BUF_MAX];
    int n = syscall_kheap_stats(buf, MEMSTAT_BUF_MAX);
    if (n < 0) {
        printf("memstat: SYS_KHEAP_STATS failed\n");
        syscall_exit(1);
    }

    for (int i = 0; i < n; ++i) {
        if (json) print_entry_json(&buf[i]);
        else      print_entry_human(&buf[i]);
    }
    syscall_exit(0);
}
