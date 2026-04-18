// user/klog.c
// Phase 13: standalone /bin/klog reader. Spawned by gash via the
// `dmesg` builtin... actually not — gash's `dmesg` calls
// SYS_KLOG_READ inline because spawn() doesn't carry argv on
// GrahaOS yet. /bin/klog therefore exists for the spec's
// "/bin/klog should be present" surface and for any future
// argv-aware spawn path. With no args it dumps the full ring tail.

#include "syscalls.h"
#include "../kernel/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *lvl_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};
static const char *kernel_subs[] = {
    "CORE", "MM", "SCHED", "SYSCALL", "VFS",
    "FS", "NET", "CAP", "DRV", "TEST",
};

static void print_entry(const klog_entry_t *e) {
    uint8_t lv = (uint8_t)(e->level & 0x0F);
    const char *lv_name = (lv < 6) ? lvl_names[lv] : "?????";
    const char *sub_name = (e->subsystem_id < 10)
        ? kernel_subs[e->subsystem_id] : "USER";

    uint64_t secs = e->ns_timestamp / 1000000000ULL;
    uint64_t nsec = e->ns_timestamp % 1000000000ULL;

    // [   T.NNNNNNNNN] LEVEL SUBSYS msg
    printf("[%4lu.%09lu] %-5s %-7s %s\n",
           (unsigned long)secs, (unsigned long)nsec,
           lv_name, sub_name, e->message);
}

void _start(void) {
    static klog_entry_t buf[64];
    int n = syscall_klog_read(0, 0, buf, sizeof(buf));
    if (n < 0) {
        printf("klog: SYS_KLOG_READ failed (%d)\n", n);
        exit(1);
    }
    for (int i = 0; i < n; i++) print_entry(&buf[i]);
    exit(0);
}
