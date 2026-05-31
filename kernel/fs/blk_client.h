// kernel/fs/blk_client.h — Phase 23 S4.
//
// Kernel-side block-I/O client. Single instance (g_blk_kernel_client) shared
// across all kernel-resident filesystem code (grahafs_v2, grahafs v1
// fallback, journal, segment, vfs). All disk I/O converges on four wrappers
// here: grahafs_block_read / grahafs_block_write / grahafs_block_flush /
// grahafs_block_identify. Phase 23's invariant: NO kernel code calls
// ahci_read/write/flush_cache directly; everything goes through these
// wrappers, which today (Phase 23 S4) shim onto the legacy in-kernel AHCI
// driver and tomorrow (when ahcid stabilises) will dispatch via channel
// to /bin/ahcid. The wrapper API is the cutover point.
//
// FS error state. Phase 23 introduces FS_OK / FS_READ_ONLY_ERROR. Set on
// blk_client EPIPE (ahcid death); cleared after journal replay completes.
// Write paths in grahafs_v2 check the state and return -EROFS when set.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "blk_proto.h"

// --- FS-wide error state -------------------------------------------------
typedef enum {
    BLK_FS_OK                = 0,
    BLK_FS_READ_ONLY_ERROR   = 1,  /* ahcid died; journal-replay-pending */
} blk_fs_state_t;

// --- Client connection state --------------------------------------------
// Phase 24a W10: KERNEL_DIRECT removed.  All FS I/O goes through channel
// mode now.  States are linear: DISCONNECTED → CONNECTING → READY ↔ ERROR.
typedef enum {
    BLK_CLIENT_DISCONNECTED  = 0,
    BLK_CLIENT_CONNECTING    = 1,
    BLK_CLIENT_READY         = 2,  /* /sys/blk/service connected, can issue */
    BLK_CLIENT_ERROR         = 3,  /* ahcid died; reconnect pending */
} blk_client_state_t;

// Initialize the singleton client. Called early in kmain (before any FS).
// Sets state=DISCONNECTED. Allocates the shared DMA VMO if running in
// channel mode; no-op stub in kernel-direct mode.
void blk_client_init(void);

// Spawn the kernel-side mount task. Polls /sys/blk/service publication;
// when ahcid is up, calls blk_client_connect and grahafs_v2_mount.
// In transitional mode (Phase 23 S4 first pass), this directly mounts
// against the legacy in-kernel AHCI driver without polling.
void blk_client_start_mount_task(int device_id);

// Phase 23 Step 2: spawn the kernel-side blk_client task (kt task).
// The kt task waits for kmain to set g_blk_mount_done, then in
// init mode polls /sys/blk/service for publication by /bin/ahcid (spawned
// by /etc/init.conf), connects via rawnet_connect, allocates a 256 KiB
// shared DMA VMO, sends the BLK_PROTO handshake, and transitions state
// to BLK_CLIENT_READY.  In ktest mode (autorun=ktest) the kt task parks
// without doing any work — the gate exercises kernel-direct AHCI exclusively
// and bringing up ahcid would corrupt PxCLB/PxFB.  Step 3 will widen
// activation to ktest mode along with the kernel-direct strip.
void blk_client_start_kt(void);

// Phase 23 Step 2: synchronization flag set by kmain after the synchronous
// grahafs mount completes.  The kt task waits on this to avoid racing
// with mount-time block I/O via the legacy in-kernel AHCI driver.
extern volatile uint32_t g_blk_mount_done;

// Phase 23 Step 3: kt-task settled flag.  Set to 1 by the kt task once it
// has either transitioned to BLK_CLIENT_READY (channel mode active) OR
// committed to parking (handshake failed; channel mode unavailable).  kmain
// blocks on this AFTER blk_client_start_kt() and BEFORE spawning the
// autorun child, so userspace tests don't race with ahcid bring-up.  Even
// in ktest mode (which spawns ahcid kernel-context from the kt task), the
// settle handshake is the same — kmain unblocks once kt is ready or has
// given up.
extern volatile uint32_t g_blk_kt_settled;

