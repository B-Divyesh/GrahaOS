// kernel/sync/spinlock.h
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declaration to avoid circular dependency
uint32_t lapic_get_id(void);

// Recursive spinlock structure
typedef struct spinlock {
    volatile uint64_t owner;      // CPU core ID that owns the lock
    volatile uint32_t count;      // Recursion count
    volatile bool locked;         // Lock state
    const char *name;            // Lock name for debugging
    const char *last_file;       // File where lock was last acquired
    int last_line;               // Line where lock was last acquired
    uint64_t interrupt_state;    // Saved interrupt state (RFLAGS)
} spinlock_t;

// Static initializer for spinlocks
#define SPINLOCK_INITIALIZER(lockname) { \
    .owner = (uint64_t)-1, \
    .count = 0, \
    .locked = false, \
    .name = lockname, \
    .last_file = NULL, \
    .last_line = 0, \
    .interrupt_state = 0 \
}

// Debug macros
#ifdef DEBUG_LOCKS
#define spinlock_acquire(lock) _spinlock_acquire(lock, __FILE__, __LINE__)
#define spinlock_release(lock) _spinlock_release(lock, __FILE__, __LINE__)
#else
#define spinlock_acquire(lock) _spinlock_acquire(lock, NULL, 0)
#define spinlock_release(lock) _spinlock_release(lock, NULL, 0)
#endif

// Function prototypes
void spinlock_init(spinlock_t *lock, const char *name);
void _spinlock_acquire(spinlock_t *lock, const char *file, int line);
void _spinlock_release(spinlock_t *lock, const char *file, int line);
bool spinlock_held(spinlock_t *lock);

// ---------------------------------------------------------------------------
// Phase 20: structured-panic spinlock.
//
// spinlock_acquire_with_budget enforces a TSC-derived wall-clock budget on
// the test_and_set loop. On budget overrun, fills a spinlock_oops_t and
// panics via Phase 13's kpanic_at(). The legacy spinlock_acquire is now a
// thin wrapper with a default 100 ms budget. There is NO silent-timeout
// path — if a lock can't be acquired within its budget, the kernel dies
// LOUDLY and the oops frame names the holder CPU, the acquirer CPU, and
// the budget that was exceeded.
//
// Before tsc_is_ready() returns true (i.e., during very early boot) the
// budget enforcement falls back to a generous iteration cap. This avoids
// needing special-case plumbing in spinlock_init.
// ---------------------------------------------------------------------------

// Default budget for legacy spinlock_acquire callers.
//
// Phase 23 Stage-2 cutover: bumped 100 ms → 5 s.  Channel-mediated FS I/O
// holds locks (notably grahafs_lock and the per-task cap_handle locks)
// while it issues a chan_send → ahcid round-trip → chan_recv pair.  Each
// round-trip can take 10-50 ms in QEMU TCG; under contention several
// stacked requests easily exceed the previous 100 ms budget and trip the
// "deadlock" panic on healthy code.  5 s still catches real deadlocks
// loudly while letting the kernel-direct → channel-mode transition land.
// Phase 24 task: refactor grahafs to release its lock around block I/O,
// then we can drop this back to a tighter envelope.
#define SPINLOCK_DEFAULT_BUDGET_NS   5000000000ULL

void _spinlock_acquire_with_budget(spinlock_t *lock, uint64_t budget_ns,
                                   const char *file, int line);

// Convenience macro — picks up file/line automatically.
#ifdef DEBUG_LOCKS
#define spinlock_acquire_with_budget(lock, budget_ns) \
    _spinlock_acquire_with_budget((lock), (budget_ns), __FILE__, __LINE__)
#else
#define spinlock_acquire_with_budget(lock, budget_ns) \
    _spinlock_acquire_with_budget((lock), (budget_ns), NULL, 0)
#endif

// ---------------------------------------------------------------------------
// Phase 29 Session I (FU28.D): non-panicking timeout variant.
//
// _spinlock_acquire_with_timeout returns true on acquire, false on timeout.
// The caller decides the failure policy.  Existing callers that wrap the
// original spinlock_acquire() still hit the panic-on-budget path; new
// callers that want graceful degradation (e.g. diagnostic logging, retry,
// fall-back) use this variant.
//
// Recursive (same-CPU re-entry) returns true immediately and bumps count.
// On timeout the lock is NOT held and the saved-interrupt-state is NOT
// touched (caller's interrupt mask is preserved).
// ---------------------------------------------------------------------------
bool _spinlock_acquire_with_timeout(spinlock_t *lock, uint64_t budget_ns,
                                    const char *file, int line);

#ifdef DEBUG_LOCKS
#define spinlock_acquire_with_timeout(lock, budget_ns) \
    _spinlock_acquire_with_timeout((lock), (budget_ns), __FILE__, __LINE__)
#else
#define spinlock_acquire_with_timeout(lock, budget_ns) \
    _spinlock_acquire_with_timeout((lock), (budget_ns), NULL, 0)
#endif

// Diagnostic counter: incremented every time a timeout-variant acquire
// returns false because the budget was exceeded.  Tests use this to verify
// the variant degraded gracefully rather than panicking.
extern uint64_t g_spinlock_timeout_count;

// spinlock_oops_t — structured payload passed to kpanic_at when a spinlock
// budget is exceeded (or an illegal release is attempted). Flattened into
// the ==OOPS== frame's detail string by the kpanic handler.
typedef struct spinlock_oops {
    const char *lock_name;      // from spinlock_t.name
    uint64_t    holder_cpu;     // CPU id that currently holds the lock (0xFF if free)
    uint64_t    acquirer_cpu;   // CPU id that tried to acquire
    uint64_t    budget_ns;      // the TSC-measured budget that was exceeded
    uint64_t    elapsed_ns;     // actual wait time at panic
    const char *acquirer_file;  // __FILE__ at acquirer site
    int         acquirer_line;  // __LINE__ at acquirer site
    const char *holder_file;    // __FILE__ of current holder's last acquire (if DEBUG_LOCKS)
    int         holder_line;    // __LINE__ of current holder's last acquire
} spinlock_oops_t;

// Get current CPU ID - implementation moved to spinlock.c to avoid circular dependency
uint64_t get_cpu_id(void);

// Panic function declaration
void kernel_panic(const char *fmt, ...);

// Debug functions to check lock states
bool debug_is_sched_lock_held(void);
bool debug_is_fb_lock_held(void);
bool debug_is_pmm_lock_held(void);
bool debug_is_vfs_lock_held(void);
void debug_print_lock_states(void);