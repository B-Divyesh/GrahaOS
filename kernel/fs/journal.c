// kernel/fs/journal.c
//
// Phase 19 — full-data physical journal. See journal.h for the contract,
// grahafs_v2.h for on-disk layout, and journal_barrier.h for the barrier
// primitive.

#define __GRAHAFS_V2_INTERNAL__
#include "journal.h"

#include <stddef.h>
#include <string.h>

#include "grahafs_v2.h"
#include "journal_barrier.h"
#include "blk_client.h"
#include "../log.h"
#include "../mm/kheap.h"
#include "../sync/spinlock.h"
#include "../audit.h"
#include "../lib/crc32.h"

// ---------------------------------------------------------------------------
// In-memory journal state. `append_lock` serializes begin→commit; held only
// inside one txn's lifecycle, never reentered.
// ---------------------------------------------------------------------------
grahafs_v2_journal_state_t g_v2_journal;

static int       g_journal_device_id = -1;
static uint64_t  g_journal_sb_block_lba = 0;  // Where sb lives (always 0).

// FU29.H perf (#665): persist the sb (journal head/tail/last_txn_id) LAZILY —
// every JOURNAL_SB_PERSIST_INTERVAL commits — rather than on every commit. See
// the call site in journal_txn_commit for the crash-safety argument (FLUSH #3
// already makes home durable; head/tail are invariant at base; last_txn_id
// reuse after a crash is benign under single-txn-in-flight replay).
#define JOURNAL_SB_PERSIST_INTERVAL 64u
static uint32_t  g_v2_journal_commits_since_sb = 0;

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------
static int ahci_write_block(int dev, uint64_t lba, const void *buf) {
    /* Phase 23 cutover: route through blk_client wrapper.  FU29.H: v2 uses
     * 4096-byte logical blocks, so go through the scaled v2 helper (block*8,
     * 8 sectors) — `lba` here is a 4096-byte block index (journal/main-area
     * block), NOT a 512-byte sector.  Helper returns 1 on full success. */
    int rc = grahafs_v2_block_write((uint8_t)dev, lba, buf);
    return rc == 1 ? 0 : -5;
}

static int ahci_read_block(int dev, uint64_t lba, void *buf) {
    int rc = grahafs_v2_block_read((uint8_t)dev, lba, buf);
    return rc == 1 ? 0 : -5;
}

