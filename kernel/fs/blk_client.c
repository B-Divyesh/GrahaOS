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
#include "vfs.h"     /* Phase 24a W10: block_device_t + vfs_node_t for kt mount */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../sync/spinlock.h"
#include "../log.h"
#include "../audit.h"
#include "../cmdline.h"
#include "../mm/vmo.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/token.h"
#include "../net/rawnet.h"
#include "../ipc/channel.h"
#include "../../arch/x86_64/drivers/ahci/ahci.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/mm/vmm.h"   // g_hhdm_offset for DMA VMO kernel-virt access

// External — for journal replay trigger on reconnect.
extern int  grahafs_v2_journal_replay(void);
extern bool grahafs_v2_is_mounted(void);

// External — kernel timer tick counter (10 ms per tick, see lapic_timer_init).
extern volatile uint64_t g_timer_ticks;

// --- Phase 23 Step 2: kt task synchronization ---------------------------
// Set by kmain after the synchronous grahafs mount + audit_attach_fs
// completes.  The kt task waits on this before doing any ahcid bring-up
// work to avoid racing with the legacy in-kernel AHCI driver during
// mount-time block I/O.
volatile uint32_t g_blk_mount_done = 0;

// --- Phase 23 Step 3: kt-task settled signal ----------------------------
// kt task flips 0 → 1 once it has either reached BLK_CLIENT_READY (channel
// mode usable) or committed to parking (handshake failed).  kmain spins
// on this AFTER blk_client_start_kt and BEFORE autorun_decide so the
// userspace autorun child doesn't issue FS ops mid-bring-up — particularly
// in ktest mode, where the kt task spawns /bin/ahcid kernel-context, which
// reprograms PxCLB/PxFB on the AHCI ports.  Tests that hit kernel-direct
// during that window would silently corrupt; the settled wait makes the
// transition atomic from the autorun child's point of view.
volatile uint32_t g_blk_kt_settled = 0;

// Pid of the spawned kt task (0 == not yet spawned).  Idempotency guard
// for blk_client_start_kt.
static int g_blk_kt_pid = 0;

// Polling cadence: tick = 10 ms (LAPIC timer at 100 Hz).
#define BLK_KT_SHORT_SLEEP_TICKS  5u    // 50 ms — between mount-done polls.
#define BLK_KT_POLL_TIMEOUT_TICKS 3000u // 30 s — total wait for /sys/blk/service.
#define BLK_KT_PARK_SLEEP_TICKS   100u  // 1 s — sleep cadence when parked.

// Persistent kt-task-owned state.  Populated on successful handshake.  The
// channel pointers are also held alive by the kt task's cap_handles entry,
// so dropping the pointers here does not free the underlying channel_t.
// Step 3 will use these to issue blk_req_msg_t requests from the FS hot
// path; Step 2 only proves the bring-up path works.
static channel_t *g_blk_req_chan  = NULL;  // kt → ahcid (request stream)
static channel_t *g_blk_resp_chan = NULL;  // ahcid → kt (response stream)
static vmo_t     *g_blk_dma_vmo   = NULL;  // shared 256 KiB DMA buffer

// Phase 24a W5 — SPSC ring in shared 4 KiB VMO.  Mapped into both kernel
// (via HHDM, since VMO_CONTIGUOUS) and ahcid (via vmo_mmap).  Producer
// (kernel) writes req fields then atomic_store(ready=1, RELEASE) per slot.
// Consumer (ahcid) polls all 64 slots, processes ready=1, writes resp,
// atomic_store(done=1, RELEASE).  Producer reads done (acquire), reads
// resp, sets ready=0 (release) to mark slot reusable.  See
// kernel/fs/blk_proto.h::blk_spsc_slot_t.  When g_blk_spsc_ring != NULL
// the kernel skips chan_send on the request side; otherwise it falls
// back to legacy chan_send (compat with v1 ahcid builds).
static vmo_t            *g_blk_spsc_vmo  = NULL;  // shared 4 KiB ring VMO
static blk_spsc_slot_t  *g_blk_spsc_ring = NULL;  // HHDM kva pointer (64 slots)

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

// ===========================================================================
// Phase 23 Step 3: channel-mode waiter table + workers + response loop.
// ===========================================================================
//
// The DMA VMO is 256 KiB = 64 contiguous pages (VMO_CONTIGUOUS|VMO_PINNED).
// Each waiter slot owns one 4-KiB page (slot_idx == page_idx).  All current
// grahafs callers use count=1 sector (512 B) per request, leaving 3.5 KiB
// headroom per slot — sufficient for callers that grow up to 8 sectors per
// request.  Larger transfers are not supported in this iteration; callers
// must split them.
//
// Wait-queue model: per-slot one-element waiter list.  Caller blocks via
// sched_block_on_channel(&slot, CHAN_WAIT_READ, 5s, &slot.waiter_head).
// Response handler (the kt task itself, post-handshake) wakes via
// sched_wake_one_on_channel.
//
// Spinlock g_waiters_lock guards the in_use bitmap.  Status / bytes /
// waiter_head fields of each in-use slot are written by the response handler
// only AFTER the slot's request was sent — so no concurrent writers; no
// per-slot lock needed.

#define BLK_WAITER_SLOTS    64u
#define BLK_DMA_PAGE_SIZE   4096u   /* 1 slot = 1 page */
#define BLK_MAX_SECTORS     8u      /* 4 KiB / 512 B */

typedef struct blk_waiter {
    uint8_t  in_use;
    uint8_t  is_read;       /* 1 = READ (kbuf gets data on wake), 0 = W/F */
    uint8_t  _pad[2];
    uint32_t req_id;        /* matches blk_resp_msg_t.req_id */
    int32_t  status;        /* set by response handler before wake */
    uint32_t bytes;         /* set by response handler */
    struct task_struct *waiter_head;  /* one-element wait list */
    /* F1: lost-wakeup race fix.  Responder sets `completed=1` with RELEASE
     * ordering BEFORE calling sched_wake_one_on_channel.  Caller checks it
     * with ACQUIRE ordering BEFORE calling sched_block_on_channel and on
     * every wake/timeout iteration.  Decouples "completion" from "linked
     * into waiter list" so a wake that fires while waiter_head is still
     * NULL is recovered on the next periodic re-check. */
    volatile uint32_t completed;
} blk_waiter_t;

static blk_waiter_t g_waiters[BLK_WAITER_SLOTS];
static spinlock_t   g_waiters_lock = SPINLOCK_INITIALIZER("blk_waiters");
static volatile uint32_t g_next_req_id = 1u;  /* skip 0 = "uninitialized" */

// Allocate a free slot.  Returns slot index on success, -1 if exhausted.
// On success, writes a fresh request id to *out_req_id.
static int waiter_alloc(uint32_t *out_req_id) {
    spinlock_acquire(&g_waiters_lock);
    for (uint32_t i = 0; i < BLK_WAITER_SLOTS; i++) {
        if (!g_waiters[i].in_use) {
            uint32_t id = __atomic_fetch_add(&g_next_req_id, 1u, __ATOMIC_RELAXED);
            if (id == 0u) {
                id = __atomic_fetch_add(&g_next_req_id, 1u, __ATOMIC_RELAXED);
            }
            g_waiters[i].in_use      = 1;
            g_waiters[i].is_read     = 0;
            g_waiters[i].req_id      = id;
            g_waiters[i].status      = -5;  /* default -EIO */
            g_waiters[i].bytes       = 0;
            g_waiters[i].waiter_head = NULL;
            g_waiters[i].completed   = 0;   /* F1: cleared on alloc */
            *out_req_id              = id;
            spinlock_release(&g_waiters_lock);
            return (int)i;
        }
    }
    spinlock_release(&g_waiters_lock);
    return -1;
}

