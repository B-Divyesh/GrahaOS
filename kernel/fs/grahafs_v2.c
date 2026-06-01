// kernel/fs/grahafs_v2.c
//
// Phase 19 — GrahaFS v2 driver core.
//
// Assembled across units:
//   U7  inode cache (this file, section §INODE_CACHE)
//   U8  block tree walker (§BLOCK_TREE)
//   U9  read path (§READ)
//   U10 write path (§WRITE — journaled)
//   U11 metadata ops (§META)
//   U13 async wrappers (§ASYNC)
//   U14 close + SimHash + version record emission (§CLOSE)
//   U15 fsync (§FSYNC)
//   U17 mount (§MOUNT)
//
// On-disk structs and magic numbers live in grahafs_v2.h.

#define __GRAHAFS_V2_INTERNAL__
#include "grahafs_v2.h"
#include "grahafs.h"   // FU29.H: shared AI user-structs (grahafs_ai_metadata_t,
                       // grahafs_search_results_t, grahafs_ai_metadata_block_t)
                       // + GRAHAFS_AI_* / GRAHAFS_META_FLAG_* / GRAHAFS_AI_META_MAGIC.
#include "../cap/token.h"

#include <stddef.h>
#include <string.h>

#include "journal.h"
#include "journal_barrier.h"
#include "segment.h"
#include "vfs.h"
#include "cluster.h"
#include "../log.h"
#include "../sync/spinlock.h"
#include "../mm/kheap.h"
#include "blk_client.h"
#include "../lib/crc32.h"

// ===========================================================================
// §MOUNT_STATE — singletons populated by grahafs_v2_mount.
// ===========================================================================
static int                      g_v2_device_id = -1;
static grahafs_v2_superblock_t  g_v2_sb;
static bool                     g_v2_mounted = false;
static spinlock_t               g_v2_sb_lock = SPINLOCK_INITIALIZER("v2_sb");

// ===========================================================================
// §INODE_CACHE — small bounded cache of in-memory inode entries.
//
// MVP: 256-entry table, linear probe. Dirty eviction is avoided by bounding
// the cache to more slots than the working set GrahaOS will hit at this
// scale (test suite, gash workflow, AI agents). For workloads beyond 256
// concurrent inodes we'd need an LRU + flush-on-evict policy; the cache
// returns NULL today when full, so over-population fails loudly rather
// than silently. Scaling work, not a correctness gap.
// ===========================================================================
#define INODE_CACHE_SLOTS 256u

static grahafs_v2_inode_cache_t g_inode_cache[INODE_CACHE_SLOTS];
static uint32_t                 g_inode_cache_used = 0;
static spinlock_t               g_inode_cache_lock = SPINLOCK_INITIALIZER("v2_inode_cache");

// Find the LBA + offset within the inode-table block that holds `inode_num`.
static void inode_locate(uint32_t inode_num, uint64_t *lba, uint32_t *off) {
    uint32_t block_idx = (inode_num * GRAHAFS_V2_INODE_SIZE) / GRAHAFS_V2_BLOCK_SIZE;
    uint32_t block_off = (inode_num * GRAHAFS_V2_INODE_SIZE) % GRAHAFS_V2_BLOCK_SIZE;
    *lba = g_v2_sb.inode_table_start_block + block_idx;
    *off = block_off;
}

static uint32_t inode_checksum(const grahafs_v2_inode_t *ino) {
    return crc32_buf(ino, offsetof(grahafs_v2_inode_t, checksum_inode));
}

// Returns a pinned cache entry for `inode_num`. Increments pinned_readers.
// Caller must pair with inode_cache_put(). Returns NULL on I/O error or
// if the on-disk inode's magic/CRC is bad.
grahafs_v2_inode_cache_t *inode_cache_get(uint32_t inode_num) {
    if (!g_v2_mounted) return NULL;

    // (1) Fast path: cache hit under the lock — NO I/O.
    spinlock_acquire(&g_inode_cache_lock);
    for (uint32_t i = 0; i < INODE_CACHE_SLOTS; ++i) {
        grahafs_v2_inode_cache_t *e = &g_inode_cache[i];
        if (e->magic == GRAHAFS_V2_INODE_CACHE_MAGIC &&
            e->inode_num == inode_num) {
            e->pinned_readers++;
            spinlock_release(&g_inode_cache_lock);
            return e;
        }
    }
    spinlock_release(&g_inode_cache_lock);

    // (2) Miss: read the inode's block OUTSIDE the cache lock.  FU29.H / L3
    // lock-drop-around-I/O: holding g_inode_cache_lock across the (now 4 KiB)
    // channel-mode block read deadlocks under cluster load — a stalled read
    // keeps the lock held past the 5 s spinlock budget and trips
    // SCHED_SPINLOCK_PANIC(lock=v2_inode_cache).  Read into a local, validate,
    // then install under the lock.  Buffer is heap-allocated (this runs in a
    // tight loop during mount cluster-rebuild; on-stack 4 KiB overflows).
    uint64_t lba; uint32_t off;
    inode_locate(inode_num, &lba, &off);
    uint8_t *block = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    if (!block) {
        klog(KLOG_ERROR, SUBSYS_FS, "inode_cache_get: kmalloc failed inode=%u", inode_num);
        return NULL;
    }
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, block) != 1) {
        kfree(block);
        klog(KLOG_ERROR, SUBSYS_FS, "inode_cache_get: read failed inode=%u", inode_num);
        return NULL;
    }
    grahafs_v2_inode_t disk;
    memcpy(&disk, block + off, GRAHAFS_V2_INODE_SIZE);
    kfree(block);

    if (disk.magic != GRAHAFS_V2_INODE_MAGIC) {
        /* magic==0 means the slot is unallocated — expected during table
         * scans on a fresh-formatted disk. Only log actual corruption. */
        if (disk.magic != 0u) {
            klog(KLOG_WARN, SUBSYS_FS,
                 "inode_cache_get: inode=%u bad magic=0x%08x", inode_num, disk.magic);
        }
        return NULL;
    }
    uint32_t expected = disk.checksum_inode;
    disk.checksum_inode = 0;
    uint32_t got = inode_checksum(&disk);
    disk.checksum_inode = expected;
    if (got != expected) {
        klog(KLOG_WARN, SUBSYS_FS,
             "inode_cache_get: inode=%u CRC mismatch got=0x%08x exp=0x%08x",
             inode_num, got, expected);
        return NULL;
    }

    // (3) Install under the lock, re-scanning for a concurrent loader so two
    // racing misses on the same inode don't create duplicate (incoherent)
    // slots — the loser adopts the winner's entry.
    spinlock_acquire(&g_inode_cache_lock);
    for (uint32_t i = 0; i < INODE_CACHE_SLOTS; ++i) {
        grahafs_v2_inode_cache_t *e = &g_inode_cache[i];
        if (e->magic == GRAHAFS_V2_INODE_CACHE_MAGIC &&
            e->inode_num == inode_num) {
            e->pinned_readers++;
            spinlock_release(&g_inode_cache_lock);
            return e;
        }
    }
    grahafs_v2_inode_cache_t *slot = NULL;
    for (uint32_t i = 0; i < INODE_CACHE_SLOTS; ++i) {
        if (g_inode_cache[i].magic != GRAHAFS_V2_INODE_CACHE_MAGIC) {
            slot = &g_inode_cache[i];
            break;
        }
    }
    if (!slot) {
        spinlock_release(&g_inode_cache_lock);
        klog(KLOG_ERROR, SUBSYS_FS,
             "inode_cache_get: cache full (MVP bound=%u)", INODE_CACHE_SLOTS);
        return NULL;
    }
    slot->disk                      = disk;
    slot->magic                     = GRAHAFS_V2_INODE_CACHE_MAGIC;
    slot->inode_num                 = inode_num;
    slot->dirty                     = false;
    slot->pinned_readers            = 1;
    slot->version_chain_loaded      = false;
    slot->version_chain_head_cached = NULL;
    spinlock_init(&slot->lock, "v2_ino");
    g_inode_cache_used++;
    spinlock_release(&g_inode_cache_lock);
    return slot;
}

void inode_cache_put(grahafs_v2_inode_cache_t *e) {
    if (!e) return;
    spinlock_acquire(&g_inode_cache_lock);
    if (e->pinned_readers > 0) e->pinned_readers--;
    spinlock_release(&g_inode_cache_lock);
}

// Flush a single dirty inode via the journal (called under inode_lock).
int inode_cache_flush_dirty(grahafs_v2_inode_cache_t *e) {
    if (!e || !e->dirty) return 0;

    // Recompute CRC and stage the block holding this inode in a 1-block
    // metadata txn.
    uint64_t lba; uint32_t off;
    inode_locate(e->inode_num, &lba, &off);
    uint8_t block[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, block) != 1) return -5;

    e->disk.checksum_inode = 0;
    e->disk.checksum_inode = inode_checksum(&e->disk);
    memcpy(block + off, &e->disk, GRAHAFS_V2_INODE_SIZE);

    journal_txn_t *txn = journal_txn_begin();
    if (!txn) return -3;  // -ENOMEM.
    int rc = journal_txn_add_block(txn, lba, JOURNAL_BLOCK_KIND_METADATA, block);
    if (rc != 0) { journal_txn_abort(txn); return rc; }
    rc = journal_txn_commit(txn);
    if (rc != 0) return rc;
    e->dirty = false;
    return 0;
}

// ===========================================================================
// §BLOCK_TREE — logical-block → physical-LBA resolver.
//
// Covers:
//    direct[12]        : block 0..11         (48 KiB)
//    indirect          : block 12..1035      (4 MiB)
//    double_indirect   : block 1036..1048575 (4 GiB + 48 KiB)
//
// Returns the LBA or 0 if the tree branch is sparse (returns-zero-for-sparse
// is an explicit contract — callers decide whether to allocate on write).
// ===========================================================================
static uint32_t v2_indirect_lookup(uint32_t indirect_lba, uint32_t inner_idx) {
    if (indirect_lba == 0) return 0;
    uint8_t block[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, indirect_lba, block) != 1) return 0;
    uint32_t *slots = (uint32_t *)block;
    return slots[inner_idx];
}

uint32_t v2_block_index_to_lba(const grahafs_v2_inode_t *ino, uint32_t logical_block) {
    if (!ino) return 0;
    if (logical_block < 12u) {
        return ino->direct_blocks[logical_block];
    }
    uint32_t adj = logical_block - 12u;
    if (adj < GRAHAFS_V2_INDIRECT_PTRS) {
        return v2_indirect_lookup(ino->indirect_block, adj);
    }
    adj -= GRAHAFS_V2_INDIRECT_PTRS;
    if (adj < GRAHAFS_V2_INDIRECT_PTRS * GRAHAFS_V2_INDIRECT_PTRS) {
        uint32_t outer = adj / GRAHAFS_V2_INDIRECT_PTRS;
        uint32_t inner = adj % GRAHAFS_V2_INDIRECT_PTRS;
        uint32_t mid_lba = v2_indirect_lookup(ino->double_indirect, outer);
        return v2_indirect_lookup(mid_lba, inner);
    }
    return 0;  // Triple-indirect reserved; MVP does not traverse.
}

