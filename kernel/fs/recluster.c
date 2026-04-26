// kernel/fs/recluster.c
//
// Phase 19 — recluster work queue. See recluster.h.

#define __GRAHAFS_V2_INTERNAL__
#include "recluster.h"
#include "cluster_v2.h"
#include "grahafs_v2.h"

#include "../log.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

#include <stddef.h>
#include <string.h>

extern volatile uint64_t g_timer_ticks;

#define RECLUSTER_Q_DEPTH 128u

static uint32_t        g_queue[RECLUSTER_Q_DEPTH];
static uint32_t        g_q_head = 0;
static uint32_t        g_q_tail = 0;
static uint32_t        g_q_count = 0;
static spinlock_t      g_q_lock = SPINLOCK_INITIALIZER("v2_recluster");
static uint64_t        g_jobs_done = 0;
static bool            g_running = false;
static bool            g_initialized = false;

static void recluster_worker_thread(void) {
    klog(KLOG_INFO, SUBSYS_FS, "recluster: worker thread started");
    g_running = true;
    while (g_running) {
        uint32_t inode_num = 0;
        bool have_job = false;

        spinlock_acquire(&g_q_lock);
        if (g_q_count > 0) {
            inode_num = g_queue[g_q_head];
            g_q_head = (g_q_head + 1) % RECLUSTER_Q_DEPTH;
            g_q_count--;
            have_job = true;
        }
        spinlock_release(&g_q_lock);

        if (have_job) {
            if (grahafs_v2_is_mounted()) {
                (void)cluster_v2_assign_inode(inode_num);
                g_jobs_done++;
            }
            // Drain aggressively — no yield when work remains.
            continue;
        }

        // Idle: hlt until next timer tick arrives.
        uint64_t t0 = g_timer_ticks;
        while (g_timer_ticks - t0 < 1) {
            asm volatile("hlt");
        }
    }
}

void recluster_init(void) {
    if (g_initialized) return;
    spinlock_init(&g_q_lock, "v2_recluster");
    g_q_head = g_q_tail = g_q_count = 0;
    memset(g_queue, 0, sizeof(g_queue));
    int tid = sched_create_task(recluster_worker_thread);
    if (tid < 0) {
        klog(KLOG_ERROR, SUBSYS_FS, "recluster_init: worker spawn failed");
        return;
    }
    g_initialized = true;
    klog(KLOG_INFO, SUBSYS_FS, "recluster_init: worker tid=%d", tid);
}

void recluster_enqueue(uint32_t inode_num) {
    if (!g_initialized) return;
    spinlock_acquire(&g_q_lock);
    if (g_q_count >= RECLUSTER_Q_DEPTH) {
        spinlock_release(&g_q_lock);
        return;  // Best-effort — drop.
    }
    g_queue[g_q_tail] = inode_num;
    g_q_tail = (g_q_tail + 1) % RECLUSTER_Q_DEPTH;
    g_q_count++;
    spinlock_release(&g_q_lock);
}

uint32_t recluster_queue_depth(void) {
    spinlock_acquire(&g_q_lock);
    uint32_t n = g_q_count;
    spinlock_release(&g_q_lock);
    return n;
}

uint64_t recluster_jobs_processed(void) { return g_jobs_done; }
