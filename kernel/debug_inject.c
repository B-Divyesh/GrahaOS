// kernel/debug_inject.c — Phase 28 Session G.1 fault injection storage.
#include "debug_inject.h"

int64_t  g_debug_kmalloc_fail_nth          = 0;
int64_t  g_debug_pmm_fail_nth              = 0;
uint32_t g_debug_chan_send_fail_rate       = 0;
uint32_t g_debug_spinlock_timeout_rate     = 0;
uint64_t g_debug_spinlock_injection_hits   = 0;

uint64_t debug_inject_reset_all(void) {
    uint64_t prior = g_debug_spinlock_injection_hits;
    g_debug_kmalloc_fail_nth        = 0;
    g_debug_pmm_fail_nth            = 0;
    g_debug_chan_send_fail_rate     = 0;
    g_debug_spinlock_timeout_rate   = 0;
    g_debug_spinlock_injection_hits = 0;
    return prior;
}
