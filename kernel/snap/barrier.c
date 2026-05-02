// kernel/snap/barrier.c — Phase 24 W14.1 / W14.2 helpers.
//
// snap_begin_barrier  + snap_end_barrier  bracket the capture window.
// While the barrier is active, every CPU except the caller (owner) parks
// its current task into TASK_STATE_BARRIER_WAIT and dispatches its idle
// task. The owner CPU stays running so the snap_create caller can do the
// page-table walk + VMO/channel/FS-pin captures.
//
// Sequencing.
//   1. snap_begin_barrier atomically flips barrier_flag from 0 → 1, captures
//      owner_task, sends IPI_VEC_WAKEUP to every other CPU, then polls
//      runq.current==idle_task on each non-owner CPU. 100 ms TSC watchdog.
//   2. While the flag is set, schedule() (in sched.c) parks non-idle, non-
//      owner tasks into g_snap_barrier.parked_head and dispatches idle.
//      The owner CPU's schedule() short-circuits (cur == owner_task).
//   3. snap_end_barrier clears the flag, walks parked_head and re-enqueues
//      every task as READY. Sends a wake-up IPI so any idle CPU dispatches
//      promptly rather than waiting a tick.
//
// Why the watchdog. Without it, a CPU stuck in a long-held kernel-context
// spinlock (audit-flusher, grahafs lock-drop window, etc.) would hold the
// entire system. The watchdog turns "stuck barrier" into a clean -ETIME
// return that snap_create can propagate to the caller.

#include "snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../log.h"
#include "../sync/spinlock.h"

#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/tsc.h"
#include "../../arch/x86_64/drivers/lapic/lapic.h"

// Local errno mirrors (the kernel uses raw -errno integers everywhere).
#define BARRIER_EBUSY  16
#define BARRIER_ETIME 110

// W14.2 helper used by sched.c's schedule() to ack a CPU into the parked
// list. Defined here so sched.c does not need to reach into snapshot.h
// internals — the call site is a single function call gated by a single
// atomic load of barrier_flag.
//
// Caller invariants:
//   - cur is the running task on this CPU at schedule() entry.
//   - cur is not idle, not the barrier owner, and currently RUNNING.
//   - Caller has already saved cur->regs from the interrupt frame.
// On return cur->state has been flipped to TASK_STATE_BARRIER_WAIT and
// cur is linked into g_snap_barrier.parked_head.
void snap_barrier_park_locked(struct task_struct *cur) {
    if (!cur) return;
    spinlock_acquire(&g_snap_barrier.lock);
    cur->state         = TASK_STATE_BARRIER_WAIT;
    cur->barrier_next  = g_snap_barrier.parked_head;
    g_snap_barrier.parked_head = cur;
    g_snap_barrier.acks++;
    spinlock_release(&g_snap_barrier.lock);
}

// W14.1 — request a barrier window. Returns 0 on success, -EBUSY if
// another barrier is already active, -ETIME on watchdog stall.
int snap_begin_barrier(void) {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_snap_barrier.barrier_flag,
                                     &expected, 1u,
                                     /*weak=*/false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED)) {
        return -BARRIER_EBUSY;
    }

    task_t *self_task = sched_get_current_task();
    spinlock_acquire(&g_snap_barrier.lock);
    g_snap_barrier.acks               = 0;
    g_snap_barrier.parked_head        = NULL;
    g_snap_barrier.owner_task         = self_task;
    g_snap_barrier.barrier_seq++;
    g_snap_barrier.barrier_entered_tsc = tsc_is_ready() ? rdtsc() : 0;
    spinlock_release(&g_snap_barrier.lock);

    uint32_t self_cpu  = smp_get_current_cpu();
    uint32_t cpu_count = g_cpu_count;

    // Send a wakeup IPI to every other CPU. The receiver (case 48 in
    // interrupts.c) checks g_snap_barrier.barrier_flag and re-enters
    // schedule(); schedule() then parks the running task.
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i == self_cpu) continue;
        cpu_info_t *info = smp_get_cpu_info(i);
        if (!info) continue;
        apic_send_ipi(info->lapic_id, IPI_VEC_WAKEUP);
    }

    // Poll: each non-owner CPU's runq.current should land on its
    // idle_task (either it parked a real task into BARRIER_WAIT, or it
    // was already idle when the barrier began). 100 ms TSC watchdog.
    bool tsc_ok = tsc_is_ready();
    uint64_t deadline_tsc = tsc_ok
        ? (g_snap_barrier.barrier_entered_tsc + ns_to_tsc(SNAP_BARRIER_TIMEOUT_NS))
        : 0;

    for (;;) {
        bool all_idle = true;
        for (uint32_t i = 0; i < cpu_count; i++) {
            if (i == self_cpu) continue;
            runq_t *rq    = &g_cpu_locals[i].runq;
            task_t *cur   = (task_t *)__atomic_load_n(&rq->current, __ATOMIC_ACQUIRE);
            task_t *idle  = rq->idle_task;
            if (idle == NULL) {
                // Pre-AP-release: that CPU has no idle task yet, treat
                // as "nothing running there" — safe to proceed.
                continue;
            }
            if (cur != idle) {
                all_idle = false;
                break;
            }
        }
        if (all_idle) return 0;

        if (tsc_ok) {
            if (rdtsc() > deadline_tsc) {
                // Stall — abort. Clear flag so the system recovers.
                __atomic_store_n(&g_snap_barrier.barrier_flag, 0u,
                                 __ATOMIC_RELEASE);
                klog(KLOG_ERROR, SUBSYS_CORE,
                     "snap_begin_barrier: 100ms watchdog fired (acks=%u)",
                     (unsigned)g_snap_barrier.acks);
                return -BARRIER_ETIME;
            }
        }
        // Tiny relax — let other CPUs make progress.
        __asm__ volatile("pause" ::: "memory");
    }
}

// W14.1 — release the barrier window. Walks the parked list and re-
// enqueues each task as READY; sends one wakeup IPI per affected CPU.
void snap_end_barrier(void) {
    spinlock_acquire(&g_snap_barrier.lock);
    task_t *head = g_snap_barrier.parked_head;
    g_snap_barrier.parked_head = NULL;
    g_snap_barrier.owner_task  = NULL;
    spinlock_release(&g_snap_barrier.lock);

    // Clear the flag BEFORE re-enqueueing; otherwise the parked tasks
    // would just be re-parked when their CPUs run schedule() next.
    __atomic_store_n(&g_snap_barrier.barrier_flag, 0u, __ATOMIC_RELEASE);

    // Walk the parked list, flipping each task to READY and putting it
    // back on its preferred runq.
    while (head) {
        task_t *next = head->barrier_next;
        head->barrier_next = NULL;
        head->state = TASK_STATE_READY;
        sched_enqueue_ready(head);
        head = next;
    }

    // Wakeup IPI to every other CPU so newly-ready tasks get dispatched
    // promptly rather than at the next 10 ms timer tick.
    uint32_t self_cpu  = smp_get_current_cpu();
    uint32_t cpu_count = g_cpu_count;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i == self_cpu) continue;
        cpu_info_t *info = smp_get_cpu_info(i);
        if (!info) continue;
        apic_send_ipi(info->lapic_id, IPI_VEC_WAKEUP);
    }
}
