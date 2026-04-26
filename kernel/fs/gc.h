// kernel/fs/gc.h
//
// Phase 19 — version-chain garbage collector.
//
// Two retention policies, both active simultaneously:
//   1. Count-based: prune to GRAHAFS_V2_MAX_VERSIONS (16) versions per file.
//   2. Age-based: prune entries older than g_gc_max_age_ns (default 0 = off).
//      Set via gc_set_max_age_ns(). 7-day retention example:
//        gc_set_max_age_ns(7ULL * 86400ULL * 1000000000ULL).
//
// On each scan pass, walk the inode table; for every inode with
// versions to prune, pop the oldest in-memory entry (tail of the
// chain), decrement the EXACT segment that record lived in, and journal
// the inode update.
//
// Entry points:
//   gc_init        — spawn periodic 30-second worker after sched_init.
//   gc_scan_all    — synchronous pass invoked by SYS_FS_GC_NOW.
//   gc_prune_inode — single-inode pass for tests and /bin/gc builtin.
//
// Lock hierarchy (Plan §Tricky Bit #3):
//   inode_lock → segment_table_lock.
//   GC MUST NOT acquire append_lock before inode_lock, since the write
//   path takes them in inode_lock → append_lock order.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct gc_stats {
    uint32_t inodes_scanned;
    uint32_t versions_pruned;
    uint32_t segments_reclaimed;
} gc_stats_t;

void gc_init(void);

// Synchronous — called from SYS_FS_GC_NOW. Writes stats to `out` if non-NULL.
// Returns total versions pruned.
uint32_t gc_scan_all(gc_stats_t *out);

// Single-inode prune. Returns versions pruned from this inode's chain.
uint32_t gc_prune_inode(uint32_t inode_num);

// Configure age-based retention. 0 disables (count-only). Otherwise any
// version older than `ns` nanoseconds (compared to wall clock) is pruned.
void gc_set_max_age_ns(uint64_t ns);
extern uint64_t g_gc_max_age_ns;