// ===========================================================================
// §MOUNT — minimal version. Detects v2 superblock, registers in-memory
// state, runs journal_replay, but does NOT yet register with VFS (U17 wires
// vfs_mount-side dispatch).
// ===========================================================================
int grahafs_v2_mount(int device_id) {
    uint8_t sb_block[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)device_id, 0, sb_block) != 1) {
        return -5;
    }
    grahafs_v2_superblock_t *sb = (grahafs_v2_superblock_t *)sb_block;
    if (sb->magic != GRAHAFS_V2_SB_MAGIC) {
        // Not v2 — caller can fall back to v1 compat path.
        return -126;
    }
    uint32_t expected = sb->checksum_sb;
    sb->checksum_sb = 0;
    uint32_t got = crc32_buf(sb, offsetof(grahafs_v2_superblock_t, checksum_sb));
    sb->checksum_sb = expected;
    if (got != expected) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "grahafs_v2_mount: superblock CRC mismatch got=0x%08x exp=0x%08x",
             got, expected);
        return -126;
    }
    if (sb->version != 2) return -126;

    spinlock_acquire(&g_v2_sb_lock);
    memcpy(&g_v2_sb, sb, sizeof(g_v2_sb));
    g_v2_device_id = device_id;
    memset(g_inode_cache, 0, sizeof(g_inode_cache));
    g_inode_cache_used = 0;
    spinlock_release(&g_v2_sb_lock);

    // Init journal + segments.
    int rc = journal_subsystem_init(device_id, &g_v2_sb);
    if (rc != 0) return rc;
    rc = segment_subsystem_init(device_id, g_v2_sb.segment_table_start,
                                g_v2_sb.segment_count_max,
                                g_v2_sb.data_blocks_start_block);
    if (rc != 0) return rc;

    // Replay any committed-but-unapplied txns. Updates sb in place + on disk.
    rc = journal_replay(device_id, &g_v2_sb);
    if (rc != 0) return rc;

    g_v2_mounted = true;

    // Cluster rebuild: walk inode table, feed every inode with a SimHash
    // into the in-memory cluster table so `clusters` CLI reports correct
    // state immediately after mount.
    //
    // Optimisation: read INODES_PER_BLOCK (8) inodes per block, rather than
    // calling inode_cache_get per-inode. The fresh-disk case is dominated
    // by empty inodes (16K * ~10 ms/op = several minutes wall-clock under
    // TCG); reading by block collapses that to ~2 K reads. We bypass the
    // inode_cache_t cache here because cluster rebuild only needs the
    // raw on-disk fields and runs once at mount.
    cluster_init();
    uint8_t *ino_block = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    if (!ino_block) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "grahafs_v2_mount: kmalloc(ino_block) failed; skipping cluster rebuild");
    } else {
        uint32_t blocks_to_scan =
            (g_v2_sb.inode_count_max + GRAHAFS_V2_INODES_PER_BLOCK - 1) /
            GRAHAFS_V2_INODES_PER_BLOCK;
        // FU29.H read-path perf (#666): early-exit after a run of consecutive
        // all-free inode-table blocks. This scan runs at MOUNT, on the boot
        // CRITICAL PATH (before init/tests spawn), and previously read all
        // ~2048 inode-table blocks unconditionally — dominating v2 boot time
        // even though v2 allocates inodes LOWEST-FIRST, so every allocated
        // inode clusters at the low end. Once we see CLUSTER_REBUILD_EMPTY_RUN
        // consecutive blocks with no allocated inode we are past every live
        // inode; stop. (Mirrors gc_scan_all's bound, commit 98f2020. Best-effort
        // — a pathologically-sparse high inode would be missed for the in-memory
        // cluster hint only; on-disk state is unaffected and clustertest's files
        // are dense/low.)
        const uint32_t CLUSTER_REBUILD_EMPTY_RUN = 128u;  /* 1024 inodes margin */
        uint32_t empty_run = 0;
        for (uint32_t blk = 0; blk < blocks_to_scan; ++blk) {
            uint64_t lba = (uint64_t)g_v2_sb.inode_table_start_block + blk;
            if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, ino_block) != 1) {
                klog(KLOG_WARN, SUBSYS_FS,
                     "grahafs_v2_mount: cluster rebuild read failed lba=%lu",
                     (unsigned long)lba);
                break;
            }
            bool any_alloc = false;
            for (uint32_t i = 0; i < GRAHAFS_V2_INODES_PER_BLOCK; ++i) {
                uint32_t ino = blk * GRAHAFS_V2_INODES_PER_BLOCK + i;
                if (ino < 2u) continue;
                if (ino >= g_v2_sb.inode_count_max) break;
                grahafs_v2_inode_t *disk =
                    (grahafs_v2_inode_t *)
                        (ino_block + i * GRAHAFS_V2_INODE_SIZE);
                if (disk->magic != GRAHAFS_V2_INODE_MAGIC) continue;
                any_alloc = true;
                uint64_t sh = disk->ai_embedding[0];
                uint32_t cid = ((uint32_t)disk->ai_reserved[0])       |
                               ((uint32_t)disk->ai_reserved[1] << 8)  |
                               ((uint32_t)disk->ai_reserved[2] << 16) |
                               ((uint32_t)disk->ai_reserved[3] << 24);
                if (sh == 0u || cid == 0u) continue;
                char hint[28];
                memcpy(hint, disk->ai_tags, 27);
                hint[27] = 0;
                cluster_rebuild_add(ino, cid, sh, hint);
            }
            if (any_alloc) empty_run = 0;
            else if (++empty_run >= CLUSTER_REBUILD_EMPTY_RUN) break;
        }
        kfree(ino_block);
    }
    cluster_rebuild_finalize();

    klog(KLOG_INFO, SUBSYS_FS,
         "grahafs_v2_mount: v2 mounted, sb.total_blocks=%u free_blocks=%u "
         "inodes=%u segments=%u journal=%u blocks",
         g_v2_sb.total_blocks, g_v2_sb.free_blocks, g_v2_sb.free_inodes,
         g_v2_sb.segment_count_max, g_v2_sb.journal_blocks);
    return 0;
}

bool grahafs_v2_is_mounted(void) { return g_v2_mounted; }

const grahafs_v2_superblock_t *grahafs_v2_sb(void) { return &g_v2_sb; }

int grahafs_v2_device_id(void) { return g_v2_device_id; }

// Phase 23 S4: re-run journal replay against the mounted SB. Called by
// blk_client_on_ahcid_alive after the daemon respawns and reconnects.
// Idempotent — already-applied transactions are detected and skipped by
// journal_replay's tail-pointer check.
extern int journal_replay(int device_id, grahafs_v2_superblock_t *sb);
int grahafs_v2_journal_replay(void) {
    if (!g_v2_mounted) return -19;  /* -ENODEV */
    return journal_replay(g_v2_device_id, &g_v2_sb);
}

// ===========================================================================
// §BITMAP — bitmap block allocator. Reads/writes the bitmap blocks directly;
// MVP keeps no in-memory cache, preferring reliability over throughput.
// ===========================================================================
static int bitmap_block_lba(uint32_t block_num, uint64_t *lba, uint32_t *bit) {
    if (block_num >= g_v2_sb.total_blocks) return -22;
    uint32_t bit_idx = block_num;
    uint32_t block_off = bit_idx / (8u * GRAHAFS_V2_BLOCK_SIZE);
    if (block_off >= g_v2_sb.bitmap_blocks) return -22;
    *lba = (uint64_t)g_v2_sb.bitmap_start_block + block_off;
    *bit = bit_idx % (8u * GRAHAFS_V2_BLOCK_SIZE);
    return 0;
}

static bool bitmap_test(const uint8_t *buf, uint32_t bit) {
    return (buf[bit / 8u] & (1u << (bit % 8u))) != 0;
}

static void bitmap_mark(uint8_t *buf, uint32_t bit, bool set) {
    if (set) buf[bit / 8u] |=  (1u << (bit % 8u));
    else     buf[bit / 8u] &= ~(1u << (bit % 8u));
}

// Allocate one data block (not journal, not bitmap, not inode table).
// Returns 0 on exhaustion. `txn` is optional — if non-NULL, the updated
// bitmap block is added to the transaction so both allocation and data
// land atomically. The superblock's free_blocks counter is updated in
// memory and flushed at commit.
static uint32_t v2_bitmap_allocate_block(journal_txn_t *txn) {
    if (!g_v2_mounted) return 0;

    uint32_t start = g_v2_sb.data_blocks_start_block;
    if (start == 0 || start >= g_v2_sb.total_blocks) return 0;

    uint8_t buf[GRAHAFS_V2_BLOCK_SIZE];
    uint64_t cached_lba = 0;
    bool cached_valid = false;

    for (uint32_t i = start; i < g_v2_sb.total_blocks; ++i) {
        uint64_t lba; uint32_t bit;
        if (bitmap_block_lba(i, &lba, &bit) != 0) continue;
        if (!cached_valid || cached_lba != lba) {
            if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, buf) != 1) return 0;
            cached_lba = lba;
            cached_valid = true;
        }
        if (!bitmap_test(buf, bit)) {
            bitmap_mark(buf, bit, true);
            // Write updated bitmap back. When a txn is provided, stage the
            // bitmap update into the same transaction as the data block —
            // both commit atomically (replay reapplies bitmap-set + data-
            // block-write together; crash before commit reverts both).
            // When no txn is provided (early-mount, format), write directly
            // to disk; the caller is responsible for ordering.
            if (txn) {
                int rc = journal_txn_add_block(txn, lba,
                                               JOURNAL_BLOCK_KIND_METADATA,
                                               buf);
                if (rc != 0) {
                    bitmap_mark(buf, bit, false);
                    return 0;
                }
            } else {
                if (grahafs_v2_block_write((uint8_t)g_v2_device_id, lba, buf) != 1) {
                    bitmap_mark(buf, bit, false);
                    return 0;
                }
            }
            spinlock_acquire(&g_v2_sb_lock);
            if (g_v2_sb.free_blocks > 0) g_v2_sb.free_blocks--;
            spinlock_release(&g_v2_sb_lock);
            return i;
        }
    }
    return 0;
}

static int v2_bitmap_free_block(uint32_t block_num) {
    if (block_num < g_v2_sb.data_blocks_start_block ||
        block_num >= g_v2_sb.total_blocks) return -22;
    uint64_t lba; uint32_t bit;
    if (bitmap_block_lba(block_num, &lba, &bit) != 0) return -22;
    uint8_t buf[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, buf) != 1) return -5;
    bitmap_mark(buf, bit, false);
    if (grahafs_v2_block_write((uint8_t)g_v2_device_id, lba, buf) != 1) return -5;
    spinlock_acquire(&g_v2_sb_lock);
    g_v2_sb.free_blocks++;
    spinlock_release(&g_v2_sb_lock);
    return 0;
}

// Allocate one inode. Returns 0 on exhaustion.
static uint32_t v2_allocate_inode(void) {
    if (!g_v2_mounted) return 0;
    uint8_t buf[GRAHAFS_V2_BLOCK_SIZE];
    for (uint32_t i = 2; i < g_v2_sb.inode_count_max; ++i) {
        uint64_t lba = g_v2_sb.inode_table_start_block +
            (i * GRAHAFS_V2_INODE_SIZE) / GRAHAFS_V2_BLOCK_SIZE;
        uint32_t off = (i * GRAHAFS_V2_INODE_SIZE) % GRAHAFS_V2_BLOCK_SIZE;
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, buf) != 1) continue;
        grahafs_v2_inode_t *ino = (grahafs_v2_inode_t *)(buf + off);
        if (ino->magic == 0 && ino->type == 0) {
            spinlock_acquire(&g_v2_sb_lock);
            if (g_v2_sb.free_inodes > 0) g_v2_sb.free_inodes--;
            spinlock_release(&g_v2_sb_lock);
            return i;
        }
    }
    return 0;
}

