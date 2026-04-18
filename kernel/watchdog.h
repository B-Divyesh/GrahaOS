// kernel/watchdog.h
// Phase 12: TEST_TIMEOUT watchdog.
//
// When the kernel command line sets `autorun=<name>` with a non-zero
// `test_timeout_seconds`, the watchdog arms at init-process creation
// and panics the kernel if the init hasn't exited within the budget.
// Piggybacks on the 100 Hz LAPIC timer: watchdog_tick() is called
// once per hardware interrupt.

#ifndef GRAHAOS_WATCHDOG_H
#define GRAHAOS_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

// Arm the watchdog. `start_tick` is the value of g_timer_ticks at the
// moment we want the budget clock to start (typically right after
// PID 1 is created). `timeout_seconds` is the allowed runtime. A zero
// timeout disarms.
void watchdog_arm(uint64_t start_tick, uint32_t timeout_seconds);

// Called from the LAPIC 100 Hz ISR on every tick. If armed and the
// budget is exceeded, prints a panic frame and forces kernel_shutdown().
void watchdog_tick(uint64_t current_tick);

// Disarm (e.g., when init exits cleanly). No-op if never armed.
void watchdog_disarm(void);

bool watchdog_is_armed(void);

#endif
