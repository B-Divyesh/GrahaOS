// kernel/fs/gc.c
//
// Phase 19 — version GC (count-only retention; see gc.h).

#define __GRAHAFS_V2_INTERNAL__
#include "gc.h"
#include "grahafs_v2.h"
#include "journal.h"
#include "segment.h"
#include "../log.h"
#include "../audit.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../lib/crc32.h"
#include "../mm/kheap.h"

#include <stddef.h>
#include <string.h>

// Externals from grahafs_v2.c.
extern grahafs_v2_inode_cache_t *inode_cache_get(uint32_t inode_num);
extern void                      inode_cache_put(grahafs_v2_inode_cache_t *e);
extern int                       journal_stage_inode_external(journal_txn_t *txn,
                                                              uint32_t inode_num,
                                                              const grahafs_v2_inode_t *ino);

extern volatile uint64_t g_timer_ticks;

static bool    g_gc_initialized = false;
static bool    g_gc_running = false;
static uint64_t g_gc_passes = 0;

// The version chain is stored as a chain of version_record blocks linked by
// `prev_version`. The in-memory mirror (grahafs_v2_chain_pop_tail) lets us
// pop the oldest entry and decrement the EXACT segment that record lived
// in — fixing the previous MVP that always decremented the head segment's
// refcount.
//
// Cold-boot fallback (chain not yet rebuilt): we still decrement
// version_chain_segment as an approximation. After a few writes the chain
// is rebuilt and accurate.

// Age-based retention. Set non-zero (ns) to enable; default 0 means
// retention is count-only. Externally settable so tests can shorten the
// window. 7-day default = 7 * 86400 * 1e9.
uint64_t g_gc_max_age_ns = 0;

void gc_set_max_age_ns(uint64_t ns) { g_gc_max_age_ns = ns; }

extern int64_t rtc_now_seconds(void);

uint32_t gc_prune_inode(uint32_t inode_num) {
    if (!grahafs_v2_is_mounted()) return 0;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return 0;
    grahafs_v2_inode_t work = ce->disk;
    if (work.magic != GRAHAFS_V2_INODE_MAGIC) {
        inode_cache_put(ce); return 0;
    }

    // Compute prune count: count-based + age-based.
    uint64_t now_ns = (uint64_t)rtc_now_seconds() * 1000000000ULL;
    uint32_t to_prune_count = 0;
    if (work.version_count > GRAHAFS_V2_MAX_VERSIONS) {
        to_prune_count = work.version_count - GRAHAFS_V2_MAX_VERSIONS;
    }
    uint32_t to_prune_age = 0;
    if (g_gc_max_age_ns != 0) {
        // Walk the in-memory chain tail-ward counting entries older than
        // the threshold. Bounded by chain length (currently <= 16).
        grahafs_v2_version_entry_t *e = ce->version_chain_head_cached;
        while (e) {
            if (e->timestamp_ns != 0 &&
                now_ns > e->timestamp_ns &&
                (now_ns - e->timestamp_ns) > g_gc_max_age_ns) {
                to_prune_age++;
            }
            e = e->next;
        }
    }
    uint32_t to_prune = to_prune_count > to_prune_age ? to_prune_count : to_prune_age;
    if (to_prune == 0) {
        inode_cache_put(ce); return 0;
    }

    uint32_t pruned = 0;

    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return 0; }

    for (uint32_t i = 0; i < to_prune; ++i) {
        // Pop the oldest in-memory entry — that's the precise record we're
        // pruning. If the chain is empty (cold boot), fall back to the
        // approximation that decrements the head's segment.
        grahafs_v2_version_entry_t *tail = grahafs_v2_chain_pop_tail(ce);
        uint32_t target_seg = tail ? tail->segment_id
                                   : work.version_chain_segment;
        if (target_seg != 0) {
            segment_ref_dec(target_seg);
            (void)segment_reclaim_if_eligible(target_seg);
        }
        if (tail) kfree(tail);
        if (work.version_count > 0) work.version_count--;
        pruned++;
    }

    work.checksum_inode = 0;
    work.checksum_inode = crc32_buf(&work,
        offsetof(grahafs_v2_inode_t, checksum_inode));
    int rc = journal_stage_inode_external(txn, inode_num, &work);
    if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return 0; }
    rc = journal_txn_commit(txn);
    if (rc != 0) { inode_cache_put(ce); return 0; }

    spinlock_acquire(&ce->lock);
    ce->disk = work;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return pruned;
}

uint32_t gc_scan_all(gc_stats_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!grahafs_v2_is_mounted()) return 0;
    const grahafs_v2_superblock_t *sb = grahafs_v2_sb();
    uint32_t total_pruned = 0;
    uint32_t total_segs_reclaimed = 0;
    uint32_t scanned = 0;
    for (uint32_t ino = 2; ino < sb->inode_count_max; ++ino) {
        grahafs_v2_inode_cache_t *ce = inode_cache_get(ino);
        if (!ce) continue;
        bool has_versions = ce->disk.version_count > GRAHAFS_V2_MAX_VERSIONS;
        inode_cache_put(ce);
        scanned++;
        if (has_versions) {
            total_pruned += gc_prune_inode(ino);
        }
    }
    total_segs_reclaimed = segment_count_reclaimable();
    if (out) {
        out->inodes_scanned    = scanned;
        out->versions_pruned   = total_pruned;
        out->segments_reclaimed = total_segs_reclaimed;
    }
    audit_write_fs_gc_now(0, total_pruned, total_segs_reclaimed);
    return total_pruned;
}

static void gc_periodic_task(void) {
    klog(KLOG_INFO, SUBSYS_FS, "gc: periodic worker started (30 s interval)");
    g_gc_running = true;
    // Initial delay — don't run during early boot.
    uint64_t start = g_timer_ticks;
    while (g_timer_ticks - start < 1000) asm volatile("hlt");

    while (g_gc_running) {
        uint64_t t0 = g_timer_ticks;
        while (g_timer_ticks - t0 < 3000) asm volatile("hlt");  // 30s @ 100 Hz.
        if (!grahafs_v2_is_mounted()) continue;
        gc_stats_t s;
        (void)gc_scan_all(&s);
        g_gc_passes++;
        if (s.versions_pruned > 0 || s.segments_reclaimed > 0) {
            klog(KLOG_INFO, SUBSYS_FS,
                 "gc: pass=%llu scanned=%u pruned=%u reclaimed=%u",
                 (unsigned long long)g_gc_passes, s.inodes_scanned,
                 s.versions_pruned, s.segments_reclaimed);
        }
    }
}

void gc_init(void) {
    if (g_gc_initialized) return;
    int tid = sched_create_task(gc_periodic_task);
    if (tid < 0) {
        klog(KLOG_ERROR, SUBSYS_FS, "gc_init: worker spawn failed");
        return;
    }
    g_gc_initialized = true;
    klog(KLOG_INFO, SUBSYS_FS, "gc_init: worker tid=%d", tid);
}
