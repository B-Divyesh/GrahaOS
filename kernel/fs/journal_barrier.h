// kernel/fs/journal_barrier.h
//
// Phase 19 — device-level write barrier for journal / superblock persistence.
//
// `journal_barrier(port_num)` wraps `ahci_flush_cache(port_num)` with:
//   * a fault-injection probe checked at entry (build-time gated),
//   * an audit-worthy failure path (if the flush itself errors, we bubble
//     it up to the caller so txn_commit can mark the txn failed rather
//     than reporting false success).
//
// Two-barrier commit protocol (Phase 19 plan Tricky Bit #1):
//
//   (a) write begin + data + metadata blocks via ahci_write
//       (each ahci_vfs_write auto-flushes per-block),
//   (b) journal_barrier(port)   ← THIS function — ORDERS device writeback
//                                  BEFORE the commit record,
//   (c) write commit block,
//   (d) journal_barrier(port)   ← durability fence AFTER commit,
//   (e) inline checkpoint applies data blocks to main area,
//   (f) journal_barrier(port)   ← durability fence BEFORE superblock
//                                  tail-advance.
//
// Without (b) a device may reorder the commit block ahead of buffered data
// — replay would then read corrupted data with a valid commit magic.
// Without (f) a second crash could find journal_tail past unflushed data.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fault-injection control (Phase 19 plan §Tricky Bit #15). Production builds
// set this to 0; tests toggle via scripts/run_fault_injection.sh.
extern volatile uint32_t g_journal_barrier_fault_inject;

// Force the current port's write cache to durable storage. Returns 0 on
// success, negative (-EIO) on AHCI failure or fault-injection trip.
int journal_barrier(int port_num);
