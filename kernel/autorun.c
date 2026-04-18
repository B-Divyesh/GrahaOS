// kernel/autorun.c
// Phase 12: init-process selection and init-exit handling.

#include "autorun.h"
#include "cmdline.h"
#include "shutdown.h"
#include "watchdog.h"
#include "../arch/x86_64/drivers/serial/serial.h"
#include <stddef.h>
#include "log.h"

extern volatile uint64_t g_timer_ticks;

// Holds the final initrd path chosen by autorun_decide(). Sized to fit
// "bin/" + a 63-byte autorun name + NUL. Static lifetime so callers
// can stash the pointer.
#define AUTORUN_PATH_CAP 72
static char s_path_buf[AUTORUN_PATH_CAP];
static const char *s_path_default = "bin/gash";
static int  s_init_pid = -1;

static void copy_prefixed_name(const char *name) {
    // Build "bin/<name>" into s_path_buf. Truncates safely if the user
    // passes a pathologically long autorun= value.
    const char *prefix = "bin/";
    size_t i = 0;
    while (prefix[i] && i < AUTORUN_PATH_CAP - 1) {
        s_path_buf[i] = prefix[i];
        i++;
    }
    size_t j = 0;
    while (name[j] && i < AUTORUN_PATH_CAP - 1) {
        s_path_buf[i++] = name[j++];
    }
    s_path_buf[i] = '\0';
}

const char *autorun_decide(void) {
    if (g_cmdline_flags.autorun && *g_cmdline_flags.autorun) {
        copy_prefixed_name(g_cmdline_flags.autorun);
        return s_path_buf;
    }
    return s_path_default;
}

void autorun_register_init_pid(int pid) {
    s_init_pid = pid;
    klog(KLOG_INFO, SUBSYS_CORE, "autorun: init pid=%lu", (unsigned long)((uint64_t)pid));
    // Phase 12: arm the TEST_TIMEOUT watchdog iff autorun is active.
    // Interactive boots have timeout_seconds > 0 too (defaults to 60),
    // but the watchdog only fires when autorun was requested — we
    // don't want `make qemu-interactive` panicking on a user who goes
    // to the bathroom.
    if (autorun_is_active()) {
        watchdog_arm(g_timer_ticks, g_cmdline_flags.test_timeout_seconds);
    }
}

int autorun_get_init_pid(void) {
    return s_init_pid;
}

bool autorun_is_active(void) {
    return g_cmdline_flags.autorun != NULL;
}

void autorun_on_init_exit(int pid, int status) {
    // Cancel the watchdog — normal exit means we didn't time out.
    watchdog_disarm();
    klog(KLOG_INFO, SUBSYS_CORE, "autorun: init pid=%lu exited status=%lu", (unsigned long)((uint64_t)pid), (unsigned long)((uint64_t)(int64_t)status));
    if (autorun_is_active()) {
        klog(KLOG_INFO, SUBSYS_CORE, " [autorun active — shutting down]");
        // FIFO-flush nudge so the last lines survive the ACPI write.
        klog(KLOG_INFO, SUBSYS_CORE, "                ");
        kernel_shutdown();
        // unreachable
    }
    klog(KLOG_INFO, SUBSYS_CORE, " [interactive — staying up]");
}