// Persist the in-memory journal head/tail into sb on disk. Called after
// successful commit + checkpoint.
static int persist_journal_head_to_sb(void) {
    uint8_t sb_block[GRAHAFS_V2_BLOCK_SIZE];
    if (ahci_read_block(g_journal_device_id, g_journal_sb_block_lba, sb_block) != 0) {
        return -5;
    }
    grahafs_v2_superblock_t *sb = (grahafs_v2_superblock_t *)sb_block;
    if (sb->magic != GRAHAFS_V2_SB_MAGIC) return -126;
    sb->journal_head_block = g_v2_journal.head_block;
    sb->journal_tail_block = g_v2_journal.tail_block;
    sb->last_txn_id        = g_v2_journal.next_txn_id;
    sb->checksum_sb        = 0;
    sb->checksum_sb        = crc32_buf(sb,
        offsetof(grahafs_v2_superblock_t, checksum_sb));
    if (ahci_write_block(g_journal_device_id, g_journal_sb_block_lba, sb_block) != 0) {
        return -5;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subsystem lifecycle.
// ---------------------------------------------------------------------------
int journal_subsystem_init(int device_id,
                           const grahafs_v2_superblock_t *sb) {
    memset(&g_v2_journal, 0, sizeof(g_v2_journal));
    g_v2_journal.magic               = GRAHAFS_V2_JOURNAL_BEGIN;
    g_v2_journal.journal_base_block  = sb->journal_start_block;
    g_v2_journal.journal_size_blocks = sb->journal_blocks;
    g_v2_journal.head_block          = sb->journal_head_block
                                     ? sb->journal_head_block
                                     : sb->journal_start_block;
    g_v2_journal.tail_block          = sb->journal_tail_block
                                     ? sb->journal_tail_block
                                     : sb->journal_start_block;
    g_v2_journal.next_txn_id         = sb->last_txn_id + 1;
    g_v2_journal.checkpoint_in_progress = false;
    spinlock_init(&g_v2_journal.append_lock, "v2_journal");
    g_journal_device_id = device_id;
    g_journal_sb_block_lba = 0;
    g_v2_journal_commits_since_sb = 0;

    klog(KLOG_INFO, SUBSYS_FS,
         "journal_init: base=%llu size=%u head=%llu tail=%llu next_txn=%llu",
         (unsigned long long)g_v2_journal.journal_base_block,
         g_v2_journal.journal_size_blocks,
         (unsigned long long)g_v2_journal.head_block,
         (unsigned long long)g_v2_journal.tail_block,
         (unsigned long long)g_v2_journal.next_txn_id);
    return 0;
}

void journal_subsystem_shutdown(void) {
    // Nothing in-flight for MVP — append_lock guarantees serialization.
    memset(&g_v2_journal, 0, sizeof(g_v2_journal));
    g_journal_device_id = -1;
}

// ---------------------------------------------------------------------------
// Replay.
//
// MVP scope: one in-flight txn at a time. At crash, at most one txn straddles
// (head_in_mem, head_on_disk_in_sb]. Scan forward from sb.journal_head; if a
// begin block is found, look for a matching commit block and verify CRC;
// replay if valid, discard otherwise.
// ---------------------------------------------------------------------------
int journal_replay(int device_id, grahafs_v2_superblock_t *sb) {
    uint64_t scan_lba = sb->journal_head_block;
    uint64_t base     = sb->journal_start_block;
    uint64_t end      = base + sb->journal_blocks;
    uint32_t replayed = 0;
    uint32_t discarded = 0;

    if (scan_lba < base) scan_lba = base;

    // Read one block at a time looking for a begin magic. MVP: only the
    // very next slot after the committed head can hold a partial txn — so
    // we scan at most JOURNAL_TXN_MAX_BLOCKS slots before giving up.
    //
    // Buffers are heap-allocated rather than on-stack: this function lives
    // deep in the boot/mount call chain (blk_client_fs_init →
    // grahafs_v2_mount → journal_replay), and the on-stack variants
    // (3× 4 KiB) plus other 4 KiB buffers in the same chain were
    // overflowing the kernel stack. The kmalloc cost is negligible
    // because journal_replay only fires when there is actual work
    // (uncommitted txns at last shutdown) — usually a no-op.
    uint32_t scan_budget = JOURNAL_TXN_MAX_BLOCKS;
    uint8_t *buf        = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    uint8_t *commit_buf = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    uint8_t *payload    = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    if (!buf || !commit_buf || !payload) {
        if (buf) kfree(buf);
        if (commit_buf) kfree(commit_buf);
        if (payload) kfree(payload);
        klog(KLOG_ERROR, SUBSYS_FS,
             "journal_replay: kmalloc(3x %u) failed", GRAHAFS_V2_BLOCK_SIZE);
        return -3;  // -ENOMEM
    }

    while (scan_budget-- > 0 && scan_lba < end) {
        if (ahci_read_block(device_id, scan_lba, buf) != 0) {
            klog(KLOG_ERROR, SUBSYS_FS,
                 "journal_replay: read at lba=%llu failed",
                 (unsigned long long)scan_lba);
            break;
        }
        grahafs_v2_journal_begin_block_t *begin =
            (grahafs_v2_journal_begin_block_t *)buf;
        if (begin->magic != GRAHAFS_V2_JOURNAL_BEGIN) {
            // No uncommitted txn here. Stop scanning.
            break;
        }
        uint64_t begin_lba  = scan_lba;
        uint64_t txn_id     = begin->txn_id;
        uint32_t block_count = begin->block_count;
        if (block_count < 2 || block_count > JOURNAL_TXN_MAX_BLOCKS) {
            klog(KLOG_WARN, SUBSYS_FS,
                 "journal_replay: txn=%llu bad block_count=%u — discarded",
                 (unsigned long long)txn_id, block_count);
            discarded++;
            break;
        }
        uint64_t commit_lba = begin_lba + block_count - 1u;
        if (commit_lba >= end) {
            klog(KLOG_WARN, SUBSYS_FS,
                 "journal_replay: txn=%llu would overrun journal — discarded",
                 (unsigned long long)txn_id);
            discarded++;
            break;
        }
        if (ahci_read_block(device_id, commit_lba, commit_buf) != 0) {
            discarded++;
            break;
        }
        grahafs_v2_journal_commit_block_t *commit =
            (grahafs_v2_journal_commit_block_t *)commit_buf;
        if (commit->magic != GRAHAFS_V2_JOURNAL_COMMIT ||
            commit->txn_id != txn_id) {
            klog(KLOG_WARN, SUBSYS_FS,
                 "journal_replay: txn=%llu missing commit — partial, discarded",
                 (unsigned long long)txn_id);
            discarded++;
            break;
        }
        // CRC check: re-read begin + data/meta blocks, fold into CRC32.
        uint32_t crc = crc32_init();
        crc = crc32_update(crc, buf, GRAHAFS_V2_BLOCK_SIZE);
        // Re-read the begin copy since `buf` may change below.
        uint32_t refs_n = begin->ref_count;
        for (uint32_t i = 0; i < refs_n; ++i) {
            if (ahci_read_block(device_id, begin->refs[i].journal_lba, payload) != 0) {
                crc = 0;
                break;
            }
            crc = crc32_update(crc, payload, GRAHAFS_V2_BLOCK_SIZE);
        }
        uint32_t final_crc = crc32_final(crc);
        if (final_crc != commit->checksum) {
            klog(KLOG_WARN, SUBSYS_FS,
                 "journal_replay: txn=%llu CRC mismatch (got=0x%08x exp=0x%08x)",
                 (unsigned long long)txn_id, final_crc, commit->checksum);
            discarded++;
            break;
        }
        // Apply each ref to main area.
        for (uint32_t i = 0; i < refs_n; ++i) {
            if (ahci_read_block(device_id, begin->refs[i].journal_lba, payload) != 0 ||
                ahci_write_block(device_id, begin->refs[i].lba_target, payload) != 0) {
                klog(KLOG_ERROR, SUBSYS_FS,
                     "journal_replay: apply failed target=%llu",
                     (unsigned long long)begin->refs[i].lba_target);
                break;
            }
        }
        replayed++;
        scan_lba += block_count;
    }

    kfree(buf);
    kfree(commit_buf);
    kfree(payload);

    if (replayed > 0 || discarded > 0) {
        klog(KLOG_INFO, SUBSYS_FS,
             "journal_replay: replayed=%u discarded=%u",
             replayed, discarded);
    }
    audit_write_fs_journal_replay(replayed, discarded);

    // Reset head/tail. MVP is single-txn-in-flight, so post-replay the
    // journal is always empty.
    sb->journal_head_block = sb->journal_start_block;
    sb->journal_tail_block = sb->journal_start_block;
    sb->checksum_sb = 0;
    sb->checksum_sb = crc32_buf(sb,
        offsetof(grahafs_v2_superblock_t, checksum_sb));
    if (ahci_write_block(device_id, g_journal_sb_block_lba, sb) != 0) {
        return -5;
    }
    g_v2_journal.head_block = sb->journal_start_block;
    g_v2_journal.tail_block = sb->journal_start_block;
    return 0;
}

// ---------------------------------------------------------------------------
// Txn lifecycle.
// ---------------------------------------------------------------------------
journal_txn_t *journal_txn_begin(void) {
    // Append-lock serializes the entire begin→commit window. Acquire here;
    // release in commit() / abort().
    spinlock_acquire(&g_v2_journal.append_lock);

    journal_txn_t *txn = kmalloc(sizeof(journal_txn_t), SUBSYS_FS);
    if (!txn) {
        spinlock_release(&g_v2_journal.append_lock);
        return NULL;
    }
    memset(txn, 0, sizeof(*txn));
    txn->txn_id = g_v2_journal.next_txn_id++;
    txn->flags  = 0;
    return txn;
}

int journal_txn_add_block(journal_txn_t *txn, uint64_t lba_target,
                          uint8_t kind, const void *buf_4096) {
    if (!txn || !buf_4096) return -22;
    if (txn->ref_count >= GRAHAFS_V2_JOURNAL_BLOCK_REFS_MAX) return -27;  // -EFBIG.

    uint32_t slot = txn->ref_count;
    uint8_t *copy = kmalloc(GRAHAFS_V2_BLOCK_SIZE, SUBSYS_FS);
    if (!copy) return -3;  // -ENOMEM.
    memcpy(copy, buf_4096, GRAHAFS_V2_BLOCK_SIZE);

    txn->payloads[slot] = copy;
    txn->refs[slot].lba_target = lba_target;
    txn->refs[slot].journal_lba = 0;  // Assigned at commit.
    txn->refs[slot].kind = kind;
    txn->ref_count++;
    if (kind == JOURNAL_BLOCK_KIND_DATA) txn->data_block_count++;
    else if (kind == JOURNAL_BLOCK_KIND_METADATA) txn->metadata_block_count++;
    return 0;
}

static void journal_txn_free_payloads(journal_txn_t *txn) {
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        if (txn->payloads[i]) {
            kfree(txn->payloads[i]);
            txn->payloads[i] = NULL;
        }
    }
}

void journal_txn_abort(journal_txn_t *txn) {
    if (!txn) return;
    journal_txn_free_payloads(txn);
    kfree(txn);
    spinlock_release(&g_v2_journal.append_lock);
}

// ---------------------------------------------------------------------------
// Commit — the two-barrier protocol.
// ---------------------------------------------------------------------------
int journal_txn_commit(journal_txn_t *txn) {
    if (!txn) return -22;
    if (txn->ref_count == 0) {
        journal_txn_abort(txn);
        return 0;  // Empty commit is a no-op.
    }

    uint32_t block_count = 2u + txn->ref_count;  // begin + refs + commit.
    uint64_t head = g_v2_journal.head_block;
    uint64_t base = g_v2_journal.journal_base_block;
    uint64_t end  = base + g_v2_journal.journal_size_blocks;

    if (head + block_count > end) {
        // Would wrap. MVP policy: force reset-to-base (no circular reuse).
        // This is safe because inline checkpoint ensures all data is already
        // in main area after the previous commit — journal is effectively
        // empty. Plan §Tricky Bit #11 permits this simplification.
        head = base;
    }

    uint64_t begin_lba  = head;
    uint64_t commit_lba = head + block_count - 1u;

    // --- (1) Build begin block + assign journal LBAs ---
    grahafs_v2_journal_begin_block_t begin;
    memset(&begin, 0, sizeof(begin));
    begin.magic                = GRAHAFS_V2_JOURNAL_BEGIN;
    begin.txn_id               = txn->txn_id;
    begin.flags                = txn->flags;
    begin.timestamp_ns         = 0;  // Wall clock not critical; klog tags fill.
    begin.block_count          = block_count;
    begin.data_block_count     = txn->data_block_count;
    begin.metadata_block_count = txn->metadata_block_count;
    begin.ref_count            = txn->ref_count;
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        uint64_t journal_lba = begin_lba + 1u + i;
        txn->refs[i].journal_lba = journal_lba;
        begin.refs[i].lba_target  = txn->refs[i].lba_target;
        begin.refs[i].journal_lba = journal_lba;
        begin.refs[i].kind        = txn->refs[i].kind;
    }

    // Write begin.
    if (ahci_write_block(g_journal_device_id, begin_lba, &begin) != 0) {
        klog(KLOG_ERROR, SUBSYS_FS, "journal_commit: begin write failed txn=%llu",
             (unsigned long long)txn->txn_id);
        journal_txn_abort(txn);
        return -5;
    }

    // Write data + metadata blocks.
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        if (ahci_write_block(g_journal_device_id,
                             txn->refs[i].journal_lba,
                             txn->payloads[i]) != 0) {
            klog(KLOG_ERROR, SUBSYS_FS,
                 "journal_commit: payload write failed txn=%llu slot=%u",
                 (unsigned long long)txn->txn_id, i);
            journal_txn_abort(txn);
            return -5;
        }
    }

    // --- (2+3) Build commit block (CRC over begin+payloads), write it
    //           directly after the payloads, then ONE durability barrier.
    //
    // FU29.H perf (#665): the original protocol used FLUSH #1 (order journal
    // data before the commit marker) AND FLUSH #2 (commit marker durable) —
    // two device FLUSHes, the dominant per-commit cost. The pre-commit barrier
    // is REDUNDANT given the commit block's CRC over begin+payloads: on replay
    // (journal_replay) a torn or device-reordered journal write — commit marker
    // durable but a payload not — fails the CRC recompute (replay re-reads the
    // journal copies at refs[i].journal_lba and folds the same CRC) and the txn
    // is DISCARDED, the exact outcome the pre-commit ordering guaranteed. So a
    // single barrier after [begin + payloads + commit] makes the whole txn
    // atomically durable-or-discarded. This is the standard checksummed-commit
    // optimization (cf. ext4 journal_async_commit) and removes one FLUSH from
    // every v2 mutation. The post-checkpoint barrier (#3 below) is NOT
    // removable: reset-to-base journal reuse would otherwise let the next
    // commit overwrite this txn's journal copy before its home writes are
    // durable (adversarially confirmed data-loss otherwise).
    uint32_t crc = crc32_init();
    crc = crc32_update(crc, &begin, GRAHAFS_V2_BLOCK_SIZE);
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        crc = crc32_update(crc, txn->payloads[i], GRAHAFS_V2_BLOCK_SIZE);
    }
    grahafs_v2_journal_commit_block_t commit_blk;
    memset(&commit_blk, 0, sizeof(commit_blk));
    commit_blk.magic    = GRAHAFS_V2_JOURNAL_COMMIT;
    commit_blk.txn_id   = txn->txn_id;
    commit_blk.checksum = crc32_final(crc);

    if (ahci_write_block(g_journal_device_id, commit_lba, &commit_blk) != 0) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "journal_commit: commit write failed txn=%llu",
             (unsigned long long)txn->txn_id);
        journal_txn_abort(txn);
        return -5;
    }

    // --- (4) Single journal-durability barrier (replaces the old pre+post
    //         pair): [begin + payloads + commit] all durable as a unit; CRC is
    //         the torn/reorder backstop on replay. ---
    if (journal_barrier(g_journal_device_id) != 0) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "journal_commit: journal barrier failed txn=%llu",
             (unsigned long long)txn->txn_id);
        journal_txn_abort(txn);
        return -5;
    }

    // --- (5) Inline checkpoint: apply every ref to its main-area LBA ---
    for (uint32_t i = 0; i < txn->ref_count; ++i) {
        if (ahci_write_block(g_journal_device_id,
                             txn->refs[i].lba_target,
                             txn->payloads[i]) != 0) {
            klog(KLOG_ERROR, SUBSYS_FS,
                 "journal_commit: checkpoint failed txn=%llu slot=%u target=%llu",
                 (unsigned long long)txn->txn_id, i,
                 (unsigned long long)txn->refs[i].lba_target);
            // Journal has the data; replay on next mount will recover.
            journal_txn_abort(txn);
            return -5;
        }
    }

    // --- (6) Barrier BEFORE sb advance — ensures main-area writes durable ---
    if (journal_barrier(g_journal_device_id) != 0) {
        journal_txn_abort(txn);
        return -5;
    }

    // --- (7) Advance journal head/tail (MVP: reset to base after commit) ---
    g_v2_journal.head_block = base;
    g_v2_journal.tail_block = base;
    // FU29.H perf (#665): persist the sb LAZILY (every JOURNAL_SB_PERSIST_INTERVAL
    // commits) instead of on every commit, removing one sb READ + one sb WRITE
    // from the common path. Safe because: (a) FLUSH #3 above has already made
    // THIS txn's home writes durable, so the on-disk journal copy is never
    // needed again; (b) head/tail always reset to base, so the sb's
    // journal_head/tail fields are invariant (already base on disk after the
    // first persist, and replay clamps a sub-base head to base regardless);
    // (c) the only field that drifts is last_txn_id, and txn_id reuse after a
    // crash is benign — single-txn-in-flight replay validates by begin/commit
    // magic + CRC, never by absolute id ordering.
    if (++g_v2_journal_commits_since_sb >= JOURNAL_SB_PERSIST_INTERVAL) {
        g_v2_journal_commits_since_sb = 0;
        (void)persist_journal_head_to_sb();
    }

    // --- (8) Done — free payloads, release append_lock via abort() ---
    klog(KLOG_DEBUG, SUBSYS_FS,
         "journal_commit: ok txn=%llu blocks=%u data=%u meta=%u",
         (unsigned long long)txn->txn_id,
         txn->ref_count,
         txn->data_block_count,
         txn->metadata_block_count);
    journal_txn_free_payloads(txn);
    kfree(txn);
    spinlock_release(&g_v2_journal.append_lock);
    return 0;
}

uint64_t journal_get_next_txn_id(void) { return g_v2_journal.next_txn_id; }
uint64_t journal_get_head(void)        { return g_v2_journal.head_block; }
uint64_t journal_get_tail(void)        { return g_v2_journal.tail_block; }
