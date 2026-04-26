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
