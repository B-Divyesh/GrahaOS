// kernel/fs/recluster.h
//
// Phase 19 — per-mount recluster work queue.
//
// Kernel-internal FIFO where `grahafs_v2_close` enqueues (inode_num, simhash)
// after it has synchronously stamped the SimHash into the inode. A lone
// kernel thread pops jobs and calls `cluster_v2_assign_inode` (which runs
// the Sequential Leader algorithm and journals the inode update).
//
// This is NOT a submission stream. Streams are submitter-owned, cap-gated
// user rings; a kernel-internal "compute + update inode" job has none of
// those properties. See Phase 19 plan Tricky Bit #5.
//
// Overflow policy: if the queue is full, `recluster_enqueue` returns
// silently — reclustering is best-effort. The inode's cluster_id just
// stays stale until the next close that does queue.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialise the work queue + spawn the worker thread. Must be called AFTER
// sched_init (Phase 18 invariant P18.3).
void recluster_init(void);

// Enqueue an inode for re-cluster. Best-effort.
void recluster_enqueue(uint32_t inode_num);

// Diagnostics.
uint32_t recluster_queue_depth(void);
uint64_t recluster_jobs_processed(void);