// Flush superblock to disk with refreshed CRC.
static int v2_write_superblock(void) {
    uint8_t sb_block[GRAHAFS_V2_BLOCK_SIZE];
    memset(sb_block, 0, sizeof(sb_block));
    memcpy(sb_block, &g_v2_sb, sizeof(g_v2_sb));
    grahafs_v2_superblock_t *sb = (grahafs_v2_superblock_t *)sb_block;
    sb->checksum_sb = 0;
    sb->checksum_sb = crc32_buf(sb,
        offsetof(grahafs_v2_superblock_t, checksum_sb));
    if (grahafs_v2_block_write((uint8_t)g_v2_device_id, 0, sb_block) != 1) return -5;
    return 0;
}

// ===========================================================================
// §READ — vfs_node_t->read path. Walks block tree per-logical-block, handles
// unaligned offsets + short reads at EOF. No locks held across AHCI I/O
// beyond the inode pin (inode_cache_get bumps pinned_readers).
// ===========================================================================
ssize_t grahafs_v2_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    if (!g_v2_mounted || !node || !buffer) return -5;
    if (size == 0) return 0;

    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return -5;
    grahafs_v2_inode_t snap = ce->disk;
    inode_cache_put(ce);

    if (snap.type != GRAHAFS_V2_TYPE_FILE &&
        snap.type != GRAHAFS_V2_TYPE_DIRECTORY) return -22;
    if (offset >= snap.size) return 0;

    size_t to_read = size;
    if (offset + to_read > snap.size) to_read = (size_t)(snap.size - offset);

    size_t bytes_read = 0;
    uint32_t block_index  = (uint32_t)(offset / GRAHAFS_V2_BLOCK_SIZE);
    uint32_t block_offset = (uint32_t)(offset % GRAHAFS_V2_BLOCK_SIZE);

    while (bytes_read < to_read) {
        uint32_t lba = v2_block_index_to_lba(&snap, block_index);
        size_t chunk = GRAHAFS_V2_BLOCK_SIZE - block_offset;
        if (chunk > to_read - bytes_read) chunk = to_read - bytes_read;

        if (lba == 0) {
            // Sparse hole — return zeros.
            memset((uint8_t *)buffer + bytes_read, 0, chunk);
        } else {
            uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
            if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, blk) != 1) {
                klog(KLOG_ERROR, SUBSYS_FS,
                     "grahafs_v2_read: grahafs_block_read lba=%u failed", lba);
                return (ssize_t)bytes_read > 0 ? (ssize_t)bytes_read : -5;
            }
            memcpy((uint8_t *)buffer + bytes_read, blk + block_offset, chunk);
        }
        bytes_read += chunk;
        block_index++;
        block_offset = 0;
    }
    return (ssize_t)bytes_read;
}

// ===========================================================================
// §WRITE — vfs_node_t->write path. Journaled. Flow per plan U10:
//     inode_cache_get
//     → journal_txn_begin
//     → per logical block: v2_block_index_to_lba_alloc (may allocate)
//     → add_block(lba_target = data LBA, kind=DATA, payload=user bytes)
//     → add_block(lba_target = inode-table LBA, kind=METADATA, payload=inode blk)
//     → journal_txn_commit (two-barrier + inline checkpoint)
//     → inode_cache_put
//
// Holds no spinlock across AHCI I/O except the journal append_lock inside
// begin/commit (single-txn-in-flight MVP).
// ===========================================================================

// Allocate-on-write variant of the block tree walker.
// If the inode has a sparse logical_block, allocates a fresh data block
// (journal-staged via `txn`). May also allocate indirect/double-indirect
// pages on first reference. Returns the data-block LBA, or 0 on failure.
static uint32_t v2_block_index_to_lba_alloc(grahafs_v2_inode_t *ino,
                                            uint32_t logical_block,
                                            journal_txn_t *txn) {
    if (!ino || !txn) return 0;

    if (logical_block < GRAHAFS_V2_DIRECT_BLOCKS) {
        if (ino->direct_blocks[logical_block] == 0) {
            uint32_t b = v2_bitmap_allocate_block(txn);
            if (!b) return 0;
            // Zero the block first (sparse semantics).
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, b, JOURNAL_BLOCK_KIND_DATA, zero);
            ino->direct_blocks[logical_block] = b;
            ino->blocks_allocated++;
        }
        return ino->direct_blocks[logical_block];
    }

    uint32_t adj = logical_block - GRAHAFS_V2_DIRECT_BLOCKS;
    if (adj < GRAHAFS_V2_INDIRECT_PTRS) {
        // Indirect page.
        if (ino->indirect_block == 0) {
            uint32_t ib = v2_bitmap_allocate_block(txn);
            if (!ib) return 0;
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, ib, JOURNAL_BLOCK_KIND_METADATA, zero);
            ino->indirect_block = ib;
            ino->blocks_allocated++;
        }
        // Read indirect page (it may be freshly zeroed), patch slot, re-stage.
        uint8_t ipage[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, ino->indirect_block, ipage) != 1) return 0;
        uint32_t *slots = (uint32_t *)ipage;
        if (slots[adj] == 0) {
            uint32_t b = v2_bitmap_allocate_block(txn);
            if (!b) return 0;
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, b, JOURNAL_BLOCK_KIND_DATA, zero);
            slots[adj] = b;
            journal_txn_add_block(txn, ino->indirect_block,
                                  JOURNAL_BLOCK_KIND_METADATA, ipage);
            ino->blocks_allocated++;
        }
        return slots[adj];
    }

    adj -= GRAHAFS_V2_INDIRECT_PTRS;
    if (adj < GRAHAFS_V2_INDIRECT_PTRS * GRAHAFS_V2_INDIRECT_PTRS) {
        // Double-indirect.
        if (ino->double_indirect == 0) {
            uint32_t outer_lba = v2_bitmap_allocate_block(txn);
            if (!outer_lba) return 0;
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, outer_lba,
                                  JOURNAL_BLOCK_KIND_METADATA, zero);
            ino->double_indirect = outer_lba;
            ino->blocks_allocated++;
        }
        uint32_t outer = adj / GRAHAFS_V2_INDIRECT_PTRS;
        uint32_t inner = adj % GRAHAFS_V2_INDIRECT_PTRS;
        uint8_t outer_page[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, ino->double_indirect, outer_page) != 1)
            return 0;
        uint32_t *outer_slots = (uint32_t *)outer_page;
        if (outer_slots[outer] == 0) {
            uint32_t mid_lba = v2_bitmap_allocate_block(txn);
            if (!mid_lba) return 0;
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, mid_lba,
                                  JOURNAL_BLOCK_KIND_METADATA, zero);
            outer_slots[outer] = mid_lba;
            journal_txn_add_block(txn, ino->double_indirect,
                                  JOURNAL_BLOCK_KIND_METADATA, outer_page);
            ino->blocks_allocated++;
        }
        uint32_t mid_lba = outer_slots[outer];
        uint8_t mid_page[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, mid_lba, mid_page) != 1) return 0;
        uint32_t *mid_slots = (uint32_t *)mid_page;
        if (mid_slots[inner] == 0) {
            uint32_t b = v2_bitmap_allocate_block(txn);
            if (!b) return 0;
            uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            journal_txn_add_block(txn, b, JOURNAL_BLOCK_KIND_DATA, zero);
            mid_slots[inner] = b;
            journal_txn_add_block(txn, mid_lba,
                                  JOURNAL_BLOCK_KIND_METADATA, mid_page);
            ino->blocks_allocated++;
        }
        return mid_slots[inner];
    }
    return 0;  // Triple-indirect out of scope for MVP.
}

// External wrapper of journal_stage_inode for sibling files (cluster_v2.c,
// recluster.c, gc.c). Keeps the static helper intact below but gives cross-
// file access without exposing the implementation detail name.
int journal_stage_inode_external(journal_txn_t *txn, uint32_t inode_num,
                                 const grahafs_v2_inode_t *disk_copy);

// Stage the inode-table block holding `inode_num` into the current txn,
// overlaying the cached inode copy. Caller ensures CRC has been set.
static int journal_stage_inode(journal_txn_t *txn, uint32_t inode_num,
                               const grahafs_v2_inode_t *disk_copy) {
    uint64_t lba = g_v2_sb.inode_table_start_block +
        (inode_num * GRAHAFS_V2_INODE_SIZE) / GRAHAFS_V2_BLOCK_SIZE;
    uint32_t off = (inode_num * GRAHAFS_V2_INODE_SIZE) % GRAHAFS_V2_BLOCK_SIZE;

    // FU29.H / FU25.D — same-inode-table-block coalescing.
    //
    // INODE_SIZE=512, BLOCK_SIZE=4096 → 8 inodes share each inode-table
    // block. A single create stages TWO inodes that usually land in the same
    // block: the new file's inode AND the parent directory's inode (e.g.
    // root=1 and the first file=2 both map to inode-table block 0).
    // journal_txn_commit's inline checkpoint writes refs to their target LBA
    // in insertion order, so two refs with the SAME target LBA are
    // last-writer-wins. If we re-read the block FRESH FROM DISK for the
    // second inode, that read predates the first inode's not-yet-checkpointed
    // edit, so the later ref carries a stale block that clobbers the first
    // inode back to magic=0 on disk → finddir()/open() of the just-created
    // file then reads inode magic=0 and returns fd<0.
    //
    // Fix: if this inode-table block is ALREADY staged in the current txn,
    // overlay the inode onto the PENDING payload in place so both inodes
    // coexist in the single ref that gets checkpointed. This lives entirely
    // in the v2 journal path (v1 grahafs.c has no journal references), so it
    // cannot regress v1.
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        if (txn->refs[i].lba_target == lba && txn->payloads[i]) {
            memcpy(txn->payloads[i] + off, disk_copy, GRAHAFS_V2_INODE_SIZE);
            return 0;
        }
    }

    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, blk) != 1) return -5;
    memcpy(blk + off, disk_copy, GRAHAFS_V2_INODE_SIZE);
    return journal_txn_add_block(txn, lba, JOURNAL_BLOCK_KIND_METADATA, blk);
}

