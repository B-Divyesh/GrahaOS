// user/drivers/ahcid.h — Phase 23 S3.
//
// Userspace AHCI driver daemon — internal header. Mirrors the structures
// from the (Phase 22) in-kernel ahci.h so the programming model is
// identical. Differences from the kernel header:
//   - All MMIO accesses go through the mapped VMO pointer (libdriver-managed).
//   - DMA buffers are sys_vmo_create(VMO_CONTIGUOUS) allocations whose
//     physical addresses are discovered via syscall_vmo_phys.
//   - Single-threaded event loop; CI register access serialised by
//     construction (no locking needed).
//   - Operations carried as blk_req_msg_t over channels (see blk_proto.h).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../syscalls.h"
#include "../../kernel/fs/blk_proto.h"

// PCI class/subclass for AHCI HBAs.
#define AHCID_PCI_CLASS_STORAGE  0x01
#define AHCID_PCI_SUBCLASS_SATA  0x06
#define AHCID_VENDOR_INTEL       0x8086
#define AHCID_DEVICE_ICH9        0x2922

// AHCI port-state constants (mirror the kernel).
#define AHCID_PORT_DEV_PRESENT   0x3
#define AHCID_PORT_IPM_ACTIVE    0x1
#define AHCID_SATA_SIG_ATA       0x00000101u

#define AHCID_PxCMD_ST   0x0001u
#define AHCID_PxCMD_FRE  0x0010u
#define AHCID_PxCMD_FR   0x4000u
#define AHCID_PxCMD_CR   0x8000u

// HBA Capabilities (CAP register) bit fields.
#define AHCID_CAP_NCS_SHIFT  8
#define AHCID_CAP_NCS_MASK   0x1Fu  /* Number of command slots - 1 */
#define AHCID_CAP_NP_SHIFT   0
#define AHCID_CAP_NP_MASK    0x1Fu  /* Number of ports - 1 */

// GHC register.
#define AHCID_GHC_AE  (1u << 31)  /* AHCI Enable */
#define AHCID_GHC_IE  (1u << 1)   /* Interrupt Enable */

// BOHC register.
#define AHCID_BOHC_BOS  (1u << 0)  /* BIOS Owned Semaphore */
#define AHCID_BOHC_OOS  (1u << 1)  /* OS Owned Semaphore */

// ATA commands.
#define AHCID_ATA_READ_DMA_EXT    0x25
#define AHCID_ATA_WRITE_DMA_EXT   0x35
#define AHCID_ATA_FLUSH_CACHE     0xE7
#define AHCID_ATA_FLUSH_CACHE_EXT 0xEA
#define AHCID_ATA_IDENTIFY        0xEC

// Port reset timing (per AHCI 1.3 spec).
#define AHCID_PORT_RESET_HOLD_TICKS   1000  /* ~1 ms via spin_ticks */
#define AHCID_PORT_RESET_POLL_BUDGET  200   /* 1 s @ 5 ms intervals */

// HBA generic host control registers (offset 0x00, 256 bytes total).
typedef volatile struct {
    uint32_t cap;       // 0x00 Host Capabilities
    uint32_t ghc;       // 0x04 Global Host Control
    uint32_t is;        // 0x08 Interrupt Status
    uint32_t pi;        // 0x0C Ports Implemented
    uint32_t vs;        // 0x10 Version
    uint32_t ccc_ctl;   // 0x14
    uint32_t ccc_pts;   // 0x18
    uint32_t em_loc;    // 0x1C
    uint32_t em_ctl;    // 0x20
    uint32_t cap2;      // 0x24
    uint32_t bohc;      // 0x28
    uint8_t  rsv[0x60-0x2C];
    uint8_t  vendor[0x100-0x60];
    // Port specific registers start at 0x100, 0x80 each (32 ports max).
} __attribute__((packed)) ahcid_hba_mem_t;

// Per-port registers at HBA + 0x100 + (port_idx * 0x80).
typedef volatile struct {
    uint64_t clb;       // 0x00 Command List Base
    uint64_t fb;        // 0x08 FIS Base
    uint32_t is;        // 0x10 Interrupt Status
    uint32_t ie;        // 0x14 Interrupt Enable
    uint32_t cmd;       // 0x18 Command and Status
    uint32_t rsv0;      // 0x1C
    uint32_t tfd;       // 0x20 Task File Data
    uint32_t sig;       // 0x24 Signature
    uint32_t ssts;      // 0x28 SATA Status
    uint32_t sctl;      // 0x2C SATA Control
    uint32_t serr;      // 0x30 SATA Error
    uint32_t sact;      // 0x34 SATA Active
    uint32_t ci;        // 0x38 Command Issue
    uint32_t sntf;      // 0x3C SATA Notification
    uint32_t fbs;       // 0x40 FIS-based Switching
    uint8_t  rsv1[0x70-0x44];
    uint8_t  vendor[0x80-0x70];
} __attribute__((packed)) ahcid_port_mmio_t;

