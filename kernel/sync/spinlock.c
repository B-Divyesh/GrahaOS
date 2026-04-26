// kernel/sync/spinlock.c
//
// Phase 20 — structured-panic spinlock.
//
// Pre-Phase-20, a failed acquire (10 M iterations) would log "DEADLOCK: Lock
// X CPU:N" to the framebuffer and silently return without the lock held. The
// caller had no idea — it proceeded into the critical section believing the
// lock was owned, corrupting scheduler state. That silent path is DELETED.
//
// Post-Phase-20, every acquire carries a TSC-derived wall-clock budget
// (default 100 ms via SPINLOCK_DEFAULT_BUDGET_NS). On budget overrun we
// capture a spinlock_oops_t (holder CPU, acquirer CPU, elapsed ns, file/line
// where known) and call kpanic_at() — the system dies loudly with a
// parseable ==OOPS== frame that parse_tap.py recognises. Release-of-unheld
// and cross-CPU release are treated symmetrically: loud panic, no silent
// "log and continue".
#include "spinlock.h"
#include "../../drivers/video/framebuffer.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/tsc.h"
#include "../panic.h"
#include "../audit.h"
#include "../vsnprintf.h"
#include "../log.h"
#include <stdarg.h>

// Forward declaration
void framebuffer_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

// Fallback iteration cap used before TSC calibration completes (very early
// boot — before tsc_init runs in kmain). Generous to avoid false positives
// on slow early-boot emulators.
#define SPINLOCK_PRE_TSC_ITER_CAP 200000000U

void kernel_panic(const char *fmt, ...) {
    // Phase 13: route through kpanic so spinlock panics produce a
    // parseable ==OOPS== frame. fmt is treated as a literal reason
    // string — callers in this codebase don't pass varargs to it.
    // If %-formatting is needed in the future, ksnprintf into a
    // local buffer here before calling kpanic.
    if (!fmt) fmt = "kernel_panic (no message)";

    // Format with varargs into a stack buffer in case any caller
    // ever uses format specifiers. kpanic itself does not format.
    char reason[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);

    kpanic(reason);
}

void spinlock_init(spinlock_t *lock, const char *name) {
    if (!lock) {
        kernel_panic("spinlock_init: NULL lock pointer");
    }
    
    lock->owner = (uint64_t)-1;
    lock->count = 0;
    lock->locked = false;
    lock->name = name ? name : "unnamed";
    lock->last_file = NULL;
    lock->last_line = 0;
    lock->interrupt_state = 0;
}

// ---------------------------------------------------------------------------
// spinlock_acquire_panic: shared tail between the TSC-budget overrun path
// and the release-of-unheld / cross-CPU-release paths. Fills the oops
// payload, emits the audit event, formats the panic message, and calls
// kpanic_at. Never returns.
// ---------------------------------------------------------------------------
static __attribute__((noreturn)) void
spinlock_acquire_panic(spinlock_t *lock,
                       uint64_t acquirer_cpu,
                       uint64_t budget_ns,
                       uint64_t elapsed_ns,
                       const char *file, int line,
                       const char *why) {
    spinlock_oops_t oops = {
        .lock_name     = lock ? lock->name : "<null-lock>",
        .holder_cpu    = lock ? lock->owner : (uint64_t)-1,
        .acquirer_cpu  = acquirer_cpu,
        .budget_ns     = budget_ns,
        .elapsed_ns    = elapsed_ns,
        .acquirer_file = file,
        .acquirer_line = line,
        .holder_file   = (lock ? lock->last_file : NULL),
        .holder_line   = (lock ? lock->last_line : 0),
    };

    // Emit audit entry synchronously so the audit ring has the panic context
    // even after the reboot. audit_enqueue is IRQ-safe.
    audit_write_sched_spinlock_panic(oops.lock_name,
                                     (uint32_t)oops.holder_cpu,
                                     (uint32_t)oops.acquirer_cpu,
                                     oops.budget_ns);

    // Also drop a loud klog line — useful when the audit file hasn't been
    // attached yet (very early boot) and the audit ring doesn't survive
    // reboot.
    klog(KLOG_ERROR, SUBSYS_CORE,
         "SPINLOCK PANIC: lock=%s reason=%s holder_cpu=%u acquirer_cpu=%u "
         "budget_ns=%lu elapsed_ns=%lu acq_site=%s:%d holder_site=%s:%d",
         oops.lock_name, why,
         (unsigned)oops.holder_cpu, (unsigned)oops.acquirer_cpu,
         (unsigned long)oops.budget_ns, (unsigned long)oops.elapsed_ns,
         oops.acquirer_file ? oops.acquirer_file : "?",
         oops.acquirer_line,
         oops.holder_file ? oops.holder_file : "?",
         oops.holder_line);

    char reason[192];
    ksnprintf(reason, sizeof(reason),
              "spinlock %s: %s (holder_cpu=%u acquirer_cpu=%u "
              "budget_ns=%lu elapsed_ns=%lu)",
              oops.lock_name, why,
              (unsigned)oops.holder_cpu, (unsigned)oops.acquirer_cpu,
              (unsigned long)oops.budget_ns, (unsigned long)oops.elapsed_ns);

    kpanic(reason);
    // kpanic is noreturn; compiler needs the hint.
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// _spinlock_acquire_with_budget — the real acquire path.
//
// Three regimes:
//   1. TSC not ready (very early boot): use SPINLOCK_PRE_TSC_ITER_CAP as a
//      cheap iteration-count sanity cap. No real-time guarantee.
//   2. TSC ready: sample rdtsc() at entry; every 1024 iterations, convert
//      the elapsed TSC delta to ns and compare against budget_ns. If over,
//      panic via spinlock_acquire_panic.
//   3. Recursive: caller already owns the lock; bump count and return.
// ---------------------------------------------------------------------------
void _spinlock_acquire_with_budget(spinlock_t *lock, uint64_t budget_ns,
                                   const char *file, int line) {
    if (!lock) {
        kernel_panic("spinlock_acquire: NULL lock pointer");
    }

    uint64_t cpu_id = get_cpu_id();

    // Recursive acquisition: same CPU already holds it.
    if (lock->locked && lock->owner == cpu_id) {
        lock->count++;
        return;
    }

    // Save interrupt state and disable interrupts.
    uint64_t flags;
    asm volatile(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
    );

    // Fast path: single test_and_set.
    if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        lock->interrupt_state = flags;
        lock->owner = cpu_id;
        lock->count = 1;
        lock->last_file = file;
        lock->last_line = line;
        return;
    }

    // Contended path: spin with a budget.
    bool tsc_mode = tsc_is_ready();
    uint64_t start_tsc = tsc_mode ? rdtsc() : 0;
    uint32_t iter_cap = SPINLOCK_PRE_TSC_ITER_CAP;

    uint32_t iter = 0;
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        iter++;

        // Budget check every 1024 iterations — cheap amortized cost, TSC read
        // is ~15-20 cycles on modern x86 but we don't want it in the inner
        // loop.
        if ((iter & 1023) == 0) {
            uint64_t elapsed_ns;
            if (tsc_mode) {
                elapsed_ns = tsc_to_ns(rdtsc() - start_tsc);
                if (elapsed_ns > budget_ns) {
                    spinlock_acquire_panic(lock, cpu_id, budget_ns, elapsed_ns,
                                           file, line, "acquire budget exceeded");
                }
            } else {
                // Pre-TSC early boot: bail out after a large but bounded
                // iteration count. We can't compute real ns; pass 0.
                if (iter > iter_cap) {
                    spinlock_acquire_panic(lock, cpu_id, budget_ns, 0,
                                           file, line, "acquire iter cap exceeded (pre-TSC)");
                }
            }
        }

        asm volatile("pause");
    }

    // Got it.
    lock->interrupt_state = flags;
    lock->owner = cpu_id;
    lock->count = 1;
    lock->last_file = file;
    lock->last_line = line;
}

