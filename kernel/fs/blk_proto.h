// kernel/fs/blk_proto.h — Phase 23 S2.
//
// Shared block-I/O protocol header. Inserted on the channel between the
// kernel-side blk_client (the only kernel consumer of disk I/O) and the
// userspace ahcid daemon. Kernel and userspace MUST see identical struct
// layouts — userspace ahcid (user/drivers/ahcid.c) #includes this file
// directly via a relative path.
//
// Wire format: little-endian, packed, fixed-size. blk_req_msg_t is exactly
// 32 bytes; blk_resp_msg_t is exactly 24 bytes. Both fit comfortably in the
// channel inline-payload window (256 B) so no out-of-line VMOs are sent for
// the message itself — the data is carried in a separate shared DMA VMO
// referenced by handle.
//
// Channel pairing:
//   - service channel: kernel→ahcid request stream (kernel sends blk_req_msg_t).
//   - response channel: ahcid→kernel reply stream (ahcid sends blk_resp_msg_t).
//   - shared DMA VMO: 256 KB, kernel allocates VMO_CONTIGUOUS|VMO_PINNED,
//     mapped uncached on both sides; sub-region (waiter slot index × 4 KB)
//     is the bounce buffer for one in-flight request.
#pragma once

#include <stdint.h>

// --- Operations ---------------------------------------------------------
// Match the kernel's `enum` style — values are wire-stable.
#define BLK_OP_READ      0u  /* Hardware READ_DMA_EXT */
#define BLK_OP_WRITE     1u  /* Hardware WRITE_DMA_EXT */
#define BLK_OP_FLUSH     2u  /* Hardware FLUSH_CACHE_EXT */
#define BLK_OP_IDENTIFY  3u  /* Cached IDENTIFY DEVICE result */

// --- Request -------------------------------------------------------------
// 32 bytes. Used by blk_client to initiate one block-I/O request.
typedef struct __attribute__((packed)) blk_req_msg {
    uint32_t req_id;       //  0..3   Caller-assigned id; echoed in response.
    uint8_t  op;           //  4      BLK_OP_*
    uint8_t  dev;          //  5      Device index (0..port_count-1)
    uint16_t _pad;         //  6..7   Reserved, must be 0
    uint64_t lba;          //  8..15  Sector LBA (BLK_OP_READ/WRITE)
    uint32_t count;        // 16..19  Sector count (<=128, i.e. 64 KB max)
    uint32_t vmo_handle;   // 20..23  Shared DMA VMO handle in caller's table
    uint32_t vmo_offset;   // 24..27  Offset within VMO; 512-byte aligned
    uint32_t timeout_ms;   // 28..31  Driver-side timeout
} blk_req_msg_t;
_Static_assert(sizeof(blk_req_msg_t) == 32, "blk_req_msg_t must be 32 bytes");

// --- Response ------------------------------------------------------------
// 24 bytes. Sent by ahcid on completion.
typedef struct __attribute__((packed)) blk_resp_msg {
    uint32_t req_id;            //  0..3   Echoed from request
    int32_t  status;            //  4..7   0 = success, negative errno
    uint32_t bytes_transferred; //  8..11  count*512 on success
    uint32_t _pad;              // 12..15  Reserved
    uint64_t timestamp_tsc;     // 16..23  ahcid completion TSC; latency stat
} blk_resp_msg_t;
_Static_assert(sizeof(blk_resp_msg_t) == 24, "blk_resp_msg_t must be 24 bytes");

// --- Connect handshake ---------------------------------------------------
// First message sent by blk_client over /sys/blk/service: lets ahcid know
// the kernel-side DMA VMO handle to use for all subsequent requests.
typedef struct __attribute__((packed)) blk_connect_msg {
    uint32_t magic;        //  0..3   0x424C4B43 ("BLKC")
    uint32_t version;      //  4..7   Protocol version (1)
    uint32_t dma_vmo;      //  8..11  Shared DMA VMO handle
    uint32_t dma_vmo_size; // 12..15  Bytes (kernel allocates 256 KB)
    uint32_t resp_chan;    // 16..19  Response channel handle (ahcid sends back here)
    uint32_t _pad;         // 20..23
} blk_connect_msg_t;
_Static_assert(sizeof(blk_connect_msg_t) == 24, "blk_connect_msg_t must be 24 bytes");

#define BLK_PROTO_MAGIC    0x424C4B43u  /* 'BLKC' */
#define BLK_PROTO_VERSION  1u

// --- Channel type names (FNV-1a 64-bit hashed at compile time on both sides) -
#define BLK_SERVICE_TYPE  "grahaos.blk.service.v1"
#define BLK_LIST_TYPE     "grahaos.blk.list.v1"