static void waiter_free(uint32_t slot) {
    if (slot >= BLK_WAITER_SLOTS) return;
    spinlock_acquire(&g_waiters_lock);
    g_waiters[slot].in_use = 0;
    g_waiters[slot].req_id = 0;
    spinlock_release(&g_waiters_lock);
}

// Resolve req_id → slot index.  Returns -1 if no live slot has that id.
static int waiter_find_by_id(uint32_t req_id) {
    spinlock_acquire(&g_waiters_lock);
    for (uint32_t i = 0; i < BLK_WAITER_SLOTS; i++) {
        if (g_waiters[i].in_use && g_waiters[i].req_id == req_id) {
            spinlock_release(&g_waiters_lock);
            return (int)i;
        }
    }
    spinlock_release(&g_waiters_lock);
    return -1;
}

// Kernel-virtual pointer to slot's DMA VMO sub-region.  NULL if the VMO
// isn't ready yet.  The DMA VMO is VMO_CONTIGUOUS so every page is in HHDM.
static uint8_t *blk_dma_kva(uint32_t slot) {
    if (!g_blk_dma_vmo) return NULL;
    if (slot >= BLK_WAITER_SLOTS) return NULL;
    uint64_t phys = vmo_get_phys(g_blk_dma_vmo, slot);
    if (phys == 0) return NULL;
    return (uint8_t *)(phys + g_hhdm_offset);
}

// Phase 24a W5: write a request to the SPSC ring.  Producer side.
// Caller has already allocated a waiter slot and (for WRITE) copied data
// into the DMA VMO[slot].  Returns 0 on successful post (always; the
// "send" is just a memory store + atomic flag).
//
// Memory ordering.  All req-field stores are ordered before the
// atomic_store(ready=1, RELEASE) which synchronizes-with ahcid's
// atomic_load(ready, ACQUIRE).  The mfence before the release atomic is
// belt-and-braces: x86 already has TSO so prior plain stores cannot be
// reordered past a release atomic, but explicit mfence makes the
// invariant visible at the source level.
static int blk_spsc_post_req(uint8_t op, uint8_t dev, uint64_t lba,
                             uint32_t count, uint32_t slot, uint32_t req_id) {
    if (!g_blk_spsc_ring || slot >= BLK_SPSC_RING_SLOTS) return -5;
    blk_spsc_slot_t *s = &g_blk_spsc_ring[slot];
    /* The slot may still have stale `done=1` from a prior request that
     * landed in this slot.  Producer is the SOLE writer of `done=0` (after
     * reading it as 1 and consuming the resp); reset here so the consumer
     * sees a clean slot. */
    s->done    = 0;
    s->req_id  = req_id;
    s->op      = op;
    s->dev     = dev;
    s->count   = (uint16_t)count;
    s->lba     = lba;
    s->timeout_ms = 5000u;
    s->status  = 0;
    s->bytes   = 0;
    /* Ensure all req-field stores complete before flipping ready=1.  On
     * x86 this is implicit via TSO, but the release atomic + mfence make
     * the contract source-level explicit. */
    asm volatile("mfence" ::: "memory");
    __atomic_store_n(&s->ready, 1u, __ATOMIC_RELEASE);
    return 0;
}

// Build + send a blk_req_msg_t on g_blk_req_chan.  Caller has already
// allocated a waiter slot and (for WRITE) copied data into the DMA VMO.
static int blk_chan_send_req(uint8_t op, uint8_t dev, uint64_t lba,
                             uint32_t count, uint32_t slot, uint32_t req_id) {
    if (!g_blk_req_chan) return -5;  /* -EIO */
    task_t *self = sched_get_current_task();
    if (!self) return -5;

    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type_hash  = g_blk_req_chan->type_hash;
    msg.header.inline_len = (uint16_t)sizeof(blk_req_msg_t);
    msg.header.nhandles   = 0;

    blk_req_msg_t *r = (blk_req_msg_t *)msg.inline_payload;
    r->req_id     = req_id;
    r->op         = op;
    r->dev        = dev;
    r->_pad       = 0;
    r->lba        = lba;
    r->count      = count;
    r->vmo_handle = 0;  /* informational; ahcid uses cli->dma_vmo_handle */
    r->vmo_offset = slot * BLK_DMA_PAGE_SIZE;
    r->timeout_ms = 5000u;

    /* 100 ms send timeout: bounded but generous.  ahcid drains the request
     * channel each main-loop iteration (5 ms IRQ-wait cap), so the ring
     * should never be full long enough to hit this.  Returning a timeout
     * here surfaces as -EIO to the caller. */
    return chan_send(g_blk_req_chan, self, &msg, 100ull * 1000 * 1000);
}

// Block on slot's wait queue.  Returns the slot's status (positive bytes
// or negative errno) — sched layer translates timeout/EPIPE.
//
// F1: lost-wakeup race fix via completion flag.  The naive
// "sched_block_on_channel(5s)" is racy: chan_send returns without parking
// the caller, so the responder can fire sched_wake_one_on_channel BEFORE
// the caller has linked itself into the waiter_head — the wake is lost
// and the caller blocks for the full 5s deadline.
//
// Pattern: caller checks `completed` (acquire) BEFORE block.  If already
// set, return immediately.  Otherwise block with a 100 ms periodic
// timeout; on every wake or timeout, re-test the flag.  The responder
// sets `completed=1` (release) BEFORE its wake call, so even when wake
// is lost the next iteration's check observes completion and returns.
//
// Cost in the no-lost-wake case: zero.  The wake fires, sched_block returns
// 0, the loop's flag check exits.  Latency under L1 (INT 49) is <1 µs.
//
// Cost in the lost-wake case: bounded at 100 ms (one periodic re-check).
static int blk_wait_response(uint32_t slot) {
    blk_waiter_t *w = &g_waiters[slot];

    /* Fast path: response already arrived between chan_send return and
     * here.  No need to block. */
    if (__atomic_load_n(&w->completed, __ATOMIC_ACQUIRE)) {
        return w->status;
    }

    /* 5-second outer deadline (matches blk_req_msg_t.timeout_ms).  Inner
     * 10 ms periodic block (1 tick) ensures we re-check the flag every
     * tick even if a wake was dropped — bounding lost-wake recovery to
     * a single tick.  Tick = 10 ms → 500 ticks = 5 s. */
    uint64_t deadline = g_timer_ticks + 500u;
    while (!__atomic_load_n(&w->completed, __ATOMIC_ACQUIRE)) {
        if (g_timer_ticks >= deadline) return -110;  /* -ETIMEDOUT */
        (void)sched_block_on_channel(w, CHAN_WAIT_READ,
                                     10ull * 1000 * 1000,  /* 10 ms = 1 tick */
                                     &w->waiter_head);
        /* Re-test flag whether the wake delivered or the inner timeout
         * expired.  EPIPE / channel-close cases are surfaced via status
         * (responder sets it before flag) — caller learns of error from
         * the slot status, not from sched_block's return code. */
    }
    return w->status;
}

