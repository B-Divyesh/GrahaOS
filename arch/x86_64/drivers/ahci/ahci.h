// arch/x86_64/drivers/ahci/ahci.h
#pragma once
#include <stdint.h>
#include <stddef.h>

// AHCI HBA Memory Registers (Generic Host Control)
typedef volatile struct {
    uint32_t cap;       // 0x00 Host Capabilities
    uint32_t ghc;       // 0x04 Global Host Control
    uint32_t is;        // 0x08 Interrupt Status
    uint32_t pi;        // 0x0C Ports Implemented
    uint32_t vs;        // 0x10 Version
    uint32_t ccc_ctl;   // 0x14 Command Completion Coalescing Control
    uint32_t ccc_pts;   // 0x18 Command Completion Coalescing Ports
    uint32_t em_loc;    // 0x1C Enclosure Management Location
    uint32_t em_ctl;    // 0x20 Enclosure Management Control
    uint32_t cap2;      // 0x24 Host Capabilities Extended
    uint32_t bohc;      // 0x28 BIOS/OS Handoff Control and Status
    uint8_t  rsv[0x60-0x2C];
    uint8_t  vendor[0x100-0x60];
    // Port specific registers start at 0x100
} __attribute__((packed)) ahci_hba_mem_t;

// AHCI Port Registers
typedef volatile struct {
    uint64_t clb;       // 0x00 Command List Base Address
    uint64_t fb;        // 0x08 FIS Base Address
    uint32_t is;        // 0x10 Interrupt Status
    uint32_t ie;        // 0x14 Interrupt Enable
    uint32_t cmd;       // 0x18 Command and Status
    uint32_t rsv0;      // 0x1C Reserved
    uint32_t tfd;       // 0x20 Task File Data
    uint32_t sig;       // 0x24 Signature
    uint32_t ssts;      // 0x28 SATA Status (SCR0: SStatus)
    uint32_t sctl;      // 0x2C SATA Control (SCR2: SControl)
    uint32_t serr;      // 0x30 SATA Error (SCR1: SError)
    uint32_t sact;      // 0x34 SATA Active (SCR3: SActive)
    uint32_t ci;        // 0x38 Command Issue
    uint32_t sntf;      // 0x3C SATA Notification (SCR4: SNotification)
    uint32_t fbs;       // 0x40 FIS-based Switching Control
    uint8_t  rsv1[0x70-0x44];
    uint8_t  vendor[0x80-0x70];
} __attribute__((packed)) ahci_port_t;

// AHCI Command Header
typedef struct {
    uint8_t  cfl:5;     // Command FIS Length in DWORDS, 2 ~ 16
    uint8_t  a:1;       // ATAPI
    uint8_t  w:1;       // Write, 1: H2D, 0: D2H
    uint8_t  p:1;       // Prefetchable
    uint8_t  r:1;       // Reset
    uint8_t  b:1;       // BIST
    uint8_t  c:1;       // Clear Busy upon R_OK
    uint8_t  rsv0:1;
    uint8_t  pmp:4;     // Port Multiplier Port
    uint16_t prdtl;     // Physical Region Descriptor Table Length in entries
    volatile uint32_t prdbc; // Physical Region Descriptor Byte Count transferred
    uint64_t ctba;      // Command Table Base Address
    uint32_t rsv1[4];   // Reserved
} __attribute__((packed)) ahci_cmd_header_t;

// AHCI Physical Region Descriptor Table Entry
typedef struct {
    uint64_t dba;       // Data Base Address
    uint32_t rsv0;      // Reserved
    uint32_t dbc:22;    // Byte Count, 0-based. Max 4MB - 1.
    uint32_t rsv1:9;    // Reserved
    uint32_t i:1;       // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;

// AHCI Command Table
typedef struct {
    uint8_t  cfis[64];  // Command FIS
    uint8_t  acmd[16];  // ATAPI Command, 12 or 16 bytes
    uint8_t  rsv[48];
    ahci_prdt_entry_t prdt_entry[1]; // Variable-size PRDT
} __attribute__((packed)) ahci_cmd_table_t;

// Frame Information Structure (FIS) - Host to Device
typedef struct {
    uint8_t  fis_type;  // 0x27
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;       // 1: Command, 0: Control
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
} __attribute__((packed)) fis_reg_h2d_t;

/**
 * @brief Initializes the AHCI driver. Scans PCI for an AHCI controller,
 *        maps its memory, and initializes it and any attached drives.
 */
void ahci_init(void);

/**
 * @brief Reads sectors from an AHCI drive.
 * @param port_num The port number of the drive.
 * @param lba The starting Logical Block Address.
 * @param count The number of sectors to read.
 * @param buf The buffer to store the read data into.
 * @return 0 on success, non-zero on failure.
 */
int ahci_read(int port_num, uint64_t lba, uint16_t count, void *buf);

/**
 * @brief Writes sectors to an AHCI drive.
 * @param port_num The port number of the drive.
 * @param lba The starting Logical Block Address.
 * @param count The number of sectors to write.
 * @param buf The buffer containing the data to write.
 * @return 0 on success, non-zero on failure.
 */
int ahci_write(int port_num, uint64_t lba, uint16_t count, void *buf);

int ahci_flush_cache(int port_num);