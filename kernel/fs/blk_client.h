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
typedef enum {
    BLK_CLIENT_DISCONNECTED  = 0,
    BLK_CLIENT_CONNECTING    = 1,
    BLK_CLIENT_READY         = 2,  /* /sys/blk/service connected, can issue */
    BLK_CLIENT_ERROR         = 3,  /* ahcid died; reconnect pending */
    BLK_CLIENT_KERNEL_DIRECT = 4,  /* Transitional: legacy in-kernel AHCI active */
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
