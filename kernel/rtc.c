// kernel/rtc.c
//
// See rtc.h for the top-of-file rationale.
#include "rtc.h"

#include <stdbool.h>
#include "log.h"
#include "../arch/x86_64/cpu/ports.h"

// LAPIC timer tick counter, 100 Hz (10 ms per tick). Set up by
// arch/x86_64/cpu/interrupts.c and driven by the LAPIC timer ISR.
extern volatile uint64_t g_timer_ticks;

int64_t g_boot_wall_seconds = 0;

// CMOS I/O ports.
#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

// CMOS register indices.
#define CMOS_REG_SECONDS    0x00
#define CMOS_REG_MINUTES    0x02
#define CMOS_REG_HOURS      0x04
#define CMOS_REG_DAY        0x07
#define CMOS_REG_MONTH      0x08
#define CMOS_REG_YEAR       0x09
#define CMOS_REG_STATUS_A   0x0A  // bit 7: update in progress
#define CMOS_REG_STATUS_B   0x0B  // bit 1: 24h mode; bit 2: binary (else BCD)

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static bool update_in_progress(void) {
    return (cmos_read(CMOS_REG_STATUS_A) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

// Days-to-start-of-month table for a non-leap year.
static const int days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static bool is_leap(int year) {
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

// Convert calendar date (full year, month 1..12, day 1..31, hour, minute,
// second) to Unix epoch seconds. Assumes UTC. Correct for years >= 1970.
static int64_t calendar_to_unix(int year, int mon, int day,
                                int hour, int min, int sec)
{
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += is_leap(y) ? 366 : 365;
    }
    days += days_before_month[mon - 1];
    if (mon > 2 && is_leap(year)) {
        days++;
    }
    days += (day - 1);
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

void rtc_init(void) {
    // Poll until the CMOS is not mid-update. Bounded: the update window is
    // < 2 ms in practice; we cap at ~1 M iterations as a safety net.
    for (int i = 0; i < 1000000 && update_in_progress(); i++) { }

    uint8_t sec  = cmos_read(CMOS_REG_SECONDS);
    uint8_t min  = cmos_read(CMOS_REG_MINUTES);
    uint8_t hour = cmos_read(CMOS_REG_HOURS);
    uint8_t day  = cmos_read(CMOS_REG_DAY);
    uint8_t mon  = cmos_read(CMOS_REG_MONTH);
    uint8_t year = cmos_read(CMOS_REG_YEAR);

    // Re-read and accept only if stable; otherwise redo once (a 1-register
    // change during our read is rare but possible).
    for (int attempt = 0; attempt < 2; attempt++) {
        while (update_in_progress()) { }
        uint8_t s2 = cmos_read(CMOS_REG_SECONDS);
        uint8_t m2 = cmos_read(CMOS_REG_MINUTES);
        uint8_t h2 = cmos_read(CMOS_REG_HOURS);
        uint8_t d2 = cmos_read(CMOS_REG_DAY);
        uint8_t M2 = cmos_read(CMOS_REG_MONTH);
        uint8_t y2 = cmos_read(CMOS_REG_YEAR);
        if (s2 == sec && m2 == min && h2 == hour &&
            d2 == day && M2 == mon && y2 == year) {
            break;
        }
        sec = s2; min = m2; hour = h2;
        day = d2; mon = M2; year = y2;
    }

    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);
    bool binary_mode = (status_b & 0x04) != 0;
    bool hour_24     = (status_b & 0x02) != 0;
    uint8_t hour_raw = hour;  // Preserve PM bit before BCD-decoding strips it.

    if (!binary_mode) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    } else {
        hour &= 0x7F;  // Strip the 12-hour PM flag before range checks.
    }

    if (!hour_24 && (hour_raw & 0x80)) {
        // 12-hour mode, PM flag set: e.g. 1 PM = 13:00, 12 PM = 12:00.
        hour = (hour % 12) + 12;
    } else if (!hour_24 && hour == 12) {
        // 12 AM -> 00:00.
        hour = 0;
    }

    // CMOS year is 00..99; we're well past Y2K so assume 20xx. (The CMOS
    // "century" register at 0x32 is not universally populated across BIOSes;
    // assuming 20xx is the pragmatic Phase 15b choice. Post-2099 this will
    // need to revisit, which is a Phase-way-past-28 concern.)
    int full_year = 2000 + (int)year;

    // Sanity: if the RTC returned garbage (e.g., field out of range), fall
    // back to g_boot_wall_seconds = 0. audit log entries will carry 0 until
    // a live RTC is installed.
    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60 || full_year < 1970) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "rtc: bogus CMOS readout y=%d mo=%u d=%u %02u:%02u:%02u -- falling back to 0",
             full_year, mon, day, hour, min, sec);
        g_boot_wall_seconds = 0;
        return;
    }

    g_boot_wall_seconds = calendar_to_unix(full_year, mon, day, hour, min, sec);
    klog(KLOG_INFO, SUBSYS_CORE,
         "rtc: boot wall = %lld seconds (%d-%02u-%02u %02u:%02u:%02u UTC)",
         g_boot_wall_seconds, full_year, mon, day, hour, min, sec);
}

int64_t rtc_now_seconds(void) {
    // LAPIC timer ticks at 100 Hz (10 ms per tick). Elapsed seconds =
    // g_timer_ticks / 100. Volatile read is a single aligned 64-bit load.
    return g_boot_wall_seconds + (int64_t)(g_timer_ticks / 100ULL);
}