// Phase 24a W3: batch-send up to BLK_BATCH_MAX requests in a single
// chan_send. The caller has already allocated `n` waiter slots and
// (for WRITE) memcpy'd data into each slot's DMA-VMO sub-region.
// Each entry in `slots` MUST correspond to its own waiter_alloc()
// result; req_ids must be the matching freshly-allocated values.
//
// Returns 0 on successful send, negative errno on transport failure.
// On failure, caller must waiter_free() every slot itself.
static int blk_chan_send_batch(uint8_t op, uint8_t dev,
                               const uint64_t *lbas, const uint32_t *counts,
                               const uint32_t *slots, const uint32_t *req_ids,
                               uint32_t n) {
    if (!g_blk_req_chan || n == 0u || n > BLK_BATCH_MAX) return -22;
    task_t *self = sched_get_current_task();
    if (!self) return -5;

    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type_hash  = g_blk_req_chan->type_hash;
    msg.header.inline_len = (uint16_t)sizeof(blk_batch_req_t);
    msg.header.nhandles   = 0;

    blk_batch_req_t *batch = (blk_batch_req_t *)msg.inline_payload;
    batch->kind  = BLK_KIND_BATCH_REQ;
    batch->count = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) {
        blk_req_msg_t *r = &batch->reqs[i];
        r->req_id     = req_ids[i];
        r->op         = op;
        r->dev        = dev;
        r->_pad       = 0;
        r->lba        = lbas[i];
        r->count      = counts[i];
        r->vmo_handle = 0;
        r->vmo_offset = slots[i] * BLK_DMA_PAGE_SIZE;
        r->timeout_ms = 5000u;
    }
    return chan_send(g_blk_req_chan, self, &msg, 100ull * 1000 * 1000);
}

// Phase 24a W3: batched READ. Submits up to BLK_BATCH_MAX (= 6) reads in
// one chan_send; ahcid issues all to AHCI HBA's PxCI in a single MMIO
// store so the HBA processes them in parallel (NCS=32 supports up to
// 32 concurrent). Each request still uses its own waiter slot + DMA VMO
// sub-region — completions can arrive out of order, demuxed by req_id.
//
// Returns the number of successfully-completed reads (0..n) or a
// negative errno on whole-batch transport failure. Per-req errors are
// reported via the corresponding kbuf being left untouched and the
// per-slot status (which the caller can NOT inspect directly here);
// callers that need per-op status should use the single-op blk_chan_read
// in a loop. For grahafs_compute_simhash (the original consumer) only
// the count is meaningful — it's a best-effort speculative read.
//
// Caller arrays MUST be at least n entries long. Each kbufs[i] is a
// destination buffer of counts[i] * 512 bytes.
static int blk_chan_read_batch(uint8_t dev,
                               const uint64_t *lbas,
                               const uint32_t *counts,
                               void *const *kbufs,
                               uint32_t n) {
    if (n == 0u || n > BLK_BATCH_MAX) return -22;
    for (uint32_t i = 0; i < n; i++) {
        if (counts[i] == 0u || counts[i] > BLK_MAX_SECTORS) return -22;
    }

    uint32_t slots[BLK_BATCH_MAX];
    uint32_t req_ids[BLK_BATCH_MAX];
    uint32_t allocated = 0;
    for (; allocated < n; allocated++) {
        int slot = waiter_alloc(&req_ids[allocated]);
        if (slot < 0) {
            for (uint32_t j = 0; j < allocated; j++) waiter_free(slots[j]);
            return -11;  /* -EAGAIN: waiter table exhausted mid-batch */
        }
        slots[allocated] = (uint32_t)slot;
        g_waiters[slot].is_read = 1;
    }

    int rc;
    if (g_blk_spsc_ring) {
        /* W5: post each request to its own ring slot.  No chan_send.
         * ahcid will see N ready slots in one main-loop iteration and
         * issue them to the AHCI HBA in parallel via PxCI (NCS=32). */
        for (uint32_t i = 0; i < n; i++) {
            (void)blk_spsc_post_req(BLK_OP_READ, dev, lbas[i], counts[i],
                                    slots[i], req_ids[i]);
        }
        rc = 0;
    } else {
        rc = blk_chan_send_batch(BLK_OP_READ, dev,
                                 lbas, counts, slots, req_ids, n);
    }
    if (rc < 0) {
        for (uint32_t j = 0; j < n; j++) waiter_free(slots[j]);
        return rc;
    }

    /* Wait per-slot. Completions can arrive out of order — each slot
     * has its own waiter_head + completion flag (F1 pattern). */
    int completed = 0;
    for (uint32_t i = 0; i < n; i++) {
        int wrc = blk_wait_response(slots[i]);
        if (wrc == 0) {
            uint8_t *dma = blk_dma_kva(slots[i]);
            if (dma) {
                asm volatile("mfence" ::: "memory");
                memcpy(kbufs[i], dma, (size_t)counts[i] * 512u);
                completed++;
            }
        }
        waiter_free(slots[i]);
        __atomic_add_fetch(&g_blk.request_count, 1u, __ATOMIC_RELAXED);
        if (wrc < 0) {
            __atomic_add_fetch(&g_blk.error_count, 1u, __ATOMIC_RELAXED);
        }
    }
    return completed;
}

// READ via channel mode.  Caller-side memcpy-out happens AFTER ahcid has
// DMA'd the data into our shared VMO (so reads are correct as long as
// CPU/DMA coherency is intact, which on x86 + WB cache is the case post
// the response wake's mfence implied by sched_lock release).
//
// Phase 24a W5: when g_blk_spsc_ring is set, post via the ring (no
// chan_send).  Otherwise fall back to the legacy chan_send path (compat
// with v1 ahcid builds + initial-bringup paths where the ring isn't
// allocated yet).
static int blk_chan_read(uint8_t dev, uint64_t lba, uint32_t count, void *kbuf) {
    if (count == 0u || count > BLK_MAX_SECTORS) return -22;
    uint32_t req_id = 0;
    int slot = waiter_alloc(&req_id);
    if (slot < 0) return -11;  /* -EAGAIN: 64-slot table exhausted */
    g_waiters[slot].is_read = 1;

    int rc;
    if (g_blk_spsc_ring) {
        rc = blk_spsc_post_req(BLK_OP_READ, dev, lba, count,
                               (uint32_t)slot, req_id);
    } else {
        rc = blk_chan_send_req(BLK_OP_READ, dev, lba, count,
                               (uint32_t)slot, req_id);
    }
    if (rc < 0) {
        waiter_free((uint32_t)slot);
        return rc;
    }

    int wrc = blk_wait_response((uint32_t)slot);
    int retval;
    if (wrc == 0) {
        uint8_t *dma = blk_dma_kva((uint32_t)slot);
        if (!dma) {
            retval = -5;
        } else {
            asm volatile("mfence" ::: "memory");
            memcpy(kbuf, dma, (size_t)count * 512u);
            retval = (int)count;
        }
    } else {
        retval = wrc;
    }

    waiter_free((uint32_t)slot);
    __atomic_add_fetch(&g_blk.request_count, 1u, __ATOMIC_RELAXED);
    if (retval < 0) {
        __atomic_add_fetch(&g_blk.error_count, 1u, __ATOMIC_RELAXED);
    }
    return retval;
}

