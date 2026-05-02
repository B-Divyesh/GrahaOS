// arch/x86_64/drivers/ahci/ahci.h — Phase 24a W10 stripped header.
//
// Pre-strip: 152 LOC including ahci_cmd_header_t, ahci_prdt_entry_t,
// ahci_cmd_table_t, fis_reg_h2d_t, ATA constants, and prototypes for
// ahci_read / ahci_write / ahci_flush_cache / ahci_activate / etc.
// All of those moved to user/drivers/ahcid.c.
//
// What remains: HBA generic-host-control + per-port register layout
// (still needed by ahci_init's PCI walk + L10 restore), and the three
// surviving entry points.
#pragma once
#include <stdint.h>
#include <stddef.h>

// AHCI HBA Memory Registers (Generic Host Control).
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
    uint32_t bohc;      // 0x28 BIOS/OS Handoff Control
    uint8_t  rsv[0x60-0x2C];
    uint8_t  vendor[0x100-0x60];
    // Port-specific registers start at 0x100.
} __attribute__((packed)) ahci_hba_mem_t;

// AHCI Port Registers.
typedef volatile struct {
    uint64_t clb;       // 0x00 Command List Base
    uint64_t fb;        // 0x08 FIS Base
    uint32_t is;        // 0x10 Interrupt Status
    uint32_t ie;        // 0x14 Interrupt Enable
    uint32_t cmd;       // 0x18 Command + Status
    uint32_t rsv0;
    uint32_t tfd;       // 0x20 Task File Data
    uint32_t sig;       // 0x24 Signature
    uint32_t ssts;      // 0x28 SCR0 SStatus
    uint32_t sctl;      // 0x2C SCR2 SControl
    uint32_t serr;      // 0x30 SCR1 SError
    uint32_t sact;      // 0x34 SCR3 SActive
    uint32_t ci;        // 0x38 Command Issue
    uint32_t sntf;      // 0x3C SCR4 SNotification
    uint32_t fbs;       // 0x40 FIS-based Switching
    uint8_t  rsv1[0x70-0x44];
    uint8_t  vendor[0x80-0x70];
} __attribute__((packed)) ahci_port_t;

// Initialise the kernel's view of the AHCI HBA: PCI scan, BAR5 map, BIOS
// hand-off, GHC.AE, snapshot per-port CLB/FB pointers.  Does NOT register
// a VFS device or a CAN cap — those duties moved to /bin/ahcid.
void ahci_init(void);

// Mark the AHCI HBA as claimable by /bin/ahcid via SYS_DRV_REGISTER.
void ahci_expose_to_userdrv(void);

// L10 substrate: re-point PxCLB/PxFB to the kernel's saved values after a
// userspace ahcid daemon dies.  Called from userdrv_on_owner_death.  See
// ahci.c for details.
void ahci_restore_after_userdrv_death(void);
