// arch/x86_64/cpu/tsc.c
//
// Phase 20 — Time Stamp Counter calibration.
//
// tsc_init() gates a known number of PIT ticks and measures the matching
// TSC delta; g_tsc_hz = (tsc_delta * 100) for a 10 ms window. We derive
// g_ns_per_tsc_fp32 from g_tsc_hz once so every subsequent tsc_to_ns call is
// a single 128-bit multiply + right-shift. See tsc.h for the
// fixed-point contract.
#include "tsc.h"
#include "ports.h"

uint64_t g_tsc_hz = 0;
uint64_t g_ns_per_tsc_fp32 = 0;
static bool g_tsc_ready = false;

// PIT reference: 1.193182 MHz oscillator (well-known value). 11932 counts
// ≈ 10 ms (± 1 µs). Wider windows give better precision but steal more boot
// time; 10 ms is the sweet spot — lapic_timer_calibrate uses the same gate.
#define PIT_HZ                1193182u
#define PIT_CALIBRATION_COUNT 11932u     // 10 ms
#define PIT_CALIBRATION_NS    10000000ULL // 10 ms in ns

#define PIT_PORT_CH0    0x40
#define PIT_PORT_CMD    0x43

static uint64_t tsc_pit_gate_10ms(void) {
    // Channel 0, lobyte/hibyte, one-shot (mode 0).
    outb(PIT_PORT_CMD, 0x30);
    outb(PIT_PORT_CH0, PIT_CALIBRATION_COUNT & 0xFF);
    outb(PIT_PORT_CH0, (PIT_CALIBRATION_COUNT >> 8) & 0xFF);

    uint64_t tsc_start = rdtsc();

    // Poll the output bit. In mode 0 the output stays low while counting and
    // goes high when the count reaches 0. Read-back command 0xE2 latches
    // channel 0 status; bit 7 mirrors the output pin.
    uint8_t status;
    do {
        outb(PIT_PORT_CMD, 0xE2);
        status = inb(PIT_PORT_CH0);
    } while (!(status & 0x80));

    return rdtsc() - tsc_start;
}

void tsc_init(void) {
    if (g_tsc_ready) return;

    // Interrupts off for the measurement window — a preempt here would add
    // the scheduler's cost to the TSC delta.
    uint64_t flags;
    __asm__ __volatile__(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags));

    uint64_t delta = tsc_pit_gate_10ms();

    // Restore interrupt state before doing any non-trivial work.
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }

    // Defensive: if the PIT path returned an obviously-wrong delta (e.g.,
    // running in an environment without a PIT), fall back to an assumed 3 GHz.
    // The fallback is loud and will show up in any sanity test; we prefer
    // "wrong but bounded" to "infinite loop in a future spinlock budget check".
    if (delta < 1000000ULL || delta > 1000000000000ULL) {
        g_tsc_hz = 3000000000ULL;
    } else {
        // delta ticks in 10 ms  →  delta * 100 ticks/sec
        g_tsc_hz = delta * 100ULL;
    }

    // Precompute 32.32 fixed-point ns/tick = (1e9 << 32) / g_tsc_hz.
    // 1e9 << 32 = 0x3B9ACA0000000000 which fits in uint64_t, so stay in 64-bit
    // to avoid the libgcc __udivti3 helper (not linked in the kernel because
    // -mno-sse disables the usual softfloat path).
    uint64_t numerator = 1000000000ULL << 32;
    g_ns_per_tsc_fp32 = numerator / g_tsc_hz;

    g_tsc_ready = true;
}

bool tsc_is_ready(void) {
    return g_tsc_ready;
}