// WRITE via channel mode.  Copy kbuf into the DMA VMO sub-region, mfence,
// then send.  ahcid's DMA reads from the VMO's physical pages; the mfence
// ensures the CPU's writeback cache state is ordered before the
// hardware-visible write.
static int blk_chan_write(uint8_t dev, uint64_t lba, uint32_t count,
                          const void *kbuf) {
    if (count == 0u || count > BLK_MAX_SECTORS) return -22;
    uint32_t req_id = 0;
    int slot = waiter_alloc(&req_id);
    if (slot < 0) return -11;
    g_waiters[slot].is_read = 0;

    uint8_t *dma = blk_dma_kva((uint32_t)slot);
    if (!dma) {
        waiter_free((uint32_t)slot);
        return -5;
    }
    memcpy(dma, kbuf, (size_t)count * 512u);
    asm volatile("mfence" ::: "memory");

    int rc;
    if (g_blk_spsc_ring) {
        rc = blk_spsc_post_req(BLK_OP_WRITE, dev, lba, count,
                               (uint32_t)slot, req_id);
    } else {
        rc = blk_chan_send_req(BLK_OP_WRITE, dev, lba, count,
                               (uint32_t)slot, req_id);
    }
    if (rc < 0) {
        waiter_free((uint32_t)slot);
        return rc;
    }

    int wrc = blk_wait_response((uint32_t)slot);
    int retval = (wrc == 0) ? (int)count : wrc;

    waiter_free((uint32_t)slot);
    __atomic_add_fetch(&g_blk.request_count, 1u, __ATOMIC_RELAXED);
    if (retval < 0) {
        __atomic_add_fetch(&g_blk.error_count, 1u, __ATOMIC_RELAXED);
    }
    return retval;
}

// FLUSH via channel mode.  No DMA, no data — just a tagged round-trip.
static int blk_chan_flush(uint8_t dev) {
    uint32_t req_id = 0;
    int slot = waiter_alloc(&req_id);
    if (slot < 0) return -11;

    int rc;
    if (g_blk_spsc_ring) {
        rc = blk_spsc_post_req(BLK_OP_FLUSH, dev, 0, 0,
                               (uint32_t)slot, req_id);
    } else {
        rc = blk_chan_send_req(BLK_OP_FLUSH, dev, 0, 0,
                               (uint32_t)slot, req_id);
    }
    if (rc < 0) {
        waiter_free((uint32_t)slot);
        return rc;
    }

    int wrc = blk_wait_response((uint32_t)slot);
    waiter_free((uint32_t)slot);
    __atomic_add_fetch(&g_blk.request_count, 1u, __ATOMIC_RELAXED);
    if (wrc < 0) {
        __atomic_add_fetch(&g_blk.error_count, 1u, __ATOMIC_RELAXED);
    }
    return wrc;
}

