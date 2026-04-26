// user/drivers/e1000d.h
//
// Phase 21.1 — Userspace E1000 (Intel 82540EM) NIC driver. Lifted from the
// kernel-resident driver at arch/x86_64/drivers/e1000/e1000.{c,h} (which is
// now stripped to ~70 LOC of PCI-detect + userdrv exposure). All MMIO,
// EEPROM, descriptor-ring, and IRQ logic now lives in this binary, talking
// to the kernel through three channels handed out by sys_drv_register:
//
//   - irq_channel      (kernel→daemon, SPSC ring of drv_irq_msg_t entries)
//   - downstream chan  (daemon→kernel, carries ANNOUNCE + RX_NOTIFY + LINK_*)
//   - upstream chan    (kernel→daemon, carries TX_NOTIFY)
//
// Frames never travel inline through any channel — that would require
// CHAN_MSG_INLINE_MAX > 1518 (today it's 256). Instead the daemon allocates
// two contiguous DMA VMOs (RX ring + TX ring, 16 slots × 4 KiB each, total
// 128 KiB) via libdriver's drv_dma_alloc, and sends the physical addresses
// to the kernel proxy in the ANNOUNCE message at startup. The proxy reaches
// the slots through HHDM (kernel virt = phys + g_hhdm_offset). Channel
// messages then carry only (op, slot_idx, length) tuples — under 32 bytes.

#pragma once
#include <stdint.h>

// ====================================================================
// PCI identification (must match arch/x86_64/drivers/e1000/e1000.h).
// ====================================================================
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E   // 82540EM (QEMU default)

// ====================================================================
// MMIO register offsets (lifted verbatim from the old kernel driver).
// ====================================================================
#define E1000_CTRL          0x00000
#define E1000_STATUS        0x00008
#define E1000_EERD          0x00014
#define E1000_ICR           0x000C0
#define E1000_IMS           0x000D0
#define E1000_IMC           0x000D8
#define E1000_RCTL          0x00100
#define E1000_TCTL          0x00400
#define E1000_TIPG          0x00410
#define E1000_RDBAL         0x02800
#define E1000_RDBAH         0x02804
#define E1000_RDLEN         0x02808
#define E1000_RDH           0x02810
#define E1000_RDT           0x02818
#define E1000_TDBAL         0x03800
#define E1000_TDBAH         0x03804
#define E1000_TDLEN         0x03808
#define E1000_TDH           0x03810
#define E1000_TDT           0x03818
#define E1000_RAL           0x05400
#define E1000_RAH           0x05404
#define E1000_MTA_BASE      0x05200

// CTRL bits.
#define E1000_CTRL_FD       (1u << 0)
#define E1000_CTRL_ASDE     (1u << 5)
#define E1000_CTRL_SLU      (1u << 6)
#define E1000_CTRL_RST      (1u << 26)

// STATUS bits.
#define E1000_STATUS_LU     (1u << 1)

// RCTL bits.
#define E1000_RCTL_EN       (1u << 1)
#define E1000_RCTL_BAM      (1u << 15)
#define E1000_RCTL_BSIZE_2K (0u << 16)
#define E1000_RCTL_SECRC    (1u << 26)

// TCTL bits.
#define E1000_TCTL_EN       (1u << 1)
#define E1000_TCTL_PSP      (1u << 3)

// TX descriptor command bits.
#define E1000_TXD_CMD_EOP   (1u << 0)
#define E1000_TXD_CMD_RS    (1u << 3)

// Descriptor status bits.
#define E1000_DESC_DD       (1u << 0)

// EERD bits.
#define E1000_EERD_START    (1u << 0)
#define E1000_EERD_DONE     (1u << 4)

// IMS bits.
#define E1000_IMS_TXDW      (1u << 0)
#define E1000_IMS_TXQE      (1u << 1)
#define E1000_IMS_LSC       (1u << 2)
#define E1000_IMS_RXDMT0    (1u << 4)
#define E1000_IMS_RXSEQ     (1u << 3)
#define E1000_IMS_RXT0      (1u << 7)

// Ring configuration.
#define E1000_NUM_RX_DESC   256
#define E1000_NUM_TX_DESC   256
#define E1000_BUF_SIZE      2048

// Legacy 16-byte descriptors (must be packed).
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

// ====================================================================
// Channel message schema. Shared with kernel/net/e1000_proxy.c (the
// proxy keeps a byte-compatible copy with a _Static_assert).
//
// Op codes:
//   ANNOUNCE   — sent ONCE by daemon right after sys_drv_register returns.
//                Carries: MAC address, initial link state, rx_ring_phys,
//                tx_ring_phys, slot_count, slot_size.
//   RX_NOTIFY  — sent by daemon for every received frame. Carries:
//                slot index in rx_ring + frame length.
//   TX_NOTIFY  — sent by proxy for every outgoing frame. Carries:
//                slot index in tx_ring + frame length.
//   LINK_UP    — sent by daemon when STATUS.LU goes 0 → 1.
//   LINK_DOWN  — sent by daemon when STATUS.LU goes 1 → 0.
// ====================================================================
#define E1000_OP_ANNOUNCE     1u
#define E1000_OP_RX_NOTIFY    2u
#define E1000_OP_TX_NOTIFY    3u
#define E1000_OP_LINK_UP      4u
#define E1000_OP_LINK_DOWN    5u

#define E1000D_RING_SLOTS     16u
#define E1000D_SLOT_SIZE      4096u

typedef struct __attribute__((packed)) e1000_msg {
    uint8_t  op;             // E1000_OP_*
    uint8_t  _pad0[3];
    uint32_t slot;           // For RX/TX_NOTIFY
    uint32_t length;         // For RX/TX_NOTIFY (frame length)
    uint8_t  mac[6];         // For ANNOUNCE
    uint8_t  link_up;        // For ANNOUNCE
    uint8_t  _pad1;
    uint64_t rx_ring_phys;   // For ANNOUNCE
    uint64_t tx_ring_phys;   // For ANNOUNCE
    uint32_t slot_count;     // For ANNOUNCE
    uint32_t slot_size;      // For ANNOUNCE
} e1000_msg_t;

_Static_assert(sizeof(e1000_msg_t) == 44, "e1000_msg_t layout drift");
