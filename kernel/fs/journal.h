// kernel/fs/journal.h
//
// Phase 19 — full-data physical journal for GrahaFS v2.
//
// Lifecycle:
//    journal_subsystem_init(device, sb)  — at mount. Reads sb.journal_*.
//    journal_replay(device, sb)          — at mount, pre-use. Applies any
//                                          committed-but-unapplied txns.
//    journal_txn_begin()                 — returns an in-memory txn handle.
//    journal_txn_add_block(txn, lba, kind, bytes_4096)
//                                        — stages one block.
//    journal_txn_commit(txn)             — two-barrier commit + inline
//                                          checkpoint + txn-free.
//    journal_subsystem_shutdown()        — flushes any in-flight state.
//
// TWO-BARRIER COMMIT PROTOCOL (Plan §Tricky Bit #1):
//
//    1. Write begin block (header + block_refs[])    via ahci_write.
//    2. Write all data blocks + metadata blocks      via ahci_write.
//    3. journal_barrier()  ← orders device writeback BEFORE commit.
//    4. Write commit block (with CRC over 1+2).
//    5. journal_barrier()  ← durability fence for commit block.
//    6. Inline checkpoint: apply blocks to main area via ahci_write.
//    7. journal_barrier()  ← fence before advancing journal_head in sb.
//    8. Update sb.journal_head in memory.
//
// CRC covers begin+data+metadata (not just data) — prevents "torn-zero
// begin, valid commit" false positives.
//
// Replay:
//    From sb.journal_tail scan forward. For each begin block found,
//    scan ahead block_count slots for a commit block with matching txn_id
//    and valid CRC. If found, re-apply each block_refs[i] (data or meta)
//    from journal_lba → lba_target. Discard partial txns (no commit found).
//    Advance sb.journal_head/tail after replay.
//
// MVP simplifications:
//    * One txn in flight at a time (append_lock serializes).
//    * After successful commit + checkpoint, tail == head → journal is
//      effectively empty on the happy path.
//    * No lazy indirect-block allocation — caller pre-stages every block
//      it wants journaled.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grahafs_v2.h"

// Maximum blocks per txn (including begin + commit). Bounded by the
// begin-block ref array.
#define JOURNAL_TXN_MAX_BLOCKS (2u + GRAHAFS_V2_JOURNAL_BLOCK_REFS_MAX)

// In-memory transaction handle.
typedef struct journal_txn {
    uint64_t txn_id;
    uint32_t flags;
    // Counts, updated as add_block is called.
    uint32_t data_block_count;
    uint32_t metadata_block_count;
    uint32_t ref_count;
    // Each ref carries the 4 KiB payload inline (up to 168 blocks × 4 KiB
    // = 672 KiB per txn). Keeps staging self-contained; caller hands bytes
    // in, we copy to heap, then persist at commit.
    grahafs_v2_journal_block_ref_t refs[GRAHAFS_V2_JOURNAL_BLOCK_REFS_MAX];
    uint8_t *payloads[GRAHAFS_V2_JOURNAL_BLOCK_REFS_MAX];
} journal_txn_t;

#define JOURNAL_BLOCK_KIND_DATA      0u
#define JOURNAL_BLOCK_KIND_METADATA  1u

// Called from grahafs_v2_mount. Initialises in-memory state from sb.
int journal_subsystem_init(int device_id,
                           const grahafs_v2_superblock_t *sb);

// Called BEFORE first write at mount. Scans journal from tail → head,
// replays any committed-but-unapplied txns. On success: updates sb's
// journal_head/journal_tail in memory to reflect the replayed state and
// writes the updated superblock. Emits AUDIT_FS_JOURNAL_REPLAY.
// Returns 0 on success, -errno otherwise.
int journal_replay(int device_id, grahafs_v2_superblock_t *sb);

// Called at unmount. Flushes any in-flight commit-in-progress state.
void journal_subsystem_shutdown(void);

// Begin a new transaction. Returns NULL on ENOMEM.
journal_txn_t *journal_txn_begin(void);

// Stage one 4096-byte block for the transaction. `buf` is copied; caller
// retains ownership. `lba_target` is the main-area LBA the block will land
// at during checkpoint. Returns 0 on success, negative errno on overflow
// or OOM.
int journal_txn_add_block(journal_txn_t *txn, uint64_t lba_target,
                          uint8_t kind, const void *buf_4096);

// Commit the transaction using the two-barrier protocol. Returns 0 on
// success, negative errno on any failure. On any failure: the txn is
// aborted (in-memory freed) and NO blocks were applied to main area —
// caller can retry with fresh state.
int journal_txn_commit(journal_txn_t *txn);

// Abort a txn without committing. Frees memory. Use if caller decides
// mid-staging that the transaction should not proceed.
void journal_txn_abort(journal_txn_t *txn);

// Diagnostics — exposed for /bin/memstat.
uint64_t journal_get_next_txn_id(void);
uint64_t journal_get_head(void);
uint64_t journal_get_tail(void);
