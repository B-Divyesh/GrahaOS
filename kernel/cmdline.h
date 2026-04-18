// kernel/cmdline.h
// Phase 12: kernel command-line flags parsed from the Limine
// executable_cmdline response. See specs/phase-12-test-harness.yml.

#ifndef GRAHAOS_CMDLINE_H
#define GRAHAOS_CMDLINE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct cmdline_flags {
    const char *autorun;
    bool        quiet;
    uint32_t    test_timeout_seconds;
    // Phase 13 fault-injection flags. `inject_klog_preinit=N` makes
    // main.c emit N klog calls BEFORE klog_init runs, exercising the
    // g_early_drops counter + retrospective summary entry.
    // `inject_ring_wrap=N` makes main.c emit N klog calls right after
    // klog_init, exercising the head-wrap path (N > 16384 forces wrap).
    uint32_t    inject_klog_preinit;
    uint32_t    inject_ring_wrap;
    // Phase 13: klog_mirror=0 disables the COM1 mirror so klog
    // entries land only in the ring. -1 means "use the build-time
    // KLOG_MIRROR_DEFAULT". Resolved by main.c after klog_init.
    int32_t     klog_mirror;
} cmdline_flags_t;

extern cmdline_flags_t g_cmdline_flags;

// Parse the kernel command line. `raw` may be NULL or empty, in which
// case defaults are retained. The string is copied into a private static
// buffer so the returned pointers in g_cmdline_flags.autorun remain
// valid for the kernel's lifetime regardless of where `raw` pointed.
void cmdline_parse(const char *raw);

#endif
