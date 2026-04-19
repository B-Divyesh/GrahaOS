// kernel/rtc.h
//
// Phase 15b (out-of-spec #2) — CMOS Real-Time Clock reader.
//
// GrahaOS pre-15b had no wall-clock source; klog timestamps were nanoseconds
// since boot. Phase 15b's audit log records `wall_clock_seconds` for every
// entry and rotates `/var/audit/` files on a UTC-day boundary, both of which
// require a wall-time reference. This module reads the x86 CMOS Real-Time
// Clock once at boot and exposes a monotonic `rtc_now_seconds()` = boot wall
// time plus elapsed nanoseconds (derived from the LAPIC timer tick counter).
//
// Precision is coarse (the LAPIC fires at 100 Hz, so `rtc_now_seconds()`
// jitters by up to 10 ms). That is adequate for day-level file rotation and
// second-granularity audit entries.
#pragma once

#include <stdint.h>

// Unix epoch seconds captured at boot via a single CMOS read. Set by
// rtc_init(). Zero until rtc_init() has run; stable thereafter.
extern int64_t g_boot_wall_seconds;

// Read the CMOS RTC once, decode date/time, compute Unix epoch, and publish
// via g_boot_wall_seconds. Safe to call from BSP boot context with interrupts
// still disabled. Must run before the first call to rtc_now_seconds() — the
// recommended slot in main.c is right after percpu_init(0).
void rtc_init(void);

// Current wall clock seconds, monotonic and approximate. Returns
// g_boot_wall_seconds + (LAPIC-tick-derived seconds since boot). Safe at any
// scheduling context; reads a volatile tick counter.
int64_t rtc_now_seconds(void);
