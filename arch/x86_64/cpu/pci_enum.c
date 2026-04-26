// arch/x86_64/cpu/pci_enum.c
//
// Phase 21 — boot-time PCI enumeration. See header for design notes.

#include "pci_enum.h"
#include "pci.h"
#include "ports.h"
#include "../../../kernel/log.h"

pci_table_entry_t g_pci_table[PCI_TABLE_MAX];
uint32_t          g_pci_table_count = 0;

// ---------------------------------------------------------------------------
// BAR size-decode dance (per OSDev wiki).
//   1. Save original BAR value.
//   2. Write 0xFFFFFFFF to BAR.
//   3. Read back; size = (~(read_back & mask)) + 1, where mask is 0xFFFFFFF0
//      for memory BARs and 0xFFFFFFFC for I/O BARs.
//   4. Restore original.
//
// For 64-bit memory BARs (low 4 bits = 0x4 / mid bits 1..2 = 0x2), the size
// is decoded across the pair. Phase 21's targets (E1000, AHCI) are all 32-bit
// memory BARs, but we handle 64-bit for completeness. I/O BARs are skipped
// (bar_size = 0).
// ---------------------------------------------------------------------------
static void decode_bars(pci_table_entry_t *e) {
    int bar_count = (e->header_type & 0x7F) == 0 ? 6 : 2;  // type 0 = device, type 1 = bridge
    for (int i = 0; i < bar_count; i++) {
        uint8_t off = (uint8_t)(0x10 + i * 4);
        uint32_t orig = pci_read_config(e->bus, e->device, e->function, off);
        if (orig == 0) {
            e->bars[i] = 0;
            e->bar_sizes[i] = 0;
            continue;
        }
        // I/O BAR: low bit set. Skip — drivers we care about use memory BARs.
        if (orig & 0x1) {
            e->bars[i] = orig & 0xFFFFFFFCu;
            e->bar_sizes[i] = 0;
            continue;
        }
        // Memory BAR. Inspect low 4 bits to detect 64-bit.
        uint8_t bar_type = (orig >> 1) & 0x3;  // 0=32-bit, 2=64-bit
        // Size dance — write all-ones, read back, compute size, restore.
        pci_write_config(e->bus, e->device, e->function, off, 0xFFFFFFFFu);
        uint32_t low_back = pci_read_config(e->bus, e->device, e->function, off);
        pci_write_config(e->bus, e->device, e->function, off, orig);

        if (bar_type == 0x2 && i < bar_count - 1) {
            uint8_t off_hi = (uint8_t)(0x10 + (i + 1) * 4);
            uint32_t orig_hi = pci_read_config(e->bus, e->device, e->function, off_hi);
            pci_write_config(e->bus, e->device, e->function, off_hi, 0xFFFFFFFFu);
            uint32_t hi_back = pci_read_config(e->bus, e->device, e->function, off_hi);
            pci_write_config(e->bus, e->device, e->function, off_hi, orig_hi);

            uint64_t base = ((uint64_t)orig_hi << 32) | (orig & 0xFFFFFFF0u);
            uint64_t mask = ((uint64_t)hi_back << 32) | (low_back & 0xFFFFFFF0u);
            e->bars[i]      = base;
            e->bar_sizes[i] = (~mask) + 1;
            // Pair occupies BAR i and i+1; mark upper as consumed.
            e->bars[i + 1]      = 0;
            e->bar_sizes[i + 1] = 0;
            i++;  // Skip the upper half.
        } else {
            uint32_t base32 = orig & 0xFFFFFFF0u;
            uint32_t mask32 = low_back & 0xFFFFFFF0u;
            e->bars[i]      = base32;
            e->bar_sizes[i] = (uint64_t)((~mask32) + 1u);
        }
    }
}

