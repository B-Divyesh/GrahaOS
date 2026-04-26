// kernel/fs/blk_client.c — Phase 23 S4.
//
// Kernel-side block-I/O client. Singleton g_blk_kernel_client. The four
// wrapper functions (grahafs_block_read/write/flush/identify) are the
// single point through which all kernel FS code (grahafs_v2, journal,
// vfs, segment) talks to storage.
//
// Phase 23 S4 stage 1 (this file): wrappers shim onto the legacy in-kernel
// AHCI driver via ahci_read/write/flush_cache. The architecture is set up
// so a Phase 23 closeout can swap the implementations to channel-mediated
// RPC against /bin/ahcid without touching any of the 46 call sites in
// grahafs_v2.c, journal.c, grahafs.c, segment.c, vfs.c, journal_barrier.c.
//
// FS_ERROR state machine: BLK_FS_OK on boot. Transitions to
// BLK_FS_READ_ONLY_ERROR when blk_client_on_ahcid_death fires. Cleared
// when blk_client_on_ahcid_alive completes (after journal replay).
// Write paths (grahafs_block_write, grahafs_block_flush) refuse with
// -EROFS while the state is READ_ONLY_ERROR.

#include "blk_client.h"
#include "blk_proto.h"

#include <stddef.h>
#include <stdint.h>

#include "../sync/spinlock.h"
#include "../log.h"
#include "../audit.h"
#include "../../arch/x86_64/drivers/ahci/ahci.h"

// External — for journal replay trigger on reconnect.
extern int  grahafs_v2_journal_replay(void);
extern bool grahafs_v2_is_mounted(void);

// --- Singleton state ----------------------------------------------------
typedef struct {
    blk_client_state_t state;
    blk_fs_state_t     fs_state;
    int                fs_error_reason;
    spinlock_t         lock;
    int32_t            ahcid_pid;             /* 0 if not channel-connected */
    uint32_t           reconnect_count;
    uint64_t           request_count;
    uint64_t           error_count;
} blk_client_kernel_t;

static blk_client_kernel_t g_blk = {
    .state      = BLK_CLIENT_DISCONNECTED,
    .fs_state   = BLK_FS_OK,
    .fs_error_reason = 0,
    .lock       = SPINLOCK_INITIALIZER("blk_client"),
    .ahcid_pid  = 0,
    .reconnect_count = 0,
    .request_count   = 0,
    .error_count     = 0,
};

// --- Lifecycle ----------------------------------------------------------
void blk_client_init(void) {
    spinlock_acquire(&g_blk.lock);
    // Phase 23 S4 stage 1: mark as kernel-direct so the wrappers know to
    // route to the in-kernel AHCI driver. When the channel-cutover lands,
    // state will transition KERNEL_DIRECT → DISCONNECTED → CONNECTING →
    // READY as ahcid publishes /sys/blk/service.
    g_blk.state    = BLK_CLIENT_KERNEL_DIRECT;
    g_blk.fs_state = BLK_FS_OK;
    spinlock_release(&g_blk.lock);
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client: init complete (kernel-direct mode)");
}

void blk_client_start_mount_task(int device_id) {
    // In stage-1 mode the kernel-direct path is synchronous; the actual
    // grahafs mount happens at kmain and goes through the kernel AHCI
    // driver. This hook exists so a future channel-cutover can defer
    // mount until /sys/blk/service publishes.
    (void)device_id;
}

blk_client_state_t blk_client_state(void) {
    return __atomic_load_n(&g_blk.state, __ATOMIC_ACQUIRE);
}

blk_fs_state_t blk_fs_state(void) {
    return __atomic_load_n(&g_blk.fs_state, __ATOMIC_ACQUIRE);
}

void blk_fs_set_error(int reason) {
    spinlock_acquire(&g_blk.lock);
    if (g_blk.fs_state != BLK_FS_READ_ONLY_ERROR) {
        g_blk.fs_state = BLK_FS_READ_ONLY_ERROR;
        g_blk.fs_error_reason = reason;
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client: FS entering READ_ONLY+ERROR (reason=%d)", reason);
    }
    spinlock_release(&g_blk.lock);
}

void blk_fs_clear_error(void) {
    spinlock_acquire(&g_blk.lock);
    blk_fs_state_t prev = g_blk.fs_state;
    g_blk.fs_state = BLK_FS_OK;
    g_blk.fs_error_reason = 0;
    spinlock_release(&g_blk.lock);
    if (prev == BLK_FS_READ_ONLY_ERROR) {
        klog(KLOG_INFO, SUBSYS_CORE,
             "blk_client: FS error cleared, back to READ_WRITE");
    }
}

// --- The four wrappers --------------------------------------------------
// Stage-1 implementation: pass-through to the legacy in-kernel AHCI driver.