// Command Header (32 B). 32 of these per port, in the command list.
typedef struct {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint64_t ctba;
    uint32_t rsv1[4];
} __attribute__((packed)) ahcid_cmd_header_t;

// PRDT Entry (16 B).
typedef struct {
    uint64_t dba;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} __attribute__((packed)) ahcid_prdt_entry_t;

// Command Table (4 KB). Single PRDT entry sized; we don't need >1 PRD per
// request because each blk_req references at most 64 KB (PRD's max).
typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    ahcid_prdt_entry_t prdt_entry[8];  /* up to 8 entries */
} __attribute__((packed)) ahcid_cmd_table_t;

// Host-to-Device FIS.
typedef struct {
    uint8_t  fis_type;  // 0x27
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} __attribute__((packed)) ahcid_fis_h2d_t;

// Per-port runtime state.
typedef struct {
    uint8_t  present;            /* 1 = drive attached, identified */
    uint8_t  port_idx;
    uint8_t  _pad[2];
    ahcid_port_mmio_t *mmio;     /* pointer into the mapped HBA region */
    void   *cmd_list_va;         /* command list (1 KB) */
    uint64_t cmd_list_phys;
    void   *fis_va;              /* FIS receive buffer (256 B) */
    uint64_t fis_phys;
    void   *cmd_table_va[32];    /* command tables (4 KB each) */
    uint64_t cmd_table_phys[32];
    uint64_t sector_count;       /* Total LBAs from IDENTIFY */
    uint16_t sector_size;        /* Almost always 512 */
    uint8_t  identify[512];      /* Cached IDENTIFY DEVICE result */
    /* Per-slot waiter (single-threaded so no lock needed). */
    struct {
        uint8_t   in_use;
        uint8_t   _pad[3];
        uint32_t  req_id;
        uint64_t  resp_chan;     /* Channel handle (full 64-bit cap_token) */
        uint64_t  start_tsc;
        uint32_t  bytes;
    } slot[32];
} ahcid_port_state_t;

// Top-level state for a connected client (kernel blk_client + tests).
// Phase 23 closeout fix: cap_token_raw_t is uint64_t. The client_chan_*
// fields and dma_vmo_handle MUST hold the full 64-bit token (idx + flags +
// generation) — truncating to uint32_t loses generation, which the kernel
// then rejects on the next syscall as a stale-handle mismatch.
typedef struct {
    uint8_t  in_use;
    uint8_t  _pad[3];
    uint64_t client_chan_read;   /* We receive blk_req here (full 64-bit) */
    uint64_t client_chan_write;  /* We send blk_resp here (full 64-bit) */
    uint64_t dma_vmo_handle;     /* Caller's shared DMA VMO (full 64-bit) */
    int32_t  client_pid;         /* For audit + cleanup */
    uint32_t reqs_handled;
} ahcid_client_t;

#define AHCID_MAX_CLIENTS  16
#define AHCID_MAX_PORTS    32

// Global daemon context.
typedef struct {
    drv_caps_t          caps;
    ahcid_hba_mem_t    *hba;
    uint32_t            ncs;          /* number of command slots (1..32) */
    uint32_t            np;           /* number of ports (1..32) */
    uint32_t            pi;           /* ports implemented bitmap */
    ahcid_port_state_t  ports[AHCID_MAX_PORTS];
    uint32_t            port_count;
    ahcid_client_t      clients[AHCID_MAX_CLIENTS];
    uint64_t            irq_count;
    uint64_t            requests_total;
    uint64_t            errors_total;
} ahcid_state_t;

extern ahcid_state_t g_ahcid;

// --- Public function table ---------------------------------------------
int  ahcid_register_pci(void);
int  ahcid_map_mmio(void);
int  ahcid_take_bios_ownership(void);
int  ahcid_enable_ahci(void);
int  ahcid_enumerate_ports(void);
int  ahcid_port_init(uint8_t port_idx);
int  ahcid_identify_device(uint8_t port_idx);
int  ahcid_publish_service(void);
void ahcid_main_loop(void);

// Op dispatch.
int  ahcid_do_read(ahcid_client_t *cli, const blk_req_msg_t *req);
int  ahcid_do_write(ahcid_client_t *cli, const blk_req_msg_t *req);
int  ahcid_do_flush(ahcid_client_t *cli, const blk_req_msg_t *req);
int  ahcid_do_identify(ahcid_client_t *cli, const blk_req_msg_t *req);

// Error recovery.
int  ahcid_port_reset(uint8_t port_idx);