// ---------------------------------------------------------------------------
// Probe a single (bus, dev, func) slot. If a device exists, populate one
// table entry. Returns 1 if multifunction (caller should probe func 1..7).
// ---------------------------------------------------------------------------
static int probe_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t vendor_device = pci_read_config(bus, dev, func, 0x00);
    if ((vendor_device & 0xFFFF) == 0xFFFF) return 0;  // No device.
    if (g_pci_table_count >= PCI_TABLE_MAX) {
        klog(KLOG_WARN, SUBSYS_CORE, "pci_enum: table full (%u entries)",
             (unsigned)g_pci_table_count);
        return 0;
    }

    uint32_t class_reg   = pci_read_config(bus, dev, func, 0x08);
    uint32_t header_reg  = pci_read_config(bus, dev, func, 0x0C);
    uint32_t irq_reg     = pci_read_config(bus, dev, func, 0x3C);

    pci_table_entry_t *e = &g_pci_table[g_pci_table_count++];
    e->bus       = bus;
    e->device    = dev;
    e->function  = func;
    e->is_fake   = 0;
    e->vendor_id = (uint16_t)(vendor_device & 0xFFFF);
    e->device_id = (uint16_t)(vendor_device >> 16);
    e->prog_if   = (uint8_t)((class_reg >> 8) & 0xFF);
    e->subclass  = (uint8_t)((class_reg >> 16) & 0xFF);
    e->class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    e->header_type = (uint8_t)((header_reg >> 16) & 0xFF);
    e->irq_line  = (uint8_t)(irq_reg & 0xFF);
    e->int_pin   = (uint8_t)((irq_reg >> 8) & 0xFF);
    for (int i = 0; i < 6; i++) {
        e->bars[i] = 0;
        e->bar_sizes[i] = 0;
    }
    decode_bars(e);

    // Multifunction bit: header_type bit 7. Caller probes funcs 1..7 if set.
    return (e->header_type & 0x80) ? 1 : 0;
}

void pci_enumerate_all(void) {
    g_pci_table_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            int multi = probe_function((uint8_t)bus, dev, 0);
            if (!multi) continue;
            for (uint8_t func = 1; func < 8; func++) {
                probe_function((uint8_t)bus, dev, func);
            }
        }
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "pci_enum: %u devices discovered (max=%u)",
         (unsigned)g_pci_table_count, (unsigned)PCI_TABLE_MAX);
}

pci_table_entry_t *pci_table_find_by_address(uint32_t pci_addr) {
    uint8_t bus  = (uint8_t)((pci_addr >> 16) & 0xFF);
    uint8_t dev  = (uint8_t)((pci_addr >> 8) & 0xFF);
    uint8_t func = (uint8_t)(pci_addr & 0xFF);
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        pci_table_entry_t *e = &g_pci_table[i];
        if (e->bus == bus && e->device == dev && e->function == func)
            return e;
    }
    return 0;
}

pci_table_entry_t *pci_table_find_by_id(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        pci_table_entry_t *e = &g_pci_table[i];
        if (e->vendor_id == vendor && e->device_id == device)
            return e;
    }
    return 0;
}

pci_table_entry_t *pci_table_find_by_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        pci_table_entry_t *e = &g_pci_table[i];
        if (e->class_code == class_code && e->subclass == subclass)
            return e;
    }
    return 0;
}

int pci_inject_fake(uint16_t vendor, uint16_t device,
                    uint8_t class_code, uint8_t subclass,
                    uint64_t bar_phys, uint64_t bar_size) {
    if (g_pci_table_count >= PCI_TABLE_MAX) return -28;  // -ENOSPC
    pci_table_entry_t *e = &g_pci_table[g_pci_table_count];
    // Fakes get bus=0xFE/dev=N/func=0 to keep them out of any real PCI range.
    e->bus       = 0xFE;
    e->device    = (uint8_t)(g_pci_table_count & 0x1F);
    e->function  = 0;
    e->is_fake   = 1;
    e->vendor_id = vendor;
    e->device_id = device;
    e->prog_if   = 0;
    e->subclass  = subclass;
    e->class_code = class_code;
    e->header_type = 0;
    e->irq_line  = 0xFF;  // No real IRQ line for fakes.
    e->int_pin   = 0;
    for (int i = 0; i < 6; i++) {
        e->bars[i] = 0;
        e->bar_sizes[i] = 0;
    }
    e->bars[0]      = bar_phys;
    e->bar_sizes[0] = bar_size;
    int idx = (int)g_pci_table_count;
    g_pci_table_count++;
    klog(KLOG_INFO, SUBSYS_CORE,
         "pci_enum: injected fake idx=%d vendor=0x%04x device=0x%04x bar=0x%lx",
         idx, (unsigned)vendor, (unsigned)device, (unsigned long)bar_phys);
    return idx;
}

int pci_remove_fake(uint32_t index) {
    if (index >= g_pci_table_count) return -22;  // -EINVAL
    if (!g_pci_table[index].is_fake) return -1;  // -EPERM (real entry)
    // Compact the table by shifting subsequent entries down.
    for (uint32_t i = index; i + 1 < g_pci_table_count; i++) {
        g_pci_table[i] = g_pci_table[i + 1];
    }
    g_pci_table_count--;
    return 0;
}