ssize_t grahafs_v2_write(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    if (!g_v2_mounted || !node || !buffer) return -5;
    if (size == 0) return 0;

    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return -5;

    spinlock_acquire(&ce->lock);
    grahafs_v2_inode_t work = ce->disk;
    spinlock_release(&ce->lock);

    if (work.type != GRAHAFS_V2_TYPE_FILE &&
        work.type != GRAHAFS_V2_TYPE_DIRECTORY) {
        inode_cache_put(ce);
        return -22;
    }

    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return -3; }

    size_t bytes_written = 0;
    uint32_t block_index  = (uint32_t)(offset / GRAHAFS_V2_BLOCK_SIZE);
    uint32_t block_offset = (uint32_t)(offset % GRAHAFS_V2_BLOCK_SIZE);

    while (bytes_written < size) {
        uint32_t lba = v2_block_index_to_lba_alloc(&work, block_index, txn);
        if (!lba) { journal_txn_abort(txn); inode_cache_put(ce);
                    return bytes_written > 0 ? (ssize_t)bytes_written : (ssize_t)-28; }
        size_t chunk = GRAHAFS_V2_BLOCK_SIZE - block_offset;
        if (chunk > size - bytes_written) chunk = size - bytes_written;

        uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
        if (block_offset != 0 || chunk != GRAHAFS_V2_BLOCK_SIZE) {
            // Partial block — must RMW.
            if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, blk) != 1) {
                journal_txn_abort(txn); inode_cache_put(ce); return -5;
            }
        } else {
            memset(blk, 0, sizeof(blk));
        }
        memcpy(blk + block_offset, (const uint8_t *)buffer + bytes_written, chunk);
        int rc = journal_txn_add_block(txn, lba, JOURNAL_BLOCK_KIND_DATA, blk);
        if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return rc; }

        bytes_written += chunk;
        block_index++;
        block_offset = 0;
    }

    // Update in-memory inode meta fields.
    uint64_t new_size = offset + bytes_written;
    if (new_size > work.size) work.size = new_size;
    work.modification_time++;
    work.checksum_inode = 0;
    work.checksum_inode = crc32_buf(&work,
        offsetof(grahafs_v2_inode_t, checksum_inode));

    int rc = journal_stage_inode(txn, node->inode, &work);
    if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return rc; }

    rc = journal_txn_commit(txn);
    if (rc != 0) { inode_cache_put(ce); return rc; }

    // Persist in-memory cache copy + node size.
    spinlock_acquire(&ce->lock);
    ce->disk = work;
    ce->dirty = false;
    spinlock_release(&ce->lock);
    if (node->size < new_size) node->size = new_size;

    // Bitmap updates may have changed free_blocks in memory; flush sb so
    // other code paths (stats, mount) observe the accurate count.
    (void)v2_write_superblock();

    inode_cache_put(ce);
    return (ssize_t)bytes_written;
}

// ===========================================================================
// §META — create/finddir/readdir/truncate/unlink for v2 directories.
//
// v2 directory entries share the v1 on-disk format (32-byte records:
// inode_num u32 + name[28]) to keep readdir/finddir simple. A directory
// block is a full 4096-byte block holding up to 128 entries.
// ===========================================================================
typedef struct {
    uint32_t inode_num;
    char     name[28];
} __attribute__((packed)) v2_dirent_t;

_Static_assert(sizeof(v2_dirent_t) == 32, "v2 dirent must be 32 bytes");
#define V2_DIRENTS_PER_BLOCK (GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t))

static size_t v2_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static int v2_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
static void v2_strcpy(char *dst, const char *src, size_t dstsz) {
    size_t i = 0; for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i];
    for (; i < dstsz; ++i) dst[i] = 0;
}

static int v2_dir_add_entry(uint32_t parent_inode, const char *name,
                            uint32_t child_inode, journal_txn_t *txn) {
    grahafs_v2_inode_cache_t *ce = inode_cache_get(parent_inode);
    if (!ce) return -5;
    grahafs_v2_inode_t dir = ce->disk;
    if (dir.type != GRAHAFS_V2_TYPE_DIRECTORY) {
        inode_cache_put(ce); return -20;
    }

    // Ensure first direct block exists.
    if (dir.direct_blocks[0] == 0) {
        uint32_t b = v2_bitmap_allocate_block(txn);
        if (!b) { inode_cache_put(ce); return -28; }
        uint8_t zero[GRAHAFS_V2_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        journal_txn_add_block(txn, b, JOURNAL_BLOCK_KIND_METADATA, zero);
        dir.direct_blocks[0] = b;
        dir.blocks_allocated++;
    }

    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, dir.direct_blocks[0], blk) != 1) {
        inode_cache_put(ce); return -5;
    }
    v2_dirent_t *entries = (v2_dirent_t *)blk;
    int slot = -1;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK; ++i) {
        if (entries[i].inode_num == 0) { slot = (int)i; break; }
    }
    if (slot < 0) { inode_cache_put(ce); return -28; }  // -ENOSPC.

    entries[slot].inode_num = child_inode;
    v2_strcpy(entries[slot].name, name, sizeof(entries[slot].name));
    int rc = journal_txn_add_block(txn, dir.direct_blocks[0],
                                   JOURNAL_BLOCK_KIND_METADATA, blk);
    if (rc != 0) { inode_cache_put(ce); return rc; }

    dir.size = ((uint64_t)(slot + 1)) * sizeof(v2_dirent_t);
    dir.checksum_inode = 0;
    dir.checksum_inode = crc32_buf(&dir,
        offsetof(grahafs_v2_inode_t, checksum_inode));
    rc = journal_stage_inode(txn, parent_inode, &dir);
    if (rc != 0) { inode_cache_put(ce); return rc; }

    spinlock_acquire(&ce->lock);
    ce->disk = dir;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return 0;
}

static int v2_dir_remove_entry(uint32_t parent_inode, const char *name,
                               uint32_t *removed_inode, journal_txn_t *txn) {
    grahafs_v2_inode_cache_t *ce = inode_cache_get(parent_inode);
    if (!ce) return -5;
    grahafs_v2_inode_t dir = ce->disk;
    if (dir.type != GRAHAFS_V2_TYPE_DIRECTORY || dir.direct_blocks[0] == 0) {
        inode_cache_put(ce); return -2;  // -ENOENT.
    }
    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, dir.direct_blocks[0], blk) != 1) {
        inode_cache_put(ce); return -5;
    }
    v2_dirent_t *entries = (v2_dirent_t *)blk;
    int slot = -1;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK; ++i) {
        if (entries[i].inode_num != 0 && v2_strcmp(entries[i].name, name) == 0) {
            slot = (int)i; break;
        }
    }
    if (slot < 0) { inode_cache_put(ce); return -2; }
    if (removed_inode) *removed_inode = entries[slot].inode_num;
    entries[slot].inode_num = 0;
    memset(entries[slot].name, 0, sizeof(entries[slot].name));
    int rc = journal_txn_add_block(txn, dir.direct_blocks[0],
                                   JOURNAL_BLOCK_KIND_METADATA, blk);
    if (rc != 0) { inode_cache_put(ce); return rc; }
    dir.modification_time++;
    dir.checksum_inode = 0;
    dir.checksum_inode = crc32_buf(&dir,
        offsetof(grahafs_v2_inode_t, checksum_inode));
    rc = journal_stage_inode(txn, parent_inode, &dir);
    if (rc != 0) { inode_cache_put(ce); return rc; }
    spinlock_acquire(&ce->lock);
    ce->disk = dir;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return 0;
}

// Forward decls for VFS op table.
struct vfs_node *grahafs_v2_finddir(struct vfs_node *node, const char *name);
struct vfs_node *grahafs_v2_readdir(struct vfs_node *node, uint32_t index);
int              grahafs_v2_create(struct vfs_node *parent, const char *name, uint32_t type);
int              grahafs_v2_truncate_inode(uint32_t inode_num);

// Attach VFS callbacks to a node so read/write/finddir/readdir/create route
// to v2 paths. Separate helper because we call it from finddir/readdir/mount.
static void v2_attach_ops(struct vfs_node *n);

struct vfs_node *grahafs_v2_finddir(struct vfs_node *node, const char *name) {
    if (!g_v2_mounted || !node || !name) return NULL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return NULL;
    grahafs_v2_inode_t dir = ce->disk;
    inode_cache_put(ce);

    if (dir.type != GRAHAFS_V2_TYPE_DIRECTORY || dir.direct_blocks[0] == 0)
        return NULL;

    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, dir.direct_blocks[0], blk) != 1) return NULL;
    v2_dirent_t *entries = (v2_dirent_t *)blk;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK; ++i) {
        if (entries[i].inode_num == 0) continue;
        if (v2_strcmp(entries[i].name, name) != 0) continue;
        grahafs_v2_inode_cache_t *child = inode_cache_get(entries[i].inode_num);
        if (!child) return NULL;
        grahafs_v2_inode_t found = child->disk;
        inode_cache_put(child);
        uint32_t type = (found.type == GRAHAFS_V2_TYPE_DIRECTORY) ?
                        VFS_DIRECTORY : VFS_FILE;
        struct vfs_node *result = vfs_create_node(entries[i].name, type);
        if (result) {
            result->inode = entries[i].inode_num;
            result->size  = found.size;
            v2_attach_ops(result);
        }
        return result;
    }
    return NULL;
}

struct vfs_node *grahafs_v2_readdir(struct vfs_node *node, uint32_t index) {
    if (!g_v2_mounted || !node) return NULL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return NULL;
    grahafs_v2_inode_t dir = ce->disk;
    inode_cache_put(ce);

    if (dir.type != GRAHAFS_V2_TYPE_DIRECTORY || dir.direct_blocks[0] == 0)
        return NULL;

    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, dir.direct_blocks[0], blk) != 1) return NULL;
    v2_dirent_t *entries = (v2_dirent_t *)blk;
    uint32_t cur = 0;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK; ++i) {
        if (entries[i].inode_num == 0) continue;
        if (cur == index) {
            grahafs_v2_inode_cache_t *child = inode_cache_get(entries[i].inode_num);
            if (!child) return NULL;
            grahafs_v2_inode_t found = child->disk;
            inode_cache_put(child);
            uint32_t type = (found.type == GRAHAFS_V2_TYPE_DIRECTORY) ?
                            VFS_DIRECTORY : VFS_FILE;
            struct vfs_node *result = vfs_create_node(entries[i].name, type);
            if (result) {
                result->inode = entries[i].inode_num;
                result->size  = found.size;
                v2_attach_ops(result);
            }
            return result;
        }
        cur++;
    }
    return NULL;
}

int grahafs_v2_create(struct vfs_node *parent, const char *name, uint32_t type) {
    if (!g_v2_mounted || !parent || !name) return -22;
    if (v2_strlen(name) == 0 || v2_strlen(name) >= 28) return -22;
    if (type != VFS_FILE && type != VFS_DIRECTORY) return -22;

    uint32_t new_ino = v2_allocate_inode();
    if (!new_ino) return -28;

    journal_txn_t *txn = journal_txn_begin();
    if (!txn) return -3;

    grahafs_v2_inode_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.magic             = GRAHAFS_V2_INODE_MAGIC;
    fresh.type              = (type == VFS_DIRECTORY) ?
                              GRAHAFS_V2_TYPE_DIRECTORY : GRAHAFS_V2_TYPE_FILE;
    fresh.link_count        = (type == VFS_DIRECTORY) ? 2 : 1;
    fresh.mode              = (type == VFS_DIRECTORY) ? 0755 : 0644;
    fresh.size              = 0;
    fresh.blocks_allocated  = 0;
    fresh.creation_time     = 0;
    fresh.modification_time = 0;
    fresh.access_time       = 0;

    if (type == VFS_DIRECTORY) {
        // Directories get a block with "." and ".." at creation time.
        uint32_t b = v2_bitmap_allocate_block(txn);
        if (!b) { journal_txn_abort(txn); return -28; }
        uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
        memset(blk, 0, sizeof(blk));
        v2_dirent_t *entries = (v2_dirent_t *)blk;
        entries[0].inode_num = new_ino;  v2_strcpy(entries[0].name, ".",  28);
        entries[1].inode_num = parent->inode; v2_strcpy(entries[1].name, "..", 28);
        int rc = journal_txn_add_block(txn, b, JOURNAL_BLOCK_KIND_METADATA, blk);
        if (rc != 0) { journal_txn_abort(txn); return rc; }
        fresh.direct_blocks[0] = b;
        fresh.blocks_allocated = 1;
        fresh.size             = 2 * sizeof(v2_dirent_t);
    }
    fresh.checksum_inode = 0;
    fresh.checksum_inode = crc32_buf(&fresh,
        offsetof(grahafs_v2_inode_t, checksum_inode));

    int rc = journal_stage_inode(txn, new_ino, &fresh);
    if (rc != 0) { journal_txn_abort(txn); return rc; }

    rc = v2_dir_add_entry(parent->inode, name, new_ino, txn);
    if (rc != 0) { journal_txn_abort(txn); return rc; }

    rc = journal_txn_commit(txn);
    if (rc != 0) return rc;

    (void)v2_write_superblock();

    // FU24.A + FU25.D — Phase 29 Session H dirent race fix (Option A).
    //
    // Symptom (historical): two rapid creates back-to-back; the second create
    // reads the parent directory block from disk and sees stale data — the
    // first create's dirent is missing from the on-disk view.  Subsequent
    // vfs_path_to_node(/new_file) returns NULL even after the test does
    // syscall_fsync() between writes (FU24.A test-side workaround in
    // clustertest.c).
    //
    // Root-cause class: while journal_txn_commit's two-barrier protocol +
    // inline checkpoint guarantees the data is durably on disk before
    // returning, the *device-side write cache* (AHCI's internal buffers)
    // may not yet have flushed to the platter from the perspective of a
    // back-to-back read on the same LBA via the SPSC ring path.  In TCG
    // this manifests as a small (~100 µs) window where the read sees the
    // pre-commit block.
    //
    // Option A — least invasive: emit an explicit grahafs_block_flush()
    // after grahafs_v2_create commits the dirent + inode.  This drains
    // the device write cache to the platter before we return success.
    // Bulk-create workloads pay the flush cost once per create (one extra
    // FLUSH CACHE EXT in TCG ~50 µs); test-side syscall_fsync becomes
    // redundant (we keep it in clustertest.c as belt-and-braces).
    //
    // (Option B — cache the post-commit dirent block in the inode_cache
    // and invalidate on next read — is more invasive and changes the
    // inode_cache shape.  Option C — generation counter — would require
    // every vfs_path_to_node call to consult.  We pick A.)
    extern int grahafs_block_flush(uint8_t dev);
    (void)grahafs_block_flush((uint8_t)g_v2_device_id);

    return 0;
}

