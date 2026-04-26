// arch/x86_64/drivers/ioapic/ioapic.h
//
// Phase 21.1 — Minimal IOAPIC (82093AA) driver for legacy-IRQ → LAPIC vector
// routing. Phase 21 substrate's `pic_unmask_irq` is a no-op under LAPIC mode
// (smp.c:328 calls pic_disable() which masks all 16 PIC lines globally).
// Without IOAPIC redirection-table programming, IRQ 11 (E1000 typical) cannot
// reach userdrv vectors 50..65 — the daemon's IRQ wait would block forever.
//
// Scope intentionally narrow:
//   - Single IOAPIC at the well-known MMIO base 0xFEC00000 (QEMU default,
//     Intel reference platforms). ACPI MADT parse is a Phase 22+ item.
//   - Physical destination mode only. Targets a single LAPIC (BSP).
//   - Level-triggered active-low convention for PCI INTx (matches E1000).
//   - No MSI / MSI-X support (also Phase 22+).
//
// The 82093AA programming model is dead simple:
//   - Two MMIO registers used for everything: IOREGSEL @ 0x00, IOREGWIN @ 0x10.
//   - Write the index of the indirect register you want into IOREGSEL, then
//     read/write IOREGWIN to access that register.
//   - IOAPICID @ index 0x00, IOAPICVER @ index 0x01 (low byte = version,
//     high byte = max redirection entry index).
//   - Redirection-table entries are 64 bits each, accessed as two 32-bit
//     halves at indices 0x10 + 2*N (low) and 0x10 + 2*N + 1 (high).
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialize the IOAPIC: map MMIO, validate ID/version, mask all redirection
// entries, log the discovered max entry count. Idempotent (second call is
// a no-op).  Called from kmain after lapic_init.
void ioapic_init(void);

// Program a redirection-table entry to deliver `gsi` (global system interrupt
// — for legacy IRQs this is the same as the IRQ line) to `vector` on the
// LAPIC identified by `lapic_id` (physical destination mode).
//   level=true       → level-triggered (PCI INTx convention)
//   active_low=true  → active-low pin polarity (PCI INTx convention)
//
// The entry is left masked (bit 16 = 1); call ioapic_unmask_irq separately
// once the consumer is ready to receive interrupts. Symmetric with PIC API.
void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id,
                      bool level, bool active_low);

// Mask / unmask the redirection-table entry for `gsi`. Read-modify-write of
// bit 16 in the low half. Safe to call before ioapic_route_irq has set up
// the entry — the routing fields just stay zero until programmed.
void ioapic_mask_irq(uint8_t gsi);
void ioapic_unmask_irq(uint8_t gsi);

// True if ioapic_init succeeded (MMIO mapped + ID/version read back valid).
bool ioapic_is_present(void);

// Max redirection entry index supported by the hardware (typically 23 for
// QEMU q35, giving 24 entries [0..23] per the IOAPICVER MAXREDIR field).
uint8_t ioapic_max_redir_entry(void);