// --- Errors --------------------------------------------------------------
// Mirror the kernel's negative-errno convention. blk_resp_msg_t.status
// uses these.
#define BLK_E_OK         0
#define BLK_E_INVAL      -22  /* lba out of range, unaligned offset, bad op */
#define BLK_E_IO         -5   /* hardware error, port reset failure */
#define BLK_E_NODEV      -19  /* dev index has no device */
#define BLK_E_TIMEOUT    -110 /* operation exceeded timeout_ms */
#define BLK_E_ACCES      -13  /* VMO handle not owned by caller */
#define BLK_E_PIPE       -32  /* ahcid disconnected mid-request */

// =========================================================================
// Phase 24a W3 — Batched FS Ops (Layer 3 of the IPC optimization stack).
// =========================================================================
//
// Bundle up to BLK_BATCH_MAX requests in one chan_send. Each request still
// gets its own waiter slot, req_id, and DMA-VMO sub-region; ahcid issues
// all N to the AHCI HBA's PxCI register in a single MMIO store, so the
// HBA processes them in parallel (NCS=32 supports up to 32 concurrent).
// Multiplicative speedup with W1 doorbell IPI + W2 chan_send fastpath:
//   - 12-block compute_simhash read: 12 chan_sends → 2 chan_sends (6×).
//   - Sequential 4 KiB read stream: amortizes per-op chan_send overhead.
//
// Sizing: channel inline-payload window is CHAN_MSG_INLINE_MAX = 256 B.
// blk_batch_req_t at 6 reqs = 8 (header) + 6×32 (reqs) = 200 B. Fits
// with 56 B slack. blk_batch_resp_t at 6 resps = 8 + 6×24 = 152 B.
// Choosing 6 over 8 (the original spec target) avoids needing to bump
// the channel ABI; if a future iteration wants bigger batches, the
// natural transport is the shared DMA VMO (W5 SPSC ring lands that).
//
// Wire kind tag (BLK_KIND_*): the first byte of every blk_*_msg sent on
// the kernel↔ahcid channels. Lets the receiver demux single vs batch
// without a separate type_hash.
#define BLK_BATCH_MAX        6u

#define BLK_KIND_SINGLE_REQ  0x01u  /* legacy / non-batched: blk_req_msg_t  */
#define BLK_KIND_SINGLE_RESP 0x02u  /* legacy / non-batched: blk_resp_msg_t */
#define BLK_KIND_BATCH_REQ   0x03u  /* batched: blk_batch_req_t             */
#define BLK_KIND_BATCH_RESP  0x04u  /* batched: blk_batch_resp_t            */

// blk_batch_req_t — up to BLK_BATCH_MAX requests in one channel message.
// `kind` MUST be BLK_KIND_BATCH_REQ. `count` is the number of valid reqs[]
// entries (1..BLK_BATCH_MAX). Each reqs[i] has its own req_id; ahcid
// returns N responses (one per req_id) in a single blk_batch_resp_t.
typedef struct __attribute__((packed)) blk_batch_req {
    uint8_t  kind;                            //  0     BLK_KIND_BATCH_REQ
    uint8_t  count;                           //  1     1..BLK_BATCH_MAX
    uint8_t  _pad[6];                         //  2..7  alignment
    blk_req_msg_t reqs[BLK_BATCH_MAX];        //  8..199 (32 × 6 = 192)
} blk_batch_req_t;
_Static_assert(sizeof(blk_batch_req_t) == 200,
               "blk_batch_req_t must be 200 bytes (fits in 256 inline)");
_Static_assert(sizeof(blk_batch_req_t) <= 256,
               "blk_batch_req_t must fit in CHAN_MSG_INLINE_MAX");

// blk_batch_resp_t — paired completion envelope. resps[i].req_id matches
// some reqs[j].req_id from the request batch (not necessarily i==j; ahcid
// may complete commands out of order, so consumers MUST demux by req_id).
typedef struct __attribute__((packed)) blk_batch_resp {
    uint8_t  kind;                             //  0     BLK_KIND_BATCH_RESP
    uint8_t  count;                            //  1     1..BLK_BATCH_MAX
    uint8_t  _pad[6];                          //  2..7  alignment
    blk_resp_msg_t resps[BLK_BATCH_MAX];       //  8..151 (24 × 6 = 144)
} blk_batch_resp_t;
_Static_assert(sizeof(blk_batch_resp_t) == 152,
               "blk_batch_resp_t must be 152 bytes (fits in 256 inline)");
_Static_assert(sizeof(blk_batch_resp_t) <= 256,
               "blk_batch_resp_t must fit in CHAN_MSG_INLINE_MAX");