// State accessors.
blk_client_state_t blk_client_state(void);
blk_fs_state_t     blk_fs_state(void);
void               blk_fs_set_error(int reason);
void               blk_fs_clear_error(void);

// --- The four wrappers --------------------------------------------------
// All kernel filesystem code calls these instead of ahci_*.
//
// dev:   device index, today always 0 (single drive on QEMU default disk).
// lba:   512-byte sector LBA.
// count: 512-byte sector count.
// kbuf:  kernel-virt pointer to the I/O buffer; must be at least count*512
//        bytes large.
//
// All return 0 on success, negative errno on failure. -EROFS if FS_ERROR
// is set on a write/flush. -EIO if ahcid is dead and no kernel-direct
// fallback is available. -EAGAIN if mount has not yet completed.
int grahafs_block_read(uint8_t dev, uint64_t lba, uint32_t count, void *kbuf);
int grahafs_block_write(uint8_t dev, uint64_t lba, uint32_t count, const void *kbuf);
int grahafs_block_flush(uint8_t dev);

// FU29.H — GrahaFS v2 logical-block I/O.  v2 uses 4096-byte logical blocks but
// the block layer / ahcid is 512-byte-sector granular (fis->lba is a sector;
// dbc = count*512).  These helpers scale a 4096-byte BLOCK index to its
// 8-sector run (block*8) and transfer a full 4 KiB block (count=8) — exactly
// the convention v1's grahafs.c uses (block_num * 8u, count 8u).  Without this,
// v2 read block N at sector N (byte N*512) instead of byte N*4096, so its
// inode table overlapped the on-disk bitmap (garbage 0xffffffff reads).
// Return 1 on full success (preserving callers' "== 1" contract), else <0.
int grahafs_v2_block_read(uint8_t dev, uint64_t block, void *buf4096);
int grahafs_v2_block_write(uint8_t dev, uint64_t block, const void *buf4096);

// Phase 24a W3: batched read. Submits up to BLK_BATCH_MAX (= 6) reads in
// one chan_send. Each kbufs[i] receives counts[i]*512 bytes from lbas[i].
// Returns the number of successfully-completed reads (0..n) on the
// channel-mode path, or -EAGAIN/-EIO if channel mode is unavailable
// (caller should fall back to grahafs_block_read in a loop).
//
// For grahafs_compute_simhash and other speculative readers: best-effort
// semantics — partial completion (return < n) is signalled by the
// corresponding kbufs[i] being left untouched. Per-op error code is not
// returned; if you need it, use grahafs_block_read in a loop.
//
// Multiplicative speedup with W2 chan_send fastpath: the entire batch is
// one chan_send → ahcid issues all to PxCI in one MMIO store → AHCI HBA
// processes in parallel (NCS=32 supports up to 32 concurrent).
int grahafs_block_read_batch(uint8_t dev,
                             const uint64_t *lbas,
                             const uint32_t *counts,
                             void *const *kbufs,
                             uint32_t n);
// grahafs_block_identify returns the cached 512-byte IDENTIFY DEVICE
// payload for `dev` into `out_512`. Today this calls ahci's enum logic;
// future channel mode delegates to ahcid's BLK_OP_IDENTIFY.
int grahafs_block_identify(uint8_t dev, void *out_512);

// --- Lifecycle hooks ----------------------------------------------------
// Called when ahcid disconnects (rawnet_on_peer_death). Marks state=ERROR,
// wakes any blocked waiters with -EPIPE, sets FS_READ_ONLY_ERROR.
void blk_client_on_ahcid_death(int32_t pid);

// Called when /sys/blk/service is republished by a respawned ahcid.
// Reconnects, runs journal replay, clears FS_ERROR.
void blk_client_on_ahcid_alive(void);

// Telemetry.
uint32_t blk_client_reconnect_count(void);
uint64_t blk_client_request_count(void);