// Legacy entry point — wraps _spinlock_acquire_with_budget with the default
// 100 ms budget. Every existing `spinlock_acquire(lock)` callsite funnels
// through here.
void _spinlock_acquire(spinlock_t *lock, const char *file, int line) {
    _spinlock_acquire_with_budget(lock, SPINLOCK_DEFAULT_BUDGET_NS, file, line);
}

void _spinlock_release(spinlock_t *lock, const char *file, int line) {
    if (!lock) {
        kernel_panic("spinlock_release: NULL lock pointer");
    }

    uint64_t cpu_id = get_cpu_id();

    // Phase 20: illegal releases are LOUD. A pre-Phase-20 path silently
    // logged and returned; that hid double-release and cross-CPU-release
    // bugs. Both are now structured panics.
    if (!lock->locked) {
        spinlock_acquire_panic(lock, cpu_id, 0, 0,
                               file, line, "release of unheld lock");
    }

    if (lock->owner != cpu_id) {
        spinlock_acquire_panic(lock, cpu_id, 0, 0,
                               file, line, "release from non-owner CPU");
    }

    // Handle recursive release.
    if (--lock->count > 0) {
        return;
    }
    
    // Get saved interrupt state
    uint64_t flags = lock->interrupt_state;
    
    // Release the lock
    lock->owner = (uint64_t)-1;
    lock->interrupt_state = 0;
    
    // CRITICAL: Memory barrier before releasing
    asm volatile("mfence" ::: "memory");
    
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    
    // Restore interrupt state
    if (flags & 0x200) {
        asm volatile("sti");
    }
}

bool spinlock_held(spinlock_t *lock) {
    if (!lock) return false;
    return lock->locked && lock->owner == get_cpu_id();
}

// Implementation of get_cpu_id
uint64_t get_cpu_id(void) {
    return (uint64_t)smp_get_current_cpu();
}

// Debug function to check all locks
void spinlock_check_all(void) {
    extern spinlock_t sched_lock;
    extern spinlock_t fb_lock;
    extern spinlock_t pmm_lock;
    extern spinlock_t vfs_lock;
    
    uint64_t cpu_id = get_cpu_id();
    
    char msg[64] = "Lock check CPU ";
    msg[15] = '0' + cpu_id;
    msg[16] = ':';
    msg[17] = '\0';
    framebuffer_draw_string(msg, 600, 10, COLOR_YELLOW, COLOR_BLACK);
    
    if (sched_lock.locked) {
        msg[0] = 's'; msg[1] = 'c'; msg[2] = 'h'; msg[3] = ':';
        msg[4] = '0' + sched_lock.owner;
        msg[5] = '\0';
        framebuffer_draw_string(msg, 600, 30, COLOR_CYAN, COLOR_BLACK);
    }
}