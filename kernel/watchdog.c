// kernel/watchdog.c
// Phase 12: kernel-side TEST_TIMEOUT watchdog. Single-CPU state; the
// LAPIC timer interrupt currently executes on BSP and guards global
// g_timer_ticks, so no additional locking is needed.

#include "watchdog.h"
#include "shutdown.h"
#include "panic.h"
#include "log.h"
#include "vsnprintf.h"
#include "../arch/x86_64/drivers/serial/serial.h"

#define LAPIC_HZ 100u

static bool     s_armed          = false;
static uint64_t s_start_tick     = 0;
static uint64_t s_deadline_ticks = 0;

void watchdog_arm(uint64_t start_tick, uint32_t timeout_seconds) {
    if (timeout_seconds == 0) {
        s_armed = false;
        return;
    }
    s_start_tick     = start_tick;
    s_deadline_ticks = (uint64_t)timeout_seconds * LAPIC_HZ;
    s_armed          = true;

    klog(KLOG_INFO, SUBSYS_CORE, "watchdog: armed deadline=");
    serial_write_dec((uint64_t)timeout_seconds);
    klog(KLOG_INFO, SUBSYS_CORE, "s (");
    serial_write_dec(s_deadline_ticks);
    klog(KLOG_INFO, SUBSYS_CORE, " ticks)");
}

void watchdog_disarm(void) {
    s_armed = false;
}

bool watchdog_is_armed(void) {
    return s_armed;
}

__attribute__((noreturn))
static void watchdog_panic(uint64_t elapsed_ticks) {
    // Phase 13: route through klog + kpanic so the watchdog panic
    // produces a proper ==OOPS== frame instead of a bare diagnostic
    // line. The klog FATAL entry persists in the ring tail dumped by
    // the oops, which is what parse_oops.py keys on.
    uint64_t secs = elapsed_ticks / LAPIC_HZ;
    klog(KLOG_FATAL, SUBSYS_CORE,
         "TEST_TIMEOUT after %lus (init did not exit within budget)",
         secs);

    char reason[64];
    ksnprintf(reason, sizeof(reason), "TEST_TIMEOUT %lus", secs);
    kpanic(reason);
}

void watchdog_tick(uint64_t current_tick) {
    if (!s_armed) return;
    if (current_tick < s_start_tick) return; // tick rollover sanity
    uint64_t elapsed = current_tick - s_start_tick;
    if (elapsed > s_deadline_ticks) {
        s_armed = false;  // avoid re-entry if shutdown spins
        watchdog_panic(elapsed);
    }
}
