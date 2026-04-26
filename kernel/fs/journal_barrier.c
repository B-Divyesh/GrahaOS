// kernel/fs/journal_barrier.c
//
// Phase 19 — device-level write barrier. See header for contract + commit
// protocol annotation.

#include "journal_barrier.h"

#include "blk_client.h"
#include "../log.h"

volatile uint32_t g_journal_barrier_fault_inject = 0;

int journal_barrier(int port_num) {
    if (g_journal_barrier_fault_inject) {
        // Single-shot fault injection: consume the probe and return -EIO.
        g_journal_barrier_fault_inject = 0;
        klog(KLOG_WARN, SUBSYS_FS,
             "journal_barrier: fault-injection trip port=%d -> -EIO", port_num);
        return -5;  // -EIO in cap errno convention.
    }
    int rc = grahafs_block_flush((uint8_t)port_num);
    if (rc != 0) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "journal_barrier: grahafs_block_flush(%d) failed rc=%d",
             port_num, rc);
    }
    return rc;
}