// =========================================================================
// Phase 24a W5 — SPSC Ring in Shared VMO (Layer 4 of the IPC stack).
// =========================================================================
//
// Per-slot ring stored in a shared VMO mapped into both kernel and ahcid.
// Replaces the per-op chan_send on the request side. The kernel producer
// writes one slot's req fields, then atomically sets ready=1 (release).
// The ahcid consumer polls all slots, reads ready (acquire), processes the
// request, writes the resp fields, then atomically sets done=1 (release).
// The kernel reads done (acquire), reads the resp fields, then sets
// ready=0 (release) to mark the slot reusable.
//
// Why slot-indexed (not head/tail).  We already have a 64-slot waiter table
// (kernel/fs/blk_client.c::g_waiters); reusing the slot index means a
// single waiter_alloc-ed slot is the index into BOTH the DMA VMO AND the
// SPSC ring. No queueing, no ordering — completions are naturally out-of-
// order and demuxed by req_id (already supported by W3 batching).
//
// Layout: 64 slots × 64 bytes = 4 KB = exactly one page. Each slot is
// cacheline-aligned to avoid producer-consumer ping-pong across slots.
// All allocation+pinning is via vmo_create(VMO_CONTIGUOUS|VMO_PINNED) so
// kernel can access via HHDM and ahcid via vmo_mmap.
//
// Wake mechanism.  The plan calls for "no chan_send on producer side";
// ahcid's main loop polls the ring with a 1 ms drv_irq_wait timeout, so
// fresh requests are picked up within 1 ms of posting (best-case <1 us
// when ahcid is in its loop body anyway). The response side keeps the
// existing chan_send doorbell (1 byte payload) — kt task receives, scans
// done=1 slots, wakes their waiters via the existing F1 completion-flag
// pattern. Net win: ~1 chan_send saved per op (the request side).
typedef struct __attribute__((aligned(64))) blk_spsc_slot {
    /* Synchronization flags. Producer (kernel) sets ready (release) AFTER
     * writing req fields. Consumer (ahcid) reads ready (acquire) BEFORE
     * reading req fields. After processing, consumer writes resp fields
     * THEN sets done (release). Producer reads done (acquire), reads resp,
     * then sets ready=0 (release) — slot is reusable. */
    volatile uint32_t ready;   //  0..3   0 = empty/done-consumed, 1 = req posted
    volatile uint32_t done;    //  4..7   0 = pending, 1 = resp posted
    /* Producer-written req fields. */
    uint32_t req_id;           //  8..11  matches blk_req_msg_t.req_id
    uint8_t  op;               // 12      BLK_OP_*
    uint8_t  dev;              // 13
    uint16_t count;            // 14..15  sectors
    uint64_t lba;              // 16..23
    uint32_t timeout_ms;       // 24..27
    uint32_t _req_pad;         // 28..31
    /* Consumer-written resp fields. */
    int32_t  status;           // 32..35  0 or negative errno
    uint32_t bytes;            // 36..39
    /* Reserved + pad to 64 bytes (cacheline). */
    uint8_t  _pad[24];         // 40..63
} blk_spsc_slot_t;
_Static_assert(sizeof(blk_spsc_slot_t) == 64,
               "blk_spsc_slot_t must be exactly 64 bytes (one cacheline)");

#define BLK_SPSC_RING_SLOTS  64u
#define BLK_SPSC_RING_BYTES  (BLK_SPSC_RING_SLOTS * sizeof(blk_spsc_slot_t))
_Static_assert(BLK_SPSC_RING_BYTES == 4096,
               "BLK_SPSC_RING must fit in exactly one 4 KiB page");

// Extended connect message used when SPSC ring is in tree (W5).  The kernel
// passes both DMA and SPSC VMO handles in a single connect frame.  Older
// ahcid builds that only know about blk_connect_msg_t will read just the
// first 24 bytes; newer ahcid builds check the inline_len and read the
// full 32 bytes if present.  Wire-stable: spsc_vmo == 0 means "no SPSC
// ring; ahcid should use the legacy chan_send request path".
typedef struct __attribute__((packed)) blk_connect_msg_v2 {
    uint32_t magic;        //  0..3   BLK_PROTO_MAGIC
    uint32_t version;      //  4..7   BLK_PROTO_VERSION_V2 (= 2)
    uint32_t dma_vmo;      //  8..11  DMA VMO handle (slot[0..63] of 4 KB)
    uint32_t dma_vmo_size; // 12..15  Bytes (256 KiB)
    uint32_t resp_chan;    // 16..19  Response channel handle
    uint32_t spsc_vmo;     // 20..23  SPSC ring VMO handle (0 = legacy)
    uint32_t spsc_size;    // 24..27  4096 (BLK_SPSC_RING_BYTES)
    uint32_t _pad;         // 28..31
} blk_connect_msg_v2_t;
_Static_assert(sizeof(blk_connect_msg_v2_t) == 32,
               "blk_connect_msg_v2_t must be 32 bytes");

#define BLK_PROTO_VERSION_V2  2u  /* SPSC-ring-aware connect frame */
