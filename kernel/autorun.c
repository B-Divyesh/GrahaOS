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
// Phase 22 closeout (G2 / P21.1.8): default autorun is now `bin/init`, the
// userspace supervisor that reads /etc/init.conf and spawns the e1000d +
// netd daemons before launching gash as the autorun child.  `make test`
// still passes `autorun=ktest` on the cmdline, so the test harness path
// is unchanged.  Booting with no autorun= now gives a fully-up TCP/IP
// stack from the gash prompt onward.
static const char *s_path_default = "bin/init";
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

    // Phase 26 FU25.F: install a CAP_KIND_SYSTEM sub-token in init's handle
    // table. Init can derive narrowed sub-caps to trusted daemons (wasmd,
    // future system-monitor) without sharing RIGHTS_ALL. Best-effort: if the
    // bootcap doesn't exist (cap_system_init failed at boot) or insertion
    // fails (handle table OOM), log and continue — init runs without the
    // privileged cap; only TXN_FLAG_GLOBAL_SCOPE and similar paths are
    // affected, and they'd return -EPERM until the cap is granted.
    {
        extern int cap_system_install_to_pid(int pid, unsigned long long rights_subset);
        // RIGHT_INSPECT(0x20) | RIGHT_REVOKE(0x10) | RIGHT_DERIVE(0x8) |
        // RIGHT_INVOKE(0x10000) — keeps init in lockstep with the bootcap's
        // own rights mask so cap_object_derive doesn't reject.
        int rc = cap_system_install_to_pid(pid, 0x10038ULL);
        if (rc != 0) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "autorun: cap_system_install_to_pid pid=%d failed rc=%d",
                 pid, rc);
        }
    }

    // Phase 12: arm the TEST_TIMEOUT watchdog iff autorun is active.
    // Interactive boots have timeout_seconds > 0 too (defaults to 60),
    // but the watchdog only fires when autorun was requested — we
    // don't want `make qemu-interactive` panicking on a user who goes
    // to the bathroom.
    if (autorun_is_active()) {
        watchdog_arm(g_timer_ticks, g_cmdline_flags.test_timeout_seconds);
    }

    // FU29.H TUI: in INTERACTIVE mode (not the headless ktest gate), the kernel
    // console owns the display from here on. Wipe the boot-progress / debug
    // cruft now, and route subsequent direct kernel framebuffer_draw_* debug
    // draws (ELF loader, SYS_WAIT/spawn diagnostics, daemon respawns) to no-ops
    // so they don't trample the shell's TUI. The console's own rendering uses
    // framebuffer_force_draw_* (bypass-immune); the panic path still overrides
    // via g_panic_in_progress. Gated to non-ktest so `make test` is identical.
    {
        const char *ar = g_cmdline_flags.autorun;
        bool is_ktest = ar && ar[0] == 'k' && ar[1] == 't' && ar[2] == 'e' &&
                        ar[3] == 's' && ar[4] == 't' && ar[5] == '\0';
        if (!is_ktest) {
            extern void framebuffer_clear(uint32_t color);
            extern void framebuffer_set_console_owns_display(bool owns);
            extern void console_text_set_top(uint32_t y_px);
            framebuffer_clear(0x00101828u);   // dark-navy console backdrop
            framebuffer_set_console_owns_display(true);
            // Reserve the top 24 text rows (= gsh's 80x24 cell-grid chrome at
            // 16 px/row) so the kernel text console scrolls BELOW the persistent
            // cap-sidebar instead of clearing the whole screen over it.
            console_text_set_top(24u * 16u);
        }
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
