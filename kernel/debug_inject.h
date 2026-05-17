// kernel/debug_inject.h — Phase 28 Session G.1 fault injection counters.
//
// Four hooks let userspace tests + the soak harness drive synthetic
// failures at the lowest layers of the kernel:
//   - kmalloc / pmm_alloc_pages    countdown ("fail on the Nth call")
//   - chan_send / spinlock_acquire sample-gated ("approx 1/256 calls")
//
// Counters are zero-initialised so the gate sees no behavioural change
// until DEBUG_INJECT_* subops set them.  See arch/x86_64/cpu/syscall/syscall.h.

#pragma once
#include <stdint.h>

// Countdown counters: when > 0, decremented on each call; the call that
// drops the counter to 0 returns failure (NULL / -ENOMEM).
extern int64_t  g_debug_kmalloc_fail_nth;
extern int64_t  g_debug_pmm_fail_nth;

// Sample-gated rate flags: when > 0, the hook fires whenever the sampled
// entropy bits are zero (~1/256 calls).  Sample-based to keep the
// dispatcher's per-call overhead below 1% (Phase 25 T2 perf-regression
// lesson — never do per-call work in hot paths).
extern uint32_t g_debug_chan_send_fail_rate;
extern uint32_t g_debug_spinlock_timeout_rate;

// Observability counter for the spinlock hook: incremented when the
// rate sample fires.  Spinlock injection is OBSERVE-ONLY (does not
// alter control flow) per the safety analysis — a panic-on-sample
// design would brick the kernel on its own syscall return paths.
extern uint64_t g_debug_spinlock_injection_hits;

// Zero all counters.  Used by DEBUG_INJECT_RESET_ALL and between soak
// iterations.  Returns the prior spinlock_injection_hits value so the
// soak driver can sample it.
uint64_t debug_inject_reset_all(void);