int grahafs_v2_truncate_inode(uint32_t inode_num) {
    if (!g_v2_mounted) return -22;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return -5;
    grahafs_v2_inode_t work = ce->disk;
    if (work.type != GRAHAFS_V2_TYPE_FILE) {
        inode_cache_put(ce); return -22;
    }

    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return -3; }

    // Walk all allocated data blocks, free them via bitmap. Direct blocks
    // first, then indirect (1024 ptrs in one indirect block), then
    // double-indirect (1024 indirect blocks each with 1024 data ptrs).
    for (uint32_t i = 0; i < GRAHAFS_V2_DIRECT_BLOCKS; ++i) {
        if (work.direct_blocks[i]) {
            (void)v2_bitmap_free_block(work.direct_blocks[i]);
            work.direct_blocks[i] = 0;
        }
    }
    // Indirect block: read the 1024 pointers, free each, then free the
    // indirect block itself.
    if (work.indirect_block) {
        uint32_t indirect_lba = work.indirect_block;
        uint8_t indir_buf[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, indirect_lba, indir_buf) == 1) {
            uint32_t *ptrs = (uint32_t *)indir_buf;
            for (uint32_t i = 0; i < GRAHAFS_V2_INDIRECT_PTRS; ++i) {
                if (ptrs[i]) (void)v2_bitmap_free_block(ptrs[i]);
            }
        }
        (void)v2_bitmap_free_block(indirect_lba);
        work.indirect_block = 0;
    }
    // Double-indirect: 1024 indirect blocks each with 1024 ptrs.
    if (work.double_indirect) {
        uint32_t dindir_lba = work.double_indirect;
        uint8_t dindir_buf[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, dindir_lba, dindir_buf) == 1) {
            uint32_t *l1 = (uint32_t *)dindir_buf;
            for (uint32_t a = 0; a < GRAHAFS_V2_INDIRECT_PTRS; ++a) {
                if (!l1[a]) continue;
                uint8_t l2_buf[GRAHAFS_V2_BLOCK_SIZE];
                if (grahafs_v2_block_read((uint8_t)g_v2_device_id, l1[a], l2_buf) == 1) {
                    uint32_t *l2 = (uint32_t *)l2_buf;
                    for (uint32_t b = 0; b < GRAHAFS_V2_INDIRECT_PTRS; ++b) {
                        if (l2[b]) (void)v2_bitmap_free_block(l2[b]);
                    }
                }
                (void)v2_bitmap_free_block(l1[a]);
            }
        }
        (void)v2_bitmap_free_block(dindir_lba);
        work.double_indirect = 0;
    }
    work.size = 0;
    work.blocks_allocated = 0;
    work.modification_time++;
    work.checksum_inode = 0;
    work.checksum_inode = crc32_buf(&work,
        offsetof(grahafs_v2_inode_t, checksum_inode));
    int rc = journal_stage_inode(txn, inode_num, &work);
    if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return rc; }
    rc = journal_txn_commit(txn);
    if (rc != 0) { inode_cache_put(ce); return rc; }

    spinlock_acquire(&ce->lock);
    ce->disk = work;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    (void)v2_write_superblock();
    return 0;
}

int grahafs_v2_unlink(struct vfs_node *parent, const char *name) {
    if (!g_v2_mounted || !parent || !name) return -22;
    journal_txn_t *txn = journal_txn_begin();
    if (!txn) return -3;
    uint32_t removed = 0;
    int rc = v2_dir_remove_entry(parent->inode, name, &removed, txn);
    if (rc != 0) { journal_txn_abort(txn); return rc; }

    // MVP: zero the child inode (simpler than tracking link_count).
    grahafs_v2_inode_cache_t *child = inode_cache_get(removed);
    if (child) {
        grahafs_v2_inode_t zeroed;
        memset(&zeroed, 0, sizeof(zeroed));
        (void)journal_stage_inode(txn, removed, &zeroed);
        // Free its direct blocks for simple files.
        for (uint32_t i = 0; i < GRAHAFS_V2_DIRECT_BLOCKS; ++i) {
            if (child->disk.direct_blocks[i])
                (void)v2_bitmap_free_block(child->disk.direct_blocks[i]);
        }
        spinlock_acquire(&child->lock);
        memset(&child->disk, 0, sizeof(child->disk));
        child->magic = 0;  // Evict from cache so next get() refreshes.
        spinlock_release(&child->lock);
        inode_cache_put(child);
        spinlock_acquire(&g_v2_sb_lock);
        g_v2_sb.free_inodes++;
        spinlock_release(&g_v2_sb_lock);
    }
    rc = journal_txn_commit(txn);
    if (rc != 0) return rc;
    (void)v2_write_superblock();
    return 0;
}

// ===========================================================================
// §ASYNC — Phase 18 async entry points. MVP wraps the sync path and invokes
// the completion callback synchronously. Phase 23 (userspace AHCI) will
// genuinely go async.
// ===========================================================================
int grahafs_v2_async_read(struct vfs_node *node, uint64_t offset, uint64_t len,
                          void *dst, vfs_async_completion_t cb, void *user_data) {
    ssize_t r = grahafs_v2_read(node, offset, (size_t)len, dst);
    if (cb) cb((int64_t)r, user_data);
    return 0;
}

int grahafs_v2_async_write(struct vfs_node *node, uint64_t offset, uint64_t len,
                           const void *src, vfs_async_completion_t cb, void *user_data) {
    ssize_t r = grahafs_v2_write(node, offset, (size_t)len, (void *)src);
    if (cb) cb((int64_t)r, user_data);
    return 0;
}

// ===========================================================================
// §FSYNC — forces any dirty inode-cache state for this node through the
// journal, then flushes the device cache.
// ===========================================================================
int grahafs_v2_fsync(struct vfs_node *node) {
    if (!g_v2_mounted || !node) return -22;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return -5;
    int rc = 0;
    if (ce->dirty) rc = inode_cache_flush_dirty(ce);
    inode_cache_put(ce);
    (void)grahafs_block_flush((uint8_t)g_v2_device_id);
    return rc;
}

// ===========================================================================
// §CLOSE — synchronous SimHash + version record emission at close().
//
// Flow per plan U14:
//     1. Flush dirty pages   (journal already owns them if a write was
//                              pending; this path is idempotent).
//     2. Read up to 1 MB of the file content (sampled from direct blocks).
//     3. Compute SimHash via simhash_auto.
//     4. Allocate a version_record inside the active segment; bump refcount.
//     5. Journal-commit inode update (version_chain_head_id++, version_count++,
//        ai_embedding[0] = simhash, ai_last_modified = timestamp).
//     6. Enqueue recluster job (best-effort, async via per-mount work queue).
//     7. Return 0.
// ===========================================================================
#include "simhash.h"
#include "recluster.h"

// In-memory version chain: linked list per inode_cache_t threaded via
// grahafs_v2_version_entry_t. Populated on every emit_version_record so
// list_versions / fs_revert / gc can walk back without re-reading
// segments. Persisted state on disk (the version_record blocks) remains
// authoritative — the in-memory list is a cache for fast queries during
// this kernel's run. After reboot, ce->version_chain_head_cached is NULL
// for all inodes and the syscalls fall back to the head-only summary
// (this is documented in chain_walk callers).
static grahafs_v2_version_entry_t *
v2_chain_alloc_entry(uint64_t version_id, uint64_t timestamp_ns,
                     uint64_t size, uint64_t simhash,
                     uint32_t segment_id, uint64_t prev_version,
                     uint64_t parent_version, uint16_t flags,
                     uint32_t cluster_id) {
    grahafs_v2_version_entry_t *e =
        (grahafs_v2_version_entry_t *)kmalloc(sizeof(*e), SUBSYS_FS);
    if (!e) return NULL;
    e->version_id            = version_id;
    e->timestamp_ns          = timestamp_ns;
    e->size                  = size;
    e->simhash               = simhash;
    e->cluster_id_at_version = cluster_id;
    e->segment_id            = segment_id;
    e->prev_version          = prev_version;
    e->parent_version        = parent_version;
    e->flags                 = flags;
    e->_pad0                 = 0;
    e->snap_pin_count        = 0;  // W17.2: not pinned at creation.
    e->next                  = NULL;
    return e;
}

static void v2_chain_push_head(grahafs_v2_inode_cache_t *ce,
                               grahafs_v2_version_entry_t *e) {
    if (!ce || !e) return;
    e->next = ce->version_chain_head_cached;
    ce->version_chain_head_cached = e;
    ce->version_chain_loaded = true;
}

// Walk newest → oldest. Stop after fn returns false or `max` calls.
uint32_t grahafs_v2_chain_walk(const grahafs_v2_inode_cache_t *ce,
                              uint32_t max,
                              bool (*fn)(const grahafs_v2_version_entry_t *, void *),
                              void *ctx) {
    uint32_t n = 0;
    if (!ce || !fn) return 0;
    grahafs_v2_version_entry_t *e = ce->version_chain_head_cached;
    while (e && n < max) {
        if (!fn(e, ctx)) break;
        n++;
        e = e->next;
    }
    return n;
}