int grahafs_block_read(uint8_t dev, uint64_t lba, uint32_t count, void *kbuf) {
    if (!kbuf) return -22;
    if (count == 0 || count > 0xFFFFu) return -22;
    blk_client_state_t st = blk_client_state();
    if (st == BLK_CLIENT_ERROR) return -5;       /* -EIO */
    if (st == BLK_CLIENT_DISCONNECTED) return -11; /* -EAGAIN */

    __atomic_add_fetch(&g_blk.request_count, 1, __ATOMIC_RELAXED);
    int rc = ahci_read((int)dev, lba, (uint16_t)count, kbuf);
    if (rc != 0) {
        __atomic_add_fetch(&g_blk.error_count, 1, __ATOMIC_RELAXED);
        return rc;
    }
    /* Phase 23 cutover: callers (grahafs_v2 + journal + segment) historically
     * checked `ahci_read(...) != 1` for single-block ops. ahci_read returns 0
     * on success, so those callers always took the error path — yet the
     * system "worked" because v1 fallback masked the issue. The wrapper now
     * returns the sector count on success, restoring the call-site
     * invariant. This unblocks v2 mount and matches the wrapper's
     * post-cutover semantics where the channel-mode response will return
     * `bytes / 512`. */
    return (int)count;
}

int grahafs_block_write(uint8_t dev, uint64_t lba, uint32_t count, const void *kbuf) {
    if (!kbuf) return -22;
    if (count == 0 || count > 0xFFFFu) return -22;
    if (blk_fs_state() == BLK_FS_READ_ONLY_ERROR) return -30; /* -EROFS */
    blk_client_state_t st = blk_client_state();
    if (st == BLK_CLIENT_ERROR) return -5;
    if (st == BLK_CLIENT_DISCONNECTED) return -11;

    __atomic_add_fetch(&g_blk.request_count, 1, __ATOMIC_RELAXED);
    int rc = ahci_write((int)dev, lba, (uint16_t)count, (void *)kbuf);
    if (rc != 0) {
        __atomic_add_fetch(&g_blk.error_count, 1, __ATOMIC_RELAXED);
        return rc;
    }
    return (int)count;  /* see grahafs_block_read comment above. */
}

int grahafs_block_flush(uint8_t dev) {
    if (blk_fs_state() == BLK_FS_READ_ONLY_ERROR) return -30;
    blk_client_state_t st = blk_client_state();
    if (st == BLK_CLIENT_ERROR) return -5;
    if (st == BLK_CLIENT_DISCONNECTED) return -11;

    __atomic_add_fetch(&g_blk.request_count, 1, __ATOMIC_RELAXED);
    int rc = ahci_flush_cache((int)dev);
    if (rc != 0) __atomic_add_fetch(&g_blk.error_count, 1, __ATOMIC_RELAXED);
    return rc;
}

int grahafs_block_identify(uint8_t dev, void *out_512) {
    if (!out_512) return -22;
    (void)dev;
    // Stage-1: legacy in-kernel AHCI doesn't expose IDENTIFY directly to
    // FS code (it's part of init). For now return -ENOSYS; userspace
    // ahcid implements this via its cached IDENTIFY parse.
    return -38;  /* -ENOSYS */
}

// --- Lifecycle hooks ----------------------------------------------------
void blk_client_on_ahcid_death(int32_t pid) {
    spinlock_acquire(&g_blk.lock);
    if (g_blk.ahcid_pid != pid) {
        spinlock_release(&g_blk.lock);
        return;
    }
    g_blk.ahcid_pid = 0;
    if (g_blk.state == BLK_CLIENT_READY) {
        g_blk.state = BLK_CLIENT_ERROR;
    }
    spinlock_release(&g_blk.lock);
    blk_fs_set_error(-32 /* EPIPE */);
    klog(KLOG_WARN, SUBSYS_CORE,
         "blk_client: ahcid pid=%d died; FS now READ_ONLY+ERROR", (int)pid);
}

void blk_client_on_ahcid_alive(void) {
    // Stage-1 stub. Channel-mode implementation will:
    //   1. rawnet_connect("/sys/blk/service")
    //   2. Send blk_connect_msg_t with shared DMA VMO handle.
    //   3. Wait for ack, transition to READY.
    //   4. Run grahafs_v2_journal_replay().
    //   5. blk_fs_clear_error().
    spinlock_acquire(&g_blk.lock);
    g_blk.reconnect_count++;
    spinlock_release(&g_blk.lock);
    if (grahafs_v2_is_mounted()) {
        int rc = grahafs_v2_journal_replay();
        klog(KLOG_INFO, SUBSYS_CORE,
             "blk_client: journal replay rc=%d", rc);
    }
    blk_fs_clear_error();
}

uint32_t blk_client_reconnect_count(void) {
    return __atomic_load_n(&g_blk.reconnect_count, __ATOMIC_RELAXED);
}

uint64_t blk_client_request_count(void) {
    return __atomic_load_n(&g_blk.request_count, __ATOMIC_RELAXED);
}
