// arch/x86_64/cpu/tsc.h
//
// Phase 20 — Time Stamp Counter helpers.
//
// The TSC is a 64-bit monotonic counter incremented once per CPU clock tick
// (on modern x86 it is invariant under power-management events, meaning it
// advances at a constant rate regardless of CPU frequency changes). It is the
// highest-resolution wall-clock source available in ring 0 without touching
// external hardware, and Phase 20 uses it in three places:
//
//   1. spinlock_acquire_with_budget — real wall-clock budget in ns, not a
//      spin-count heuristic. Budget of 100 ms means "if the test_and_set loop
//      is still trying after 100 ms of wall time, panic".
//
//   2. CPU-time accounting in schedule() — the task about to be preempted has
//      its cpu_budget_remaining_ns decremented by (rdtsc() now -
//      last_ran_tsc), converted to ns.
//
//   3. p99 wakeup latency in schedbench — userspace records rdtsc() before
//      yielding and after the wake path returns; the delta is rendered in ns.
//
// Calibration is performed once at BSP boot in tsc_init() via a 10 ms PIT
// channel 0 one-shot gate — same technique used by lapic_timer_calibrate.
// The PIT is available on every x86 system including QEMU and remains the
// reference oscillator for this kind of bootstrap measurement.
//
// For now we assume constant_tsc + synchronized across cores. QEMU provides
// both. Real-hardware per-CPU TSC offset tracking only matters on multi-
// socket physical hardware (single-socket modern x86 has invariant_tsc
// that is synchronized across cores at reset). GrahaOS targets QEMU as
// the primary platform; real hardware support is a future hardware-
// porting effort, not a software-side gap.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// g_tsc_hz — ticks per second, measured at boot. 0 until tsc_init() returns.
// g_ns_per_tsc_fp32 — 32.32 fixed-point: nanoseconds per TSC tick.
//   ns = (tsc_delta * g_ns_per_tsc_fp32) >> 32
// Example: at 3 GHz, g_ns_per_tsc_fp32 = (1e9 << 32) / 3e9 = 0x5555_5555.
// Keeping the conversion as a 64-bit multiply + right-shift avoids the cost of
// division on the hot path (every schedule tick, every spinlock budget check).
// ---------------------------------------------------------------------------
extern uint64_t g_tsc_hz;
extern uint64_t g_ns_per_tsc_fp32;

// ---------------------------------------------------------------------------
// rdtsc — read the Time Stamp Counter. No memory barrier: we're happy with
// the out-of-order speculation window around it. If we ever need strict
// ordering (benchmark measurements), the caller can wrap with mfence; lfence
// pairs or use rdtscp.
// ---------------------------------------------------------------------------
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------------
// tsc_init — calibrate g_tsc_hz by gating 10 ms of PIT ticks (1193182 Hz
// oscillator; 11932 ticks ≈ 10 ms). Interrupts are disabled across the
// measurement window. Sets g_ns_per_tsc_fp32 as a side-effect. Must be called
// exactly once from BSP in kmain, after serial_init but before any code that
// depends on tsc_to_ns. Idempotent: second call is a no-op.
// ---------------------------------------------------------------------------
void tsc_init(void);

// ---------------------------------------------------------------------------
// tsc_to_ns — convert a raw TSC delta (NOT an absolute TSC reading — a delta
// between two rdtsc() snapshots) to nanoseconds. Hot path: one 64-bit
// multiply + one 32-bit right shift. Undefined behaviour if called before
// tsc_init.
// ---------------------------------------------------------------------------
static inline uint64_t tsc_to_ns(uint64_t tsc_delta) {
    // __uint128_t keeps the full precision of the 32.32 multiply; the >> 32
    // drops the fractional half. Without __uint128_t the 64-bit multiply
    // would silently overflow at ~4 seconds worth of ticks.
    __uint128_t prod = (__uint128_t)tsc_delta * (__uint128_t)g_ns_per_tsc_fp32;
    return (uint64_t)(prod >> 32);
}

// ---------------------------------------------------------------------------
// ns_to_tsc — inverse of tsc_to_ns. Useful for TSC budgets ("how many TSC
// ticks is 100 ms?") so the hot spinlock loop can skip the ns conversion.
// Guards against g_tsc_hz == 0.
// ---------------------------------------------------------------------------
static inline uint64_t ns_to_tsc(uint64_t ns) {
    if (g_tsc_hz == 0) return 0;
    // (ns * g_tsc_hz) / 1_000_000_000.
    // Kernel has no libgcc __udivti3, so we stay in uint64_t. For the usage
    // in Phase 20 (ns = spinlock budget ≤ 1e9, g_tsc_hz ≤ 1e10) the product
    // stays under 1e19 which fits. For larger ns, callers should prefer
    // tsc_to_ns(raw_delta) and compare in ns.
    return (ns * g_tsc_hz) / 1000000000ULL;
}

// ---------------------------------------------------------------------------
// tsc_is_ready — false until tsc_init has completed its first measurement.
// Callers on the spinlock hot path consult this to decide whether TSC-based
// budgets can be trusted; pre-init, they fall back to the legacy spin-count
// budget (large iteration cap).
// ---------------------------------------------------------------------------
bool tsc_is_ready(void);