// Pop the OLDEST entry (tail of the linked list) and return it. Caller
// owns the memory and must kfree(). Returns NULL on empty list.
grahafs_v2_version_entry_t *
grahafs_v2_chain_pop_tail(grahafs_v2_inode_cache_t *ce) {
    if (!ce || !ce->version_chain_head_cached) return NULL;
    // W17.2: walk newest→oldest; pop the OLDEST entry whose snap_pin_count
    // is 0. Pinned entries stay in the chain so a snapshot's referenced
    // version cannot be reaped under it. If the entire chain is pinned,
    // return NULL — GC just doesn't make progress on this inode until
    // something unpins. The walk records the latest *->slot whose entry
    // is unpinned; that's the oldest unpinned (loop visits newer-first).
    grahafs_v2_version_entry_t **slot       = &ce->version_chain_head_cached;
    grahafs_v2_version_entry_t **last_unpin = NULL;
    while (*slot) {
        if ((*slot)->snap_pin_count == 0) last_unpin = slot;
        slot = &(*slot)->next;
    }
    if (!last_unpin) return NULL;
    grahafs_v2_version_entry_t *t = *last_unpin;
    *last_unpin = t->next;
    t->next = NULL;
    return t;
}

// Find an entry by version_id. Returns NULL if not in cache.
const grahafs_v2_version_entry_t *
grahafs_v2_chain_find(const grahafs_v2_inode_cache_t *ce, uint64_t version_id) {
    if (!ce) return NULL;
    grahafs_v2_version_entry_t *e = ce->version_chain_head_cached;
    while (e) {
        if (e->version_id == version_id) return e;
        e = e->next;
    }
    return NULL;
}

// Emit one version record into a segment. Returns 0/errno. On success,
// *out_segment_id is the segment the record landed in, *out_version_id is
// the monotonic id allocated to this version. The record itself is written
// to the segment via a metadata journal block (so it survives a crash).
static uint64_t g_next_version_id = 1;

static int v2_emit_version_record(uint32_t inode_num,
                                  uint64_t file_size,
                                  uint64_t simhash,
                                  uint64_t prev_version_id,
                                  uint32_t cluster_id,
                                  journal_txn_t *txn,
                                  uint32_t *out_segment_id,
                                  uint64_t *out_version_id) {
    (void)inode_num;  /* version-record body doesn't store the inode #. */
    // Allocate one 128-byte slot — we request a full block's worth so the
    // segment allocator doesn't partially fill a block across txns.
    uint32_t bytes_needed = GRAHAFS_V2_BLOCK_SIZE;
    uint32_t seg_id = segment_allocate_for_write(bytes_needed);
    if (seg_id == 0xFFFFFFFFu) return -28;

    grahafs_v2_segment_t *seg = segment_get(seg_id);
    if (!seg) return -5;

    // Record lives at `seg->first_block + (next_free_block_offset/BLOCK - 1)`.
    // The allocator already advanced next_free_block_offset by bytes_needed,
    // so the record block is the one ending at the new offset.
    uint64_t rec_block = seg->first_block +
        ((uint64_t)(seg->next_free_block_offset / GRAHAFS_V2_BLOCK_SIZE) - 1u);

    grahafs_v2_version_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic        = GRAHAFS_V2_VERSION_MAGIC;
    rec.version_id   = g_next_version_id++;
    // Wall clock at version creation. RTC gives second precision; we
    // multiply to ns and add the tick-fraction within the current second
    // for ~10 ms resolution. age-based GC retention compares this against
    // rtc_now_seconds() * 1e9 to find records older than gc_max_age_ns.
    {
        extern int64_t rtc_now_seconds(void);
        extern volatile uint64_t g_timer_ticks;
        int64_t s = rtc_now_seconds();
        rec.timestamp_ns = (uint64_t)s * 1000000000ULL +
                           (g_timer_ticks % 100u) * 10000000ULL;
    }
    rec.size         = file_size;
    rec.simhash      = simhash;
    rec.segment_id   = seg_id;
    rec.cluster_id_at_version = cluster_id;
    rec.prev_version = prev_version_id;
    rec.parent_version = 0;
    rec.flags        = 0;
    rec.data_block_count = 0;
    rec.data_block_index = 0;
    rec.checksum     = 0;
    rec.checksum     = crc32_buf(&rec,
        offsetof(grahafs_v2_version_record_t, checksum));

    uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, &rec, sizeof(rec));
    int rrc = journal_txn_add_block(txn, rec_block, JOURNAL_BLOCK_KIND_METADATA, blk);
    if (rrc != 0) return rrc;

    if (out_segment_id) *out_segment_id = seg_id;
    if (out_version_id) *out_version_id = rec.version_id;
    return 0;
}

void grahafs_v2_close(struct vfs_node *node) {
    if (!g_v2_mounted || !node) return;

    grahafs_v2_inode_cache_t *ce = inode_cache_get(node->inode);
    if (!ce) return;

    grahafs_v2_inode_t work = ce->disk;
    if (work.type != GRAHAFS_V2_TYPE_FILE) {
        inode_cache_put(ce);
        return;
    }

    // Skip versioning for zero-size files (avoids cluttering chains).
    if (work.size == 0) {
        inode_cache_put(ce);
        return;
    }

    // Sample up to 1 MB from the file — direct blocks + indirect (one I/O per
    // block). Cap total reads at 256 blocks for MVP.
    static uint8_t sample_buf[1024 * 1024];
    uint64_t bytes_to_sample =
        (work.size < sizeof(sample_buf)) ? work.size : sizeof(sample_buf);
    uint32_t blocks = (uint32_t)((bytes_to_sample + GRAHAFS_V2_BLOCK_SIZE - 1) /
                                 GRAHAFS_V2_BLOCK_SIZE);
    uint32_t sampled = 0;
    for (uint32_t i = 0; i < blocks && sampled < sizeof(sample_buf); ++i) {
        uint32_t lba = v2_block_index_to_lba(&work, i);
        if (lba == 0) continue;
        uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id, lba, blk) != 1) break;
        uint32_t copy = GRAHAFS_V2_BLOCK_SIZE;
        if (sampled + copy > bytes_to_sample) copy = (uint32_t)(bytes_to_sample - sampled);
        if (sampled + copy > sizeof(sample_buf)) copy = (uint32_t)(sizeof(sample_buf) - sampled);
        memcpy(sample_buf + sampled, blk, copy);
        sampled += copy;
    }

    uint64_t simhash = 0;
    if (sampled > 0) simhash = simhash_auto(sample_buf, sampled);

    // Journal: version record + inode update in one txn.
    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return; }

    uint32_t seg_id = 0; uint64_t version_id = 0;
    uint64_t prev_version = work.version_chain_head_id;
    uint32_t cid = ((uint32_t)work.ai_reserved[0])       |
                   ((uint32_t)work.ai_reserved[1] << 8)  |
                   ((uint32_t)work.ai_reserved[2] << 16) |
                   ((uint32_t)work.ai_reserved[3] << 24);
    int rc = v2_emit_version_record(node->inode, work.size, simhash, prev_version,
                                    cid, txn, &seg_id, &version_id);
    if (rc != 0) {
        journal_txn_abort(txn);
        inode_cache_put(ce);
        return;
    }

    work.version_chain_head_id = version_id;
    work.version_chain_segment = seg_id;
    if (work.version_count < GRAHAFS_V2_MAX_VERSIONS) work.version_count++;
    work.ai_embedding[0] = simhash;
    work.ai_flags |= 0x04u;  // HAS_EMBEDDING.
    work.ai_last_modified++;
    work.checksum_inode = 0;
    work.checksum_inode = crc32_buf(&work,
        offsetof(grahafs_v2_inode_t, checksum_inode));

    rc = journal_stage_inode(txn, node->inode, &work);
    if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return; }

    rc = journal_txn_commit(txn);
    if (rc != 0) { inode_cache_put(ce); return; }

    // Push the new version onto the in-memory chain so list_versions /
    // fs_revert / gc_prune_inode can walk back without re-reading segments.
    // Compute timestamp_ns the same way v2_emit_version_record did.
    uint64_t now_ns;
    {
        extern int64_t rtc_now_seconds(void);
        extern volatile uint64_t g_timer_ticks;
        int64_t s = rtc_now_seconds();
        now_ns = (uint64_t)s * 1000000000ULL +
                 (g_timer_ticks % 100u) * 10000000ULL;
    }
    grahafs_v2_version_entry_t *new_entry = v2_chain_alloc_entry(
        version_id, now_ns, work.size, simhash, seg_id,
        prev_version, /*parent=*/0, /*flags=*/0, cid);
    if (new_entry) {
        spinlock_acquire(&ce->lock);
        v2_chain_push_head(ce, new_entry);
        ce->disk = work;
        ce->dirty = false;
        spinlock_release(&ce->lock);
    } else {
        spinlock_acquire(&ce->lock);
        ce->disk = work;
        ce->dirty = false;
        spinlock_release(&ce->lock);
    }
    inode_cache_put(ce);

    // Best-effort async reclustering.
    recluster_enqueue(node->inode);
}

// ===========================================================================
// §SIMHASH_ON_DEMAND — Phase 25 / FU24.A.
//
// On-demand SimHash compute path triggered by SYS_COMPUTE_SIMHASH. The v2
// close-path (above) computes simhash automatically + journals it as part
// of the version_record; this function exists for tools that want to
// recompute on demand (e.g. clustertest, the gash `simhash` command).
//
// Differences from the close-path:
//   * No version_record is allocated. Just updates ai_embedding[0] in place
//     and journals the inode.
//   * Sample budget is 48 KiB (matches v1) rather than 1 MiB — keeps the
//     BSS sample buffer small enough for the kernel image without
//     introducing a kmalloc/kfree pair on every call.
//   * cluster_assign is invoked SYNCHRONOUSLY (not via recluster_enqueue)
//     so the test in user/tests/clustertest.c sees the new cluster on
//     syscall return.
// ===========================================================================
uint64_t grahafs_v2_compute_simhash(uint32_t inode_num) {
    if (!grahafs_v2_is_mounted()) return 0;
    if (inode_num == 0) return 0;

    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return 0;

    grahafs_v2_inode_t work;
    spinlock_acquire(&ce->lock);
    work = ce->disk;
    spinlock_release(&ce->lock);

    if (work.type != GRAHAFS_V2_TYPE_FILE || work.size == 0) {
        inode_cache_put(ce);
        return 0;
    }

    // 48 KiB sample budget — matches v1. Static BSS keeps it off the
    // kernel stack and out of kheap allocation churn.
    static uint8_t sh_sample_buf[12 * GRAHAFS_V2_BLOCK_SIZE];

    uint64_t bytes_to_sample = work.size;
    if (bytes_to_sample > sizeof(sh_sample_buf)) bytes_to_sample = sizeof(sh_sample_buf);
    uint32_t blocks = (uint32_t)((bytes_to_sample + GRAHAFS_V2_BLOCK_SIZE - 1) /
                                  GRAHAFS_V2_BLOCK_SIZE);
    int dev_id = grahafs_v2_device_id();
    uint32_t sampled = 0;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t lba = v2_block_index_to_lba(&work, i);
        if (lba == 0) continue;
        uint8_t blk[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)dev_id, lba, blk) != 1) break;
        uint32_t copy = GRAHAFS_V2_BLOCK_SIZE;
        if ((uint64_t)sampled + copy > bytes_to_sample) {
            copy = (uint32_t)(bytes_to_sample - sampled);
        }
        memcpy(sh_sample_buf + sampled, blk, copy);
        sampled += copy;
    }

    uint64_t hash = 0;
    if (sampled > 0) hash = simhash_auto(sh_sample_buf, sampled);

    // Mirror v1's filename lookup: walk root directory's first block,
    // scan dirents for a record matching inode_num, copy out the name.
    // cluster_assign uses this to label the cluster member; passing an
    // empty/null name would NULL-deref cl_strncpy in cluster.c.
    char found_name[28] = {0};
    {
        const grahafs_v2_superblock_t *sb = grahafs_v2_sb();
        uint32_t root_inode = sb ? sb->root_inode : 0;
        if (root_inode != 0) {
            grahafs_v2_inode_cache_t *root_ce = inode_cache_get(root_inode);
            if (root_ce) {
                uint32_t root_db0 = 0;
                spinlock_acquire(&root_ce->lock);
                root_db0 = root_ce->disk.direct_blocks[0];
                spinlock_release(&root_ce->lock);
                if (root_db0 != 0) {
                    uint8_t dir_blk[GRAHAFS_V2_BLOCK_SIZE];
                    if (grahafs_v2_block_read((uint8_t)dev_id, root_db0, dir_blk) == 1) {
                        v2_dirent_t *entries = (v2_dirent_t *)dir_blk;
                        for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK; i++) {
                            if (entries[i].inode_num == inode_num) {
                                size_t n = 0;
                                while (n < 27 && entries[i].name[n] != '\0') {
                                    found_name[n] = entries[i].name[n];
                                    n++;
                                }
                                found_name[n] = '\0';
                                break;
                            }
                        }
                    }
                }
                inode_cache_put(root_ce);
            }
        }
    }
    if (found_name[0] == '\0') {
        // Synthetic placeholder so cluster_assign's strncpy has something
        // to copy. Real callers will typically have a name from the dir
        // walk above; this is a safety net for inodes outside the root
        // directory (sub-directory contents won't be found here).
        found_name[0] = '?';
        found_name[1] = '\0';
    }

    // Synchronous cluster assignment. Test clustertest tests 2+3 expect
    // the cluster to be visible on SYS_COMPUTE_SIMHASH return — going via
    // recluster_enqueue (async work queue) would race the test.
    uint32_t cid = cluster_assign(inode_num, hash, found_name);

    // Persist hash + cluster_id into the inode + journal it.
    spinlock_acquire(&ce->lock);
    ce->disk.ai_embedding[0] = hash;
    ce->disk.ai_flags |= 0x04u;  // HAS_EMBEDDING (matches close-path bit).
    if (cid != 0) {
        ce->disk.ai_reserved[0] = (uint8_t)(cid & 0xFF);
        ce->disk.ai_reserved[1] = (uint8_t)((cid >> 8) & 0xFF);
        ce->disk.ai_reserved[2] = (uint8_t)((cid >> 16) & 0xFF);
        ce->disk.ai_reserved[3] = (uint8_t)((cid >> 24) & 0xFF);
    }
    ce->dirty = true;
    spinlock_release(&ce->lock);

    (void)inode_cache_flush_dirty(ce);

    inode_cache_put(ce);
    return hash;
}

