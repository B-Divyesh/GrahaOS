// user/schedbench_worker.c
//
// Phase 20 U16 — tiny compute worker spawned by /bin/schedbench.
//
// Purpose: occupy a scheduler runqueue slot for the duration of the bench.
// A tight integer loop keeps the task perpetually READY (the scheduler
// preempts it at every 10 ms tick, and it re-enqueues immediately).
// schedbench sends SIGKILL after the measurement window closes.
//
// No syscalls inside the hot loop — we are being measured, we don't want to
// perturb the sample with audit/pledge overhead.

#include "syscalls.h"
#include <stdint.h>

void _start(void) {
    volatile uint64_t acc = 1;
    while (1) {
        for (int i = 1; i <= 1 << 18; i++) {
            acc = acc * 1103515245ULL + 12345ULL + (uint64_t)i;
        }
        // Small yield-ish hatch: 1-in-256 iterations do a getpid() so the
        // loop isn't 100% CPU-starved against signals. getpid is effectively
        // zero-cost (no pledge/audit gate).
        if ((acc & 0xFF) == 0) {
            (void)syscall_getpid();
        }
    }
}