// --- Lifecycle ----------------------------------------------------------
void blk_client_init(void) {
    spinlock_acquire(&g_blk.lock);
    // Phase 24a W10: kernel-direct fallback stripped.  Initial state is
    // DISCONNECTED; the kt task transitions through CONNECTING → READY
    // as /bin/ahcid publishes /sys/blk/service and the BLK_PROTO
    // handshake completes.
    g_blk.state    = BLK_CLIENT_DISCONNECTED;
    g_blk.fs_state = BLK_FS_OK;
    spinlock_release(&g_blk.lock);
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client: init complete (channel-mode only; ahcid will bring up)");
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

// Resolve the dispatch state.  If state is CONNECTING (kt task is bringing
// ahcid up), spin-wait until READY or ERROR — bounded at 30 seconds.
// Returns the final state to dispatch on.  Used by all four wrappers.
static blk_client_state_t blk_resolve_dispatch_state(void) {
    blk_client_state_t st = blk_client_state();
    if (st != BLK_CLIENT_CONNECTING) return st;
    /* Spin-wait.  Each iter is one timer tick (10 ms).  30-second deadline
     * accounts for slow ahcid startup (port_reset, IDENTIFY) plus generous
     * margin.  After deadline, return whatever state we ended in — caller
     * will see CONNECTING and likely return -EAGAIN. */
    uint64_t deadline = g_timer_ticks + 3000u;
    while (g_timer_ticks < deadline) {
        st = blk_client_state();
        if (st != BLK_CLIENT_CONNECTING) return st;
        asm volatile("sti; hlt" ::: "memory");
    }
    return blk_client_state();
}

int grahafs_block_read(uint8_t dev, uint64_t lba, uint32_t count, void *kbuf) {
    if (!kbuf) return -22;
    if (count == 0 || count > 0xFFFFu) return -22;
    blk_client_state_t st = blk_resolve_dispatch_state();
    if (st == BLK_CLIENT_ERROR)        return -5;   /* -EIO */
    if (st == BLK_CLIENT_DISCONNECTED) return -11;  /* -EAGAIN */
    if (st == BLK_CLIENT_CONNECTING)   return -11;  /* still racing */

    /* Phase 24a W10: channel-mode is the ONLY path.  state==READY is
     * required; the kernel-direct fallback was stripped along with
     * arch/x86_64/drivers/ahci/ahci.c::ahci_read. */
    return blk_chan_read(dev, lba, count, kbuf);
}

// Phase 24a W3: public batched-read wrapper.  Phase 24a W10 strip removed
// the kernel-direct fallback — all paths route through the channel-mode
// SPSC ring once state==READY.
int grahafs_block_read_batch(uint8_t dev,
                             const uint64_t *lbas,
                             const uint32_t *counts,
                             void *const *kbufs,
                             uint32_t n) {
    if (!lbas || !counts || !kbufs) return -22;
    if (n == 0u || n > BLK_BATCH_MAX) return -22;

    blk_client_state_t st = blk_resolve_dispatch_state();
    if (st == BLK_CLIENT_ERROR)        return -5;
    if (st == BLK_CLIENT_DISCONNECTED) return -11;
    if (st == BLK_CLIENT_CONNECTING)   return -11;

    return blk_chan_read_batch(dev, lbas, counts, kbufs, n);
}

int grahafs_block_write(uint8_t dev, uint64_t lba, uint32_t count, const void *kbuf) {
    if (!kbuf) return -22;
    if (count == 0 || count > 0xFFFFu) return -22;
    if (blk_fs_state() == BLK_FS_READ_ONLY_ERROR) return -30; /* -EROFS */
    blk_client_state_t st = blk_resolve_dispatch_state();
    if (st == BLK_CLIENT_ERROR)        return -5;
    if (st == BLK_CLIENT_DISCONNECTED) return -11;
    if (st == BLK_CLIENT_CONNECTING)   return -11;

    return blk_chan_write(dev, lba, count, kbuf);
}

// FU29.H — v2 4096-byte logical-block I/O: scale block→sector (×8) and
// transfer a full 8-sector (4 KiB) block, matching v1's grahafs.c convention.
// Returns 1 on full success (==1 contract), <0 on error.
int grahafs_v2_block_read(uint8_t dev, uint64_t block, void *buf4096) {
    if (!buf4096) return -22;
    int rc = grahafs_block_read(dev, block * 8u, 8u, buf4096);
    return (rc == 8) ? 1 : (rc < 0 ? rc : -5);
}

int grahafs_v2_block_write(uint8_t dev, uint64_t block, const void *buf4096) {
    if (!buf4096) return -22;
    int rc = grahafs_block_write(dev, block * 8u, 8u, buf4096);
    return (rc == 8) ? 1 : (rc < 0 ? rc : -5);
}

int grahafs_block_flush(uint8_t dev) {
    if (blk_fs_state() == BLK_FS_READ_ONLY_ERROR) return -30;
    blk_client_state_t st = blk_resolve_dispatch_state();
    if (st == BLK_CLIENT_ERROR)        return -5;
    if (st == BLK_CLIENT_DISCONNECTED) return -11;
    if (st == BLK_CLIENT_CONNECTING)   return -11;

    return blk_chan_flush(dev);
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

// ===========================================================================
// Phase 23 Step 2: kt task — channel-mode bring-up.
// ===========================================================================
//
// The kt task is responsible for the cutover handshake between the kernel
// blk_client and userspace ahcid.  It runs as a regular kernel task
// (sched_create_task), executing in kernel mode with full pledges.
//
// Mode discipline:
//   - autorun=ktest: the gate runs FS-touching tests (gcp_manifest, fstest_v2,
//     simtest, …) that depend on kernel-direct AHCI.  Bringing up ahcid here
//     would corrupt PxCLB/PxFB and break those tests.  The kt task parks
//     immediately.  Step 3 will widen activation here once the strip lands.
//   - autorun=init  (or default): /etc/init.conf spawns /bin/ahcid as a
//     userspace daemon early.  ahcid claims the AHCI HBA via drv_register
//     and publishes /sys/blk/service.  The kt task polls for that name,
//     connects, and completes the BLK_PROTO handshake.  After the handshake
//     state transitions to BLK_CLIENT_READY but the existing wrappers
//     continue to use kernel-direct (Step 3 adds dispatch).
//
// The kt task never blocks the boot path — it lives alongside autorun_decide
// and works opportunistically.
// ---------------------------------------------------------------------------

// Cooperative sleep: spin-yield until `n` LAPIC ticks have elapsed.
// Each tick is ~10 ms.  Uses sti;hlt to give other tasks the CPU instead of
// burning cycles in a tight loop.
static void kt_sleep_ticks(uint32_t n) {
    uint64_t deadline = g_timer_ticks + (uint64_t)n;
    while (g_timer_ticks < deadline) {
        asm volatile ("sti; hlt" ::: "memory");
    }
}

// Detect whether we booted with autorun=ktest.  In that mode init.conf is
// not consumed (ktest itself is PID 1) so /bin/ahcid never spawns.
static bool kt_is_ktest_mode(void) {
    const char *a = g_cmdline_flags.autorun;
    if (!a) return false;  // default = bin/init
    // Cheap hand-rolled equality test ("ktest").
    return a[0] == 'k' && a[1] == 't' && a[2] == 'e' &&
           a[3] == 's' && a[4] == 't' && a[5] == '\0';
}

// Fail-soft helper: log + park.  Used when bring-up runs into an unrecoverable
// step (rawnet_connect rc<0, vmo_create rc<0, …).  Phase 24a W10: the
// kernel-direct fallback was stripped, so park always transitions state to
// ERROR — wrappers will return -EIO instead of spinning forever.
static void kt_park_forever(const char *why) {
    klog(KLOG_WARN, SUBSYS_CORE,
         "blk_client_kt: parked (%s)", why ? why : "unspecified");
    spinlock_acquire(&g_blk.lock);
    g_blk.state = BLK_CLIENT_ERROR;
    spinlock_release(&g_blk.lock);
    __atomic_store_n(&g_blk_kt_settled, 1u, __ATOMIC_RELEASE);
    while (1) kt_sleep_ticks(BLK_KT_PARK_SLEEP_TICKS);
}

// Kernel-context spawn of /bin/ahcid for ktest mode.  /etc/init.conf is
// only consumed by /bin/init; in autorun=ktest the gate runs ktest as PID 1
// and ahcid would never come up otherwise.  parent_id = self->id makes the
// kt task ahcid's parent (sched_spawn_process tolerates this); pledges are
// inherited from the kt task (which has full pledges as a kernel task).
// Returns the new pid on success or negative on failure.
static int kt_spawn_ahcid(task_t *self) {
    int pid = sched_spawn_process("bin/ahcid", self ? self->id : -1);
    if (pid < 0) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: sched_spawn_process(bin/ahcid) rc=%d", pid);
        return pid;
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: spawned /bin/ahcid pid=%d (kernel-context)", pid);
    return pid;
}

// Build the BLK_PROTO handshake message and ship it on the request channel.
// The DMA VMO is transferred as a CHANNEL HANDLE (chan_msg.in_flight_idx[0])
// rather than embedded in the inline payload, matching ahcid's
// try_accept_new_client expectations (recv_payload_h returns the receiver-
// generated cap_token for nh>=1 in handles[0]).
//
// Phase 24a W5: optionally pass the SPSC ring VMO as in_flight_idx[1].
// inline_len is sizeof(blk_connect_msg_v2_t) when the SPSC field is set,
// matching the v2 schema in blk_proto.h.  Older ahcid (v1) reads only the
// first 24 bytes and ignores the SPSC fields; newer ahcid reads 32 bytes
// and uses spsc_vmo to map the ring.
static int kt_send_handshake(task_t *self, channel_t *req_chan,
                             uint32_t dma_obj_idx, uint32_t dma_obj_gen,
                             uint64_t dma_size_bytes,
                             uint32_t spsc_obj_idx, uint32_t spsc_obj_gen) {
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type_hash  = req_chan->type_hash;
    msg.header.inline_len = (uint16_t)sizeof(blk_connect_msg_v2_t);
    msg.header.nhandles   = (spsc_obj_idx != 0u) ? 2u : 1u;
    msg.in_flight_idx[0]  = dma_obj_idx;
    msg.in_flight_idx[1]  = spsc_obj_idx;  /* 0 if no SPSC ring */

    blk_connect_msg_v2_t *cm = (blk_connect_msg_v2_t *)msg.inline_payload;
    cm->magic        = BLK_PROTO_MAGIC;
    cm->version      = (spsc_obj_idx != 0u) ? BLK_PROTO_VERSION_V2
                                            : BLK_PROTO_VERSION;
    /* The 32-bit `dma_vmo` field is informational only; ahcid reads the
     * real cap_token from handles[0] of the channel message (full 64 bits,
     * including generation).  See ahcid.c:try_accept_new_client comment. */
    cm->dma_vmo      = dma_obj_idx;
    cm->dma_vmo_size = (uint32_t)dma_size_bytes;
    cm->resp_chan    = 0;  /* kt holds the read end locally; ahcid replies
                            * via its own write end (transferred at connect). */
    cm->spsc_vmo     = spsc_obj_idx;     /* 0 = legacy chan_send path */
    cm->spsc_size    = (spsc_obj_idx != 0u) ? (uint32_t)BLK_SPSC_RING_BYTES : 0u;
    (void)dma_obj_gen;
    (void)spsc_obj_gen;

    /* chan_send does NOT auto-transfer handles out of the sender's table —
     * the syscall path's chan_marshal_send normally does that.  We must
     * remove the handle ourselves before send so it's not double-owned
     * after the receiver inserts it. */
    for (uint32_t s = 0; s < self->cap_handles.capacity; s++) {
        cap_handle_entry_t *e = cap_handle_lookup(&self->cap_handles, s);
        if (e && (e->object_idx == dma_obj_idx ||
                  (spsc_obj_idx != 0u && e->object_idx == spsc_obj_idx))) {
            cap_handle_remove(&self->cap_handles, s);
            /* Don't break — we may have two handles to remove. */
        }
    }

    return chan_send(req_chan, self, &msg, 0);
}

// Phase 24a W10: FS-init worker.  Runs as a separate kernel thread spawned
// by blk_client_kt_entry once channel-mode is READY.  Performs the mount
// + audit attach + init-process spawn.  Must NOT be in the kt task itself
// because the kt task is the channel-response consumer — if it were the
// requester too, mount's blk_chan_read would deadlock (request goes out,
// response queues, but kt is blocked in mount and never reaches its
// chan_recv loop).  Solution: separate task → kt receives responses →
// wakes the fs-init task to complete mount.
void blk_client_fs_init_task_entry(void) {
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_fs_init: entry (channel-mode mount)");

    /* Wait for kt task to bring channel-mode up.  kt sets g_blk_kt_settled
     * just before its response loop starts.  Sleep-poll keeps us cooperative
     * with the scheduler. */
    while (__atomic_load_n(&g_blk_kt_settled, __ATOMIC_ACQUIRE) == 0u ||
           __atomic_load_n(&g_blk.state,      __ATOMIC_ACQUIRE) != BLK_CLIENT_READY) {
        kt_sleep_ticks(BLK_KT_SHORT_SLEEP_TICKS);
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_fs_init: state=READY observed; mounting FS");

    extern int grahafs_v2_mount(int device_id);
    extern struct vfs_node *grahafs_v2_build_root_node(void);
    extern void vfs_set_root(struct vfs_node *root);
    extern void gc_init(void);
    extern void recluster_init(void);
    extern struct vfs_node *grahafs_mount(struct block_device *device);
    extern void audit_attach_fs(void);

    int v2_rc = grahafs_v2_mount(0);
    struct vfs_node *root = NULL;
    if (v2_rc == 0) {
        root = grahafs_v2_build_root_node();
        if (root) {
            vfs_set_root(root);
            gc_init();
            recluster_init();
            klog(KLOG_INFO, SUBSYS_CORE,
                 "blk_client_fs_init: grahafs v2 mounted via channel mode");
        }
    } else {
        klog(KLOG_INFO, SUBSYS_CORE,
             "blk_client_fs_init: v2 mount rc=%d — falling back to v1", v2_rc);
        static struct block_device s_synth_dev = {
            .in_use       = true,
            .device_id    = 0,
            .block_size   = 512,
            .read_blocks  = NULL,
            .write_blocks = NULL,
        };
        root = grahafs_mount(&s_synth_dev);
        if (root) {
            vfs_set_root(root);
            klog(KLOG_INFO, SUBSYS_CORE,
                 "blk_client_fs_init: grahafs v1 mounted via channel mode");
        }
    }
    if (!root) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_fs_init: FS mount FAILED — gate will be degraded "
             "(initrd-only fallback)");
    }
    audit_attach_fs();
    __atomic_store_n(&g_blk_mount_done, 1u, __ATOMIC_RELEASE);
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_fs_init: g_blk_mount_done=1 — about to spawn init");

    // Spawn the init process (ktest in gate, /bin/init in interactive boot).
    {
        extern const char *autorun_decide(void);
        extern void autorun_register_init_pid(int pid);
        task_t *self = sched_get_current_task();
        const char *init_path = autorun_decide();
        int init_pid = sched_spawn_process(init_path, self ? self->id : -1);
        if (init_pid < 0) {
            klog(KLOG_ERROR, SUBSYS_CORE,
                 "blk_client_fs_init: failed to spawn init %s rc=%d",
                 init_path, init_pid);
        } else {
            autorun_register_init_pid(init_pid);
            klog(KLOG_INFO, SUBSYS_CORE,
                 "blk_client_fs_init: spawned init %s pid=%d",
                 init_path, init_pid);
        }
    }

    // Park: nothing more to do.
    while (1) kt_sleep_ticks(BLK_KT_PARK_SLEEP_TICKS);
}

// kt task entry: full bring-up dance.  Logs profusely so a serial-trace
// bisect can pinpoint which stage fails on a regression.
static void blk_client_kt_entry(void) {
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: entry (Phase 24a W10 — channel-mode is the only path)");

    // Phase 24a W10: kmain no longer mounts FS synchronously, so kt task
    // is responsible for the full bring-up: spawn ahcid (kernel-context)
    // in BOTH ktest and init modes, complete the BLK_PROTO handshake,
    // then mount FS.  kmain blocks on g_blk_mount_done (set at the end
    // of this function) before spawning user processes.
    task_t *self = sched_get_current_task();
    if (!self) kt_park_forever("sched_get_current_task returned NULL");

    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: kernel-spawning /bin/ahcid "
         "(W10 strip — kernel-direct AHCI removed; channel-mode is the only "
         "path)");
    int pid = kt_spawn_ahcid(self);
    if (pid < 0) kt_park_forever("kt_spawn_ahcid failed");

    /* /etc/init.conf is responsible for spawning /bin/ahcid before the kt
     * task makes progress in init mode; ahcid's port_init reprograms
     * PxCLB/PxFB so kernel-direct AHCI is no longer safe.  Transitioning
     * state to CONNECTING tells the wrappers to stop using kernel-direct
     * and spin-wait for READY. */
    spinlock_acquire(&g_blk.lock);
    if (g_blk.state == BLK_CLIENT_DISCONNECTED) {
        g_blk.state = BLK_CLIENT_CONNECTING;
    }
    spinlock_release(&g_blk.lock);
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: state=CONNECTING (channel-mode bring-up)");

    // Phase 3: poll for /sys/blk/service publication.  In init mode the
    // /etc/init.conf supervisor (executed by /bin/init as PID 1) launches
    // /bin/ahcid early; ahcid then publishes /sys/blk/service.  We poll
    // up to BLK_KT_POLL_TIMEOUT_TICKS (~30 s) before giving up.
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: polling for /sys/blk/service publication");
    bool published = false;
    uint64_t deadline = g_timer_ticks + (uint64_t)BLK_KT_POLL_TIMEOUT_TICKS;
    while (g_timer_ticks < deadline) {
        if (rawnet_name_exists("/sys/blk/service",
                               (uint32_t)sizeof("/sys/blk/service") - 1)) {
            published = true;
            break;
        }
        kt_sleep_ticks(BLK_KT_SHORT_SLEEP_TICKS);
    }
    if (!published) {
        kt_park_forever("/sys/blk/service did not publish in 30 s");
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: /sys/blk/service published; connecting");

    // Phase 4: connect via rawnet.  This mints a per-connection channel
    // pair (request + response) and transfers the server-side handles to
    // ahcid via the accept channel.  The kt task ends up with the client-
    // side write end (request) and read end (response).  `self` was
    // resolved at Phase 2.
    cap_token_t wr_req_tok  = {.raw = 0};
    cap_token_t rd_resp_tok = {.raw = 0};
    int rc = rawnet_connect(self->id, "/sys/blk/service",
                            (uint32_t)sizeof("/sys/blk/service") - 1,
                            &wr_req_tok, &rd_resp_tok);
    if (rc != 0) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: rawnet_connect rc=%d", rc);
        kt_park_forever("rawnet_connect failed");
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: rawnet_connect ok; wr_req=0x%lx rd_resp=0x%lx",
         (unsigned long)wr_req_tok.raw, (unsigned long)rd_resp_tok.raw);

    spinlock_acquire(&g_blk.lock);
    g_blk.state = BLK_CLIENT_CONNECTING;
    spinlock_release(&g_blk.lock);

    // Phase 5: allocate the shared DMA VMO (256 KiB, contiguous, pinned).
    // PID_PUBLIC audience so ahcid can resolve it post-handle-transfer.
    vmo_t *dma = vmo_create(256ull * 1024,
                            VMO_CONTIGUOUS | VMO_PINNED | VMO_ZEROED,
                            self->id, PID_PUBLIC);
    if (!dma) kt_park_forever("vmo_create(256K) failed");

    int32_t aud[CAP_AUDIENCE_MAX + 1] = { PID_PUBLIC, PID_NONE };
    int dma_idx = cap_object_create(CAP_KIND_VMO,
                                    RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                        RIGHT_DERIVE,
                                    aud, 0, (uintptr_t)dma, self->id,
                                    CAP_OBJECT_IDX_NONE);
    if (dma_idx < 0) {
        vmo_free(dma);
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: cap_object_create(VMO) rc=%d", dma_idx);
        kt_park_forever("cap_object_create(VMO) failed");
    }
    dma->cap_object_idx = (uint32_t)dma_idx;

    uint32_t dma_slot = 0;
    int rc_ins = cap_handle_insert(&self->cap_handles, (uint32_t)dma_idx, 0,
                                    &dma_slot);
    if (rc_ins < 0) {
        cap_object_destroy((uint32_t)dma_idx);
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: cap_handle_insert(VMO) rc=%d", rc_ins);
        kt_park_forever("cap_handle_insert(VMO) failed");
    }
    cap_object_t *dma_obj = g_cap_object_ptrs[dma_idx];
    uint32_t dma_gen = dma_obj
                      ? __atomic_load_n(&dma_obj->generation, __ATOMIC_ACQUIRE)
                      : 0;
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: DMA VMO ready idx=%u gen=%u 256 KiB contiguous",
         (unsigned)dma_idx, (unsigned)dma_gen);

    // Phase 5b (W5): allocate the SPSC ring VMO (4 KiB, 1 page, contiguous,
    // pinned, zeroed).  Fail-soft: if any step fails, fall back to the
    // legacy chan_send request path (spsc_idx stays 0 in handshake).
    int      spsc_idx = 0;     /* cap_object_t index; 0 == none */
    uint32_t spsc_gen = 0;
    vmo_t   *spsc = vmo_create(BLK_SPSC_RING_BYTES,
                               VMO_CONTIGUOUS | VMO_PINNED | VMO_ZEROED,
                               self->id, PID_PUBLIC);
    if (spsc) {
        int sidx = cap_object_create(CAP_KIND_VMO,
                                     RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                         RIGHT_DERIVE,
                                     aud, 0, (uintptr_t)spsc, self->id,
                                     CAP_OBJECT_IDX_NONE);
        if (sidx >= 0) {
            spsc->cap_object_idx = (uint32_t)sidx;
            uint32_t spsc_slot = 0;
            int rc_si = cap_handle_insert(&self->cap_handles, (uint32_t)sidx, 0,
                                          &spsc_slot);
            if (rc_si >= 0) {
                cap_object_t *spsc_obj = g_cap_object_ptrs[sidx];
                spsc_gen = spsc_obj
                          ? __atomic_load_n(&spsc_obj->generation, __ATOMIC_ACQUIRE)
                          : 0;
                spsc_idx = sidx;
                /* Resolve kernel-virt pointer via HHDM (VMO_CONTIGUOUS). */
                uint64_t spsc_phys = vmo_get_phys(spsc, 0);
                if (spsc_phys != 0) {
                    g_blk_spsc_vmo  = spsc;
                    g_blk_spsc_ring = (blk_spsc_slot_t *)(spsc_phys + g_hhdm_offset);
                    klog(KLOG_INFO, SUBSYS_CORE,
                         "blk_client_kt: SPSC VMO ready idx=%u gen=%u 4 KiB ring",
                         (unsigned)sidx, (unsigned)spsc_gen);
                } else {
                    klog(KLOG_WARN, SUBSYS_CORE,
                         "blk_client_kt: vmo_get_phys(spsc) returned 0 — falling back to chan_send");
                    cap_object_destroy((uint32_t)sidx);
                    spsc_idx = 0;
                }
            } else {
                klog(KLOG_WARN, SUBSYS_CORE,
                     "blk_client_kt: cap_handle_insert(spsc) rc=%d — falling back",
                     rc_si);
                cap_object_destroy((uint32_t)sidx);
                spsc_idx = 0;
            }
        } else {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "blk_client_kt: cap_object_create(spsc VMO) rc=%d — falling back",
                 sidx);
            vmo_free(spsc);
        }
    } else {
        klog(KLOG_WARN, SUBSYS_CORE,
             "blk_client_kt: vmo_create(SPSC 4 KiB) failed — falling back to chan_send");
    }

    // Phase 6: resolve the request channel endpoint and ship the handshake.
    channel_t *req_chan = NULL;
    uint32_t req_chan_obj_idx = 0;
    rc = chan_resolve_endpoint(self->id, wr_req_tok, CHAN_ENDPOINT_WRITE, 0,
                               &req_chan, &req_chan_obj_idx);
    if (rc != 0 || !req_chan) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: chan_resolve(wr_req) rc=%d", rc);
        kt_park_forever("chan_resolve(wr_req) failed");
    }

    rc = kt_send_handshake(self, req_chan, (uint32_t)dma_idx, dma_gen,
                           256ull * 1024,
                           (uint32_t)spsc_idx, spsc_gen);
    if (rc != 0) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: chan_send(handshake) rc=%d", rc);
        kt_park_forever("chan_send(handshake) failed");
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: BLK_PROTO handshake sent; transitioning to READY");

    // Resolve the response endpoint.  Step 3 dispatches blk_resp_msg_t on
    // this; failure here means we cannot consume responses, so we must
    // park instead of pretending to be READY.
    channel_t *resp_chan = NULL;
    uint32_t   resp_chan_obj_idx = 0;
    rc = chan_resolve_endpoint(self->id, rd_resp_tok, CHAN_ENDPOINT_READ, 0,
                               &resp_chan, &resp_chan_obj_idx);
    if (rc != 0 || !resp_chan) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_kt: chan_resolve(rd_resp) rc=%d", rc);
        kt_park_forever("chan_resolve(rd_resp) failed");
    }

    // Stash the surviving state for the channel-mode wrappers + response
    // loop.  Pointer writes are single-writer (this task); the lock around
    // state transition synchronises against concurrent blk_client_state()
    // readers.
    g_blk_dma_vmo   = dma;
    g_blk_req_chan  = req_chan;
    g_blk_resp_chan = resp_chan;
    spinlock_acquire(&g_blk.lock);
    g_blk.state = BLK_CLIENT_READY;
    spinlock_release(&g_blk.lock);
    /* Settle BEFORE first chan_recv so kmain's wait-for-settled returns
     * promptly.  Channel-mode I/O is now usable from any kernel context. */
    __atomic_store_n(&g_blk_kt_settled, 1u, __ATOMIC_RELEASE);
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: state=READY  channel-mode I/O active");

    // Phase 24a W10: spawn the FS-init worker task on a separate kernel
    // thread.  The mount + init-spawn work cannot run in this kt task
    // because the response loop below MUST be reachable to drain ahcid's
    // replies — otherwise blk_chan_read deadlocks (kt sends request, kt
    // is itself the consumer of the response, kt is blocked in mount).
    extern void blk_client_fs_init_task_entry(void);
    int fs_init_pid = sched_create_task(blk_client_fs_init_task_entry);
    if (fs_init_pid < 0) {
        kt_park_forever("sched_create_task(fs_init) failed");
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: spawned fs-init worker pid=%d", fs_init_pid);

    // Phase 8: response handler loop.  Block on g_blk_resp_chan; each
    // received blk_resp_msg_t is demuxed by req_id to wake the calling
    // task in blk_chan_{read,write,flush}.
    //
    // Phase 24a W5 Phase 2: the response side now also has a SPSC ring path.
    // ahcid writes status+bytes to the slot, atomic_store(done=1, RELEASE),
    // and emits a coalesced 1-byte chan_send doorbell per client per IRQ
    // batch. On every recv we first scan the ring for done=1 slots (regardless
    // of inline_len), then fall through to the legacy single/batch path for
    // any v1 ahcid or dispatch-error inline responses.
    while (1) {
        channel_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        /* timeout=UINT64_MAX → block forever (chan_recv treats 0 as
         * non-blocking, returning -EAGAIN immediately).  This loop wakes
         * when ahcid sends a blk_resp_msg_t (or a 1-byte W5.2 doorbell). */
        int rrc = chan_recv(g_blk_resp_chan, self, &msg,
                            0xFFFFFFFFFFFFFFFFull);
        if (rrc < 0) {
            /* Channel closed (ahcid died) or other transport error.  Drop
             * a klog and back off — blk_client_on_ahcid_death will mark
             * the FS read-only; on respawn the kt task either reconnects
             * (future work) or stays parked.  For now, log + retry. */
            klog(KLOG_WARN, SUBSYS_CORE,
                 "blk_client_kt: chan_recv rc=%d — backing off", rrc);
            kt_sleep_ticks(BLK_KT_PARK_SLEEP_TICKS);
            continue;
        }

        /* W5 Phase 2: scan the SPSC ring for done=1 slots (single producer
         * = ahcid handle_irq; single consumer = this loop). For each:
         * acquire-load done, read status/bytes, dispatch via the F1
         * completion-flag pattern, release-store done=0 to mark consumed.
         * Coalesced doorbells mean one recv may yield N completions. */
        if (g_blk_spsc_ring) {
            for (uint32_t i = 0; i < BLK_SPSC_RING_SLOTS; i++) {
                blk_spsc_slot_t *rs = &g_blk_spsc_ring[i];
                if (__atomic_load_n(&rs->done, __ATOMIC_ACQUIRE) == 0u) continue;
                int32_t  st    = rs->status;
                uint32_t bytes = rs->bytes;
                uint32_t req_id = rs->req_id;
                /* Mark consumed BEFORE the wake. Single consumer means no
                 * other scanner can re-dispatch; producer (ahcid) won't
                 * re-set done=1 until next request lands and completes. */
                __atomic_store_n(&rs->done, 0u, __ATOMIC_RELEASE);
                int slot = waiter_find_by_id(req_id);
                if (slot < 0) {
                    klog(KLOG_WARN, SUBSYS_CORE,
                         "blk_client_kt: stale spsc resp req_id=%u status=%d",
                         (unsigned)req_id, (int)st);
                    continue;
                }
                /* F1 ordering: status/bytes first, completed=1 RELEASE,
                 * then wake. The acquire-load above synchronizes with
                 * ahcid's release-store of done=1 (which followed mfence
                 * + plain stores of status/bytes). */
                g_waiters[slot].status = st;
                g_waiters[slot].bytes  = bytes;
                __atomic_store_n(&g_waiters[slot].completed, 1u,
                                 __ATOMIC_RELEASE);
                sched_wake_one_on_channel(&g_waiters[slot].waiter_head, 0);
            }
        }

        if (msg.header.inline_len < sizeof(blk_resp_msg_t)) {
            /* Short payload — typically a W5.2 1-byte doorbell. The ring
             * scan above already processed any pending completions. */
            continue;
        }

        /* Phase 24a W3: dispatch on inline_len + kind tag.
         *
         *   inline_len == sizeof(blk_resp_msg_t) (= 24)  → single response (legacy path)
         *   inline_len == sizeof(blk_batch_resp_t) (= 152) AND
         *     kind == BLK_KIND_BATCH_RESP                 → batch response with N entries
         *
         * Each entry (single or batch member) follows the F1 ordering:
         * status/bytes first, atomic-store completed=1 with RELEASE,
         * then wake. Multiple wakes from a batch translate to multiple
         * sched_wake_one_on_channel calls; each is independent.
         */
        if (msg.header.inline_len == sizeof(blk_batch_resp_t) &&
            msg.inline_payload[0] == BLK_KIND_BATCH_RESP) {
            const blk_batch_resp_t *br =
                (const blk_batch_resp_t *)msg.inline_payload;
            uint8_t n = br->count;
            if (n == 0u || n > BLK_BATCH_MAX) {
                klog(KLOG_WARN, SUBSYS_CORE,
                     "blk_client_kt: bad batch_resp count=%u", (unsigned)n);
                continue;
            }
            for (uint8_t i = 0; i < n; i++) {
                const blk_resp_msg_t *r = &br->resps[i];
                int slot = waiter_find_by_id(r->req_id);
                if (slot < 0) {
                    klog(KLOG_WARN, SUBSYS_CORE,
                         "blk_client_kt: stale batch resp req_id=%u status=%d",
                         (unsigned)r->req_id, (int)r->status);
                    continue;
                }
                g_waiters[slot].status = r->status;
                g_waiters[slot].bytes  = r->bytes_transferred;
                __atomic_store_n(&g_waiters[slot].completed, 1u, __ATOMIC_RELEASE);
                sched_wake_one_on_channel(&g_waiters[slot].waiter_head, 0);
            }
            continue;
        }

        /* Single-response path (legacy + default). */
        blk_resp_msg_t resp;
        memcpy(&resp, msg.inline_payload, sizeof(resp));
        int slot = waiter_find_by_id(resp.req_id);
        if (slot < 0) {
            /* Late or stale response — caller may have timed out and
             * already freed the slot.  Drop. */
            klog(KLOG_WARN, SUBSYS_CORE,
                 "blk_client_kt: stale resp req_id=%u status=%d",
                 (unsigned)resp.req_id, (int)resp.status);
            continue;
        }
        /* F1 ordering: write status/bytes first, then atomic-store the
         * `completed` flag with RELEASE ordering, THEN wake.  This sequence
         * closes the lost-wakeup race: even if waiter_head is still NULL
         * (caller hasn't reached sched_block_on_channel yet), the caller's
         * next iteration of the periodic re-check loop will observe
         * completed=1 (acquire) and exit without blocking.  The release
         * store ensures status/bytes are visible to the acquire load. */
        g_waiters[slot].status = resp.status;
        g_waiters[slot].bytes  = resp.bytes_transferred;
        __atomic_store_n(&g_waiters[slot].completed, 1u, __ATOMIC_RELEASE);
        sched_wake_one_on_channel(&g_waiters[slot].waiter_head, 0);
    }
}

void blk_client_start_kt(void) {
    if (g_blk_kt_pid != 0) return;  // idempotent
    int pid = sched_create_task(blk_client_kt_entry);
    if (pid < 0) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "blk_client_start_kt: sched_create_task failed");
        return;
    }
    g_blk_kt_pid = pid;
    klog(KLOG_INFO, SUBSYS_CORE,
         "blk_client_kt: spawned (kt pid=%d)", pid);
}