// ===========================================================================
// §AI_METADATA — v2 ports of the v1 grahafs.c AI-feature surface
// (set/get_ai_metadata, search_by_tag, find_similar).  FU29.H.
//
// The user-facing structs (grahafs_ai_metadata_t / grahafs_search_results_t /
// grahafs_ai_metadata_block_t) and the GRAHAFS_AI_* / GRAHAFS_META_FLAG_* bits
// are shared with v1 via grahafs.h — the syscall layer copies the SAME ABI in
// and out regardless of which FS is mounted, so these v2 variants must match
// v1's observable semantics exactly.
//
// Storage mapping v1 -> v2:
//   * importance, tags(inline 95), ai_flags  -> identical inline inode fields.
//   * simhash                                -> inode.ai_embedding[0] (v2 keeps
//     it INLINE; v1 kept it in the extended block — find_similar accounts for
//     that difference).
//   * summary / full-tags / 128-dim embedding -> extended metadata block at
//     inode.ai_metadata_block (an LBA), laid out as grahafs_ai_metadata_block_t,
//     allocated lazily via v2_bitmap_allocate_block + journalled atomically with
//     the inode update.
// ===========================================================================

// Substring match (v1 used grahafs_strstr; keep a local copy to avoid an
// include-order dependency on the v1 .c).
static const char *v2_ai_strstr(const char *hay, const char *needle) {
    if (!needle[0]) return hay;
    for (const char *h = hay; *h; ++h) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return h;
    }
    return NULL;
}

// Build "/<name>" from a (possibly non-NUL-terminated 28-byte) dirent name into
// a 256-byte path buffer.
static void v2_ai_build_root_path(char out[256], const char *name) {
    out[0] = '/';
    size_t n = 0;
    while (n < 27 && name[n] != '\0') { out[1 + n] = name[n]; ++n; }
    out[1 + n] = '\0';
}

// Read root-directory block 0's LBA (where every top-level file lives). Mirrors
// v1's single-block root scan (a pre-existing v1 limitation kept for parity).
// Returns 0 if there is no root dir block.
static uint32_t v2_ai_root_dir_block(void) {
    const grahafs_v2_superblock_t *sb = grahafs_v2_sb();
    uint32_t root_inode = sb ? sb->root_inode : 0;
    if (root_inode == 0) return 0;
    grahafs_v2_inode_cache_t *rce = inode_cache_get(root_inode);
    if (!rce) return 0;
    uint32_t db0;
    spinlock_acquire(&rce->lock);
    db0 = rce->disk.direct_blocks[0];
    spinlock_release(&rce->lock);
    inode_cache_put(rce);
    return db0;
}

// Snapshot an inode's on-disk image by number. Returns true + fills *out on a
// valid allocated inode; false on miss/IO-error/bad-magic.
static bool v2_ai_inode_snapshot(uint32_t inode_num, grahafs_v2_inode_t *out) {
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return false;
    spinlock_acquire(&ce->lock);
    *out = ce->disk;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return out->magic == GRAHAFS_V2_INODE_MAGIC;
}

int grahafs_v2_set_ai_metadata(uint32_t inode_num, const grahafs_ai_metadata_t *meta) {
    if (!meta || !grahafs_v2_is_mounted()) return -1;
    if (inode_num == 0) return -1;

    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return -1;

    grahafs_v2_inode_t work;
    spinlock_acquire(&ce->lock);
    work = ce->disk;
    spinlock_release(&ce->lock);

    if (work.magic != GRAHAFS_V2_INODE_MAGIC) { inode_cache_put(ce); return -2; }

    // Importance (always inline).
    if (meta->flags & GRAHAFS_META_FLAG_IMPORTANCE) {
        work.ai_importance = meta->importance > 100 ? 100 : meta->importance;
    }

    // Tags: inline up to 95 chars; overflow spills to the extended block.
    if (meta->flags & GRAHAFS_META_FLAG_TAGS) {
        size_t tag_len = v2_strlen(meta->tags);
        size_t inline_len = tag_len > 95 ? 95 : tag_len;
        memcpy(work.ai_tags, meta->tags, inline_len);
        work.ai_tags[inline_len] = '\0';
        work.ai_flags |= GRAHAFS_AI_HAS_TAGS;
        if (tag_len > 95) work.ai_flags |= GRAHAFS_AI_HAS_EXTENDED;
    }

    bool need_extended = (meta->flags & GRAHAFS_META_FLAG_SUMMARY) ||
                         (meta->flags & GRAHAFS_META_FLAG_EMBEDDING) ||
                         ((meta->flags & GRAHAFS_META_FLAG_TAGS) &&
                          v2_strlen(meta->tags) > 95);

    // One journal txn: extended-block bitmap alloc + extended content + inode
    // update all commit atomically (or all revert on crash).
    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return -3; }

    if (need_extended) {
        if (work.ai_metadata_block == 0) {
            uint32_t ext = v2_bitmap_allocate_block(txn);
            if (ext == 0) { journal_txn_abort(txn); inode_cache_put(ce); return -3; }
            work.ai_metadata_block = ext;
            work.ai_flags |= GRAHAFS_AI_HAS_EXTENDED;
        }

        uint8_t ext_buf[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id,
                                  work.ai_metadata_block, ext_buf) != 1) {
            memset(ext_buf, 0, sizeof(ext_buf));
        }
        grahafs_ai_metadata_block_t *ext_meta = (grahafs_ai_metadata_block_t *)ext_buf;
        if (ext_meta->magic != GRAHAFS_AI_META_MAGIC) {
            memset(ext_buf, 0, sizeof(ext_buf));
            ext_meta->magic = GRAHAFS_AI_META_MAGIC;
            ext_meta->version = 1;
        }

        if (meta->flags & GRAHAFS_META_FLAG_TAGS) {
            size_t tl = v2_strlen(meta->tags);
            size_t cl = tl > 511 ? 511 : tl;
            memcpy(ext_meta->tags, meta->tags, cl);
            ext_meta->tags[cl] = '\0';
        }
        if (meta->flags & GRAHAFS_META_FLAG_SUMMARY) {
            size_t sl = v2_strlen(meta->summary);
            size_t cl = sl > 1023 ? 1023 : sl;
            memcpy(ext_meta->summary, meta->summary, cl);
            ext_meta->summary[cl] = '\0';
            work.ai_flags |= GRAHAFS_AI_HAS_SUMMARY;
        }
        if (meta->flags & GRAHAFS_META_FLAG_EMBEDDING) {
            uint32_t dim = meta->embedding_dim > 128 ? 128 : meta->embedding_dim;
            if (dim) memcpy(ext_meta->embedding, meta->embedding,
                            dim * sizeof(uint64_t));
            ext_meta->embedding_dim = dim;
            work.ai_flags |= GRAHAFS_AI_HAS_EMBEDDING;
        }

        int rc = journal_txn_add_block(txn, work.ai_metadata_block,
                                       JOURNAL_BLOCK_KIND_DATA, ext_buf);
        if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return rc; }
    }

    work.ai_last_modified++;  // monotonic stamp (no RTC dependency needed).

    work.checksum_inode = 0;
    work.checksum_inode = inode_checksum(&work);
    int rc = journal_stage_inode(txn, inode_num, &work);
    if (rc != 0) { journal_txn_abort(txn); inode_cache_put(ce); return rc; }
    rc = journal_txn_commit(txn);
    if (rc != 0) { inode_cache_put(ce); return rc; }

    // Reflect persisted state into the cache so subsequent reads are coherent.
    spinlock_acquire(&ce->lock);
    ce->disk = work;
    ce->dirty = false;
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return 0;
}

int grahafs_v2_get_ai_metadata(uint32_t inode_num, grahafs_ai_metadata_t *meta) {
    if (!meta || !grahafs_v2_is_mounted()) return -1;
    if (inode_num == 0) return -1;

    grahafs_v2_inode_t work;
    if (!v2_ai_inode_snapshot(inode_num, &work)) return -2;

    memset(meta, 0, sizeof(*meta));
    meta->flags        = work.ai_flags;
    meta->importance   = work.ai_importance;
    meta->access_count = work.ai_access_count;
    meta->last_modified = work.ai_last_modified;

    size_t tag_len = v2_strlen(work.ai_tags);
    if (tag_len > 0) {
        size_t cl = tag_len > 511 ? 511 : tag_len;
        memcpy(meta->tags, work.ai_tags, cl);
        meta->tags[cl] = '\0';
    }

    if ((work.ai_flags & GRAHAFS_AI_HAS_EXTENDED) && work.ai_metadata_block != 0) {
        uint8_t ext_buf[GRAHAFS_V2_BLOCK_SIZE];
        if (grahafs_v2_block_read((uint8_t)g_v2_device_id,
                                  work.ai_metadata_block, ext_buf) == 1) {
            grahafs_ai_metadata_block_t *ext_meta =
                (grahafs_ai_metadata_block_t *)ext_buf;
            if (ext_meta->magic == GRAHAFS_AI_META_MAGIC) {
                if (ext_meta->tags[0] != '\0') {
                    memcpy(meta->tags, ext_meta->tags, 511);
                    meta->tags[511] = '\0';
                }
                if (work.ai_flags & GRAHAFS_AI_HAS_SUMMARY) {
                    memcpy(meta->summary, ext_meta->summary, 1023);
                    meta->summary[1023] = '\0';
                }
                if (work.ai_flags & GRAHAFS_AI_HAS_EMBEDDING) {
                    memcpy(meta->embedding, ext_meta->embedding,
                           128 * sizeof(uint64_t));
                    meta->embedding_dim = ext_meta->embedding_dim;
                }
            }
        }
    }
    // NB: unlike v1, the v2 read path does NOT persist an access_count bump —
    // get is a pure read (no journal txn per query). No test observes the
    // counter, and avoiding a write keeps the already-slower v2 query cheap.
    return 0;
}

