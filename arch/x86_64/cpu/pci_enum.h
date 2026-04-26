// arch/x86_64/cpu/pci_enum.h
//
// Phase 21 — PCI bus enumeration at boot.
//
// The pre-Phase-21 pci.c API exposed a `pci_scan_for_device(class, subclass)`
// helper that walked the bus on every call. Phase 21 needs more: per-device
// ownership tracking (`userdrv_entries[]`), BAR-range validation in
// `sys_mmio_vmo_create`, and a snapshot for `drvctl list`. All three want
// a stable, pre-decoded table.
//
// `pci_enumerate_all()` walks the bus once at boot, builds `g_pci_table[]`
// with one entry per discovered function. For each BAR present in a function's
// header, it does the size-decode dance (write 0xFFFFFFFF, read back, mask,
// restore) so we know `bar_size` without re-running the decode. Only memory
// BARs (low bit clear) are decoded; I/O BARs are recorded with size 0.
// 64-bit memory BARs (mid-bits = 0x2) are handled across the BAR pair; the
// upper half's slot in `bars[]` is set to 0 and `bar_sizes[]` to 0 so callers
// don't double-count.
//
// `pci_scan_for_device(class, subclass, *out)` is rewritten as a thin wrapper
// over `pci_table_find_by_class` for backward compatibility with existing
// drivers (e1000, ahci) that haven't been migrated to the table API.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Maximum number of PCI functions we will track. QEMU's q35 chipset typically
// exposes ~6-10 devices; the e1000 system has 4 (host bridge, ISA bridge,
// IDE, e1000). 32 is generous for any realistic VM/laptop.
#define PCI_TABLE_MAX  32

// Phase 21: debug-injected fake PCI entries (`SYS_DEBUG_INJECT_PCI`) live in
// the same table. Callers that want to distinguish real-vs-fake check the
// `is_fake` bit. Used by the userdrv TAP test.
typedef struct pci_table_entry {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint8_t  is_fake;            // 1 if injected via SYS_DEBUG_INJECT_PCI
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;        // bits 0..6; bit 7 = multifunction (stripped)
    uint8_t  irq_line;           // PCI config offset 0x3C; legacy IRQ line.
    uint8_t  int_pin;            // PCI config offset 0x3D; INTx pin (1=A..4=D).
    uint8_t  _pad0[2];
    uint64_t bars[6];            // Decoded base addresses (memory BARs only).
    uint64_t bar_sizes[6];       // Decoded sizes (memory BARs only); 0 if absent.
} pci_table_entry_t;

extern pci_table_entry_t g_pci_table[PCI_TABLE_MAX];
extern uint32_t g_pci_table_count;

// pci_addr packing convenience (matches userdrv_entry_t.pci_addr):
//   (bus << 16) | (device << 8) | function
static inline uint32_t pci_pack_addr(uint8_t bus, uint8_t dev, uint8_t func) {
    return ((uint32_t)bus << 16) | ((uint32_t)dev << 8) | (uint32_t)func;
}

// Walk the bus once, populate g_pci_table[]. Idempotent: a second call clears
// the table and re-scans (used by tests). Logs a one-line summary via klog.
void pci_enumerate_all(void);

// Look up by packed pci_addr. Returns NULL on miss. O(g_pci_table_count).
pci_table_entry_t *pci_table_find_by_address(uint32_t pci_addr);

// Look up by vendor/device pair. Returns the first match (PCI guarantees
// uniqueness only per (bus,dev,func)). NULL on miss.
pci_table_entry_t *pci_table_find_by_id(uint16_t vendor, uint16_t device);

// Look up by class/subclass. Returns first match; NULL on miss. Used by the
// backward-compat shim `pci_scan_for_device`.
pci_table_entry_t *pci_table_find_by_class(uint8_t class_code, uint8_t subclass);

// SYS_DEBUG_INJECT_PCI backend (gated by WITH_DEBUG_SYSCALL + cmdline
// test_mode=1). Appends a fake entry; returns the new index or negative errno.
// Caller supplies vendor/device/class/subclass + one fake BAR.
int pci_inject_fake(uint16_t vendor, uint16_t device,
                    uint8_t class_code, uint8_t subclass,
                    uint64_t bar_phys, uint64_t bar_size);

// Test-only: remove a fake entry by index. Real entries are immutable.
int pci_remove_fake(uint32_t index);