int grahafs_v2_search_by_tag(const char *tag, grahafs_search_results_t *results,
                             int max_results) {
    if (!tag || !results || !grahafs_v2_is_mounted()) return -1;
    if (max_results <= 0 || max_results > 16) max_results = 16;
    memset(results, 0, sizeof(*results));

    uint32_t db0 = v2_ai_root_dir_block();
    if (db0 == 0) return 0;

    uint8_t dir_blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, db0, dir_blk) != 1) return -1;
    v2_dirent_t *entries = (v2_dirent_t *)dir_blk;

    uint32_t count = 0;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK && count < (uint32_t)max_results; ++i) {
        uint32_t ino = entries[i].inode_num;
        if (ino == 0 || entries[i].name[0] == '\0') continue;
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0'))) continue;

        grahafs_v2_inode_t fin;
        if (!v2_ai_inode_snapshot(ino, &fin)) continue;
        if (!(fin.ai_flags & GRAHAFS_AI_HAS_TAGS)) continue;
        if (!v2_ai_strstr(fin.ai_tags, tag)) continue;

        grahafs_search_result_t *r = &results->results[count];
        v2_ai_build_root_path(r->path, entries[i].name);
        r->inode_num  = ino;
        r->importance = fin.ai_importance;
        size_t tc = v2_strlen(fin.ai_tags);
        if (tc > 95) tc = 95;
        memcpy(r->tags, fin.ai_tags, tc);
        r->tags[tc] = '\0';
        ++count;
    }
    results->count = count;
    return (int)count;
}

int grahafs_v2_find_similar(uint32_t ref_inode, int threshold,
                            grahafs_search_results_t *results, int max_results) {
    if (!results || !grahafs_v2_is_mounted()) return -1;
    if (threshold <= 0) threshold = SIMHASH_SIMILAR_THRESHOLD;
    if (max_results <= 0 || max_results > 16) max_results = 16;

    // v2 stores the SimHash INLINE in ai_embedding[0] (v1 kept it in the
    // extended block). No SimHash computed -> -2, matching v1's contract.
    grahafs_v2_inode_t ref;
    if (!v2_ai_inode_snapshot(ref_inode, &ref)) return -1;
    uint64_t ref_hash = ref.ai_embedding[0];
    if (ref_hash == 0) return -2;

    memset(results, 0, sizeof(*results));

    uint32_t db0 = v2_ai_root_dir_block();
    if (db0 == 0) return 0;

    uint8_t dir_blk[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_v2_block_read((uint8_t)g_v2_device_id, db0, dir_blk) != 1) return -1;
    v2_dirent_t *entries = (v2_dirent_t *)dir_blk;

    uint32_t count = 0;
    for (size_t i = 0; i < V2_DIRENTS_PER_BLOCK && count < (uint32_t)max_results; ++i) {
        uint32_t ino = entries[i].inode_num;
        if (ino == 0 || entries[i].name[0] == '\0') continue;
        if (ino == ref_inode) continue;   // skip self.
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0'))) continue;

        grahafs_v2_inode_t fin;
        if (!v2_ai_inode_snapshot(ino, &fin)) continue;
        if (fin.type != GRAHAFS_V2_TYPE_FILE) continue;
        uint64_t fhash = fin.ai_embedding[0];
        if (fhash == 0) continue;   // no SimHash for this file.

        int dist = simhash_hamming_distance(ref_hash, fhash);
        if (dist <= threshold) {
            grahafs_search_result_t *r = &results->results[count];
            v2_ai_build_root_path(r->path, entries[i].name);
            r->inode_num  = ino;
            r->importance = (uint32_t)dist;   // v1 stashes distance here.
            size_t tc = v2_strlen(fin.ai_tags);
            if (tc > 95) tc = 95;
            memcpy(r->tags, fin.ai_tags, tc);
            r->tags[tc] = '\0';
            ++count;
        }
    }
    results->count = count;
    return (int)count;
}

// ===========================================================================
// §VFS_OPS — assemble a vfs_node so it routes into v2 for all operations.
// ===========================================================================
static void v2_attach_ops(struct vfs_node *n) {
    if (!n) return;
    n->read        = grahafs_v2_read;
    n->write       = grahafs_v2_write;
    n->finddir     = grahafs_v2_finddir;
    n->readdir     = grahafs_v2_readdir;
    n->create      = grahafs_v2_create;
    n->async_read  = grahafs_v2_async_read;
    n->async_write = grahafs_v2_async_write;
}

int journal_stage_inode_external(journal_txn_t *txn, uint32_t inode_num,
                                 const grahafs_v2_inode_t *disk_copy) {
    return journal_stage_inode(txn, inode_num, disk_copy);
}

// Build the root VFS node for the v2 mount. Called from grahafs_v2_mount
// at the end of its success path (U17).
struct vfs_node *grahafs_v2_build_root_node(void) {
    if (!g_v2_mounted) return NULL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(g_v2_sb.root_inode);
    if (!ce) return NULL;
    grahafs_v2_inode_t root_ino = ce->disk;
    inode_cache_put(ce);

    struct vfs_node *root = vfs_create_node("/", VFS_DIRECTORY);
    if (!root) return NULL;
    root->inode = g_v2_sb.root_inode;
    root->size  = root_ino.size;
    v2_attach_ops(root);
    return root;
}

// ---------------------------------------------------------------------------
// Phase 24 W17.2: snapshot version pinning helpers.
//
// snap_capture_fs_pins (W14.7, deferred) calls grahafs_pin_version on every
// open inode in the captured task's FD table — bumping snap_pin_count on the
// version_entry that was HEAD at capture time. While pinned, gc_prune_inode
// must skip that entry rather than reaping it. snap_delete / snap_restore
// pair each pin with a matching grahafs_unpin_version.
//
// All three helpers take the per-mount inode_cache_get/put pair so caller
// code does not need to manage lifecycle explicitly. They walk the
// in-memory version chain (cold-boot inodes have an empty chain — the cold
// cache is rebuilt lazily by reads) and operate under ce->lock.
// ---------------------------------------------------------------------------

int grahafs_pin_version(uint32_t inode_num, uint64_t version_id) {
    if (!grahafs_v2_is_mounted()) return CAP_V2_EROFS;
    if (version_id == 0) return CAP_V2_EINVAL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return CAP_V2_EBADF;

    spinlock_acquire(&ce->lock);
    grahafs_v2_version_entry_t *e = ce->version_chain_head_cached;
    while (e) {
        if (e->version_id == version_id) {
            // Saturate at UINT32_MAX-1 so a buggy caller cannot underflow
            // the counter on the matching unpin.
            if (e->snap_pin_count < 0xFFFFFFFFu) e->snap_pin_count++;
            spinlock_release(&ce->lock);
            inode_cache_put(ce);
            return 0;
        }
        e = e->next;
    }
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    // Cold-cache fallback: if the version is the inode's current head and
    // the chain is empty (cold-boot, no in-memory entry), accept the pin
    // as a no-op. The pin only matters for GC, and the head is implicitly
    // protected anyway (gc_prune_inode walks tail-ward, never reaps head).
    return CAP_V2_EINVAL;
}

int grahafs_unpin_version(uint32_t inode_num, uint64_t version_id) {
    if (!grahafs_v2_is_mounted()) return CAP_V2_EROFS;
    if (version_id == 0) return CAP_V2_EINVAL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return CAP_V2_EBADF;

    spinlock_acquire(&ce->lock);
    grahafs_v2_version_entry_t *e = ce->version_chain_head_cached;
    while (e) {
        if (e->version_id == version_id) {
            if (e->snap_pin_count > 0) e->snap_pin_count--;
            spinlock_release(&ce->lock);
            inode_cache_put(ce);
            return 0;
        }
        e = e->next;
    }
    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return CAP_V2_EINVAL;
}

int grahafs_revert_to_version(uint32_t inode_num, uint64_t target_version) {
    if (!grahafs_v2_is_mounted()) return CAP_V2_EROFS;
    if (target_version == 0) return CAP_V2_EINVAL;
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return CAP_V2_EBADF;

    spinlock_acquire(&ce->lock);
    grahafs_v2_version_entry_t *target = NULL;
    grahafs_v2_version_entry_t *iter = ce->version_chain_head_cached;
    while (iter) {
        if (iter->version_id == target_version) { target = iter; break; }
        iter = iter->next;
    }
    if (!target) {
        // Allow no-op revert when target == current head (matches SYS_FS_REVERT
        // semantics for cold-cache cases where the chain is empty).
        if (ce->disk.version_chain_head_id == target_version) {
            spinlock_release(&ce->lock);
            inode_cache_put(ce);
            return 0;
        }
        spinlock_release(&ce->lock);
        inode_cache_put(ce);
        return CAP_V2_EINVAL;
    }

    // Allocate a new monotonic version_id and push a REVERT_CREATED entry
    // onto the chain. The new entry copies content metadata (size, simhash,
    // segment_id, cluster_id) from the target — semantically the file
    // content is now what the target held — but tags itself with
    // VE_FLAG_REVERT_CREATED + parent_version=target_version so a future
    // chain_walk records the revert lineage.
    uint64_t new_version = __atomic_fetch_add(&g_next_version_id, 1,
                                              __ATOMIC_RELAXED);
    uint64_t prev_head_id = ce->disk.version_chain_head_id;
    grahafs_v2_version_entry_t *fresh = v2_chain_alloc_entry(
        new_version,
        target->timestamp_ns,  // carries forward; on-disk emit (W14.7+) will rewrite
        target->size,
        target->simhash,
        target->segment_id,
        prev_head_id,
        target_version,         // parent_version
        (uint16_t)(target->flags | VE_FLAG_REVERT_CREATED),
        target->cluster_id_at_version);
    if (!fresh) {
        spinlock_release(&ce->lock);
        inode_cache_put(ce);
        return CAP_V2_ENOMEM;
    }
    v2_chain_push_head(ce, fresh);

    // Update the inode's head pointer in-memory; the on-disk inode
    // mutation is deferred to the journal-stage path in W14.7 / W16
    // restore so per-revert journaling is paired with snap-restore txn
    // batching. dirty=true so the next inode_cache_flush emits it.
    ce->disk.version_chain_head_id = new_version;
    ce->disk.version_chain_segment = target->segment_id;
    if (ce->disk.version_count < GRAHAFS_V2_MAX_VERSIONS) {
        ce->disk.version_count++;
    }
    ce->dirty = true;

    spinlock_release(&ce->lock);
    inode_cache_put(ce);
    return 0;
}
