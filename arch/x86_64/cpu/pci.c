// arch/x86_64/cpu/pci.c
//
// Phase 21: pci_scan_for_device rewritten to walk g_pci_table (built once at
// boot by pci_enumerate_all). The brute-force scanning loop remains accessible
// via the new pci_enum.c API for callers that haven't migrated. Existing
// drivers (e1000, ahci) keep their pci_scan_for_device call sites unchanged.
#include "pci.h"
#include "pci_enum.h"
#include "ports.h"

uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    // Create the address packet for the CONFIG_ADDRESS register.
    // Bit 31: Enable bit
    // Bits 23-16: Bus number
    // Bits 15-11: Device number
    // Bits 10-8: Function number
    // Bits 7-2: Register offset (must be 4-byte aligned, so lower 2 bits are 0)
    uint32_t address = (uint32_t)((bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC) | 0x80000000);

    // Write the address to the CONFIG_ADDRESS port.
    outl(PCI_CONFIG_ADDRESS, address);

    // Read the data from the CONFIG_DATA port.
    return inl(PCI_CONFIG_DATA);
}

int pci_scan_for_device(uint8_t class_code, uint8_t subclass_code, pci_device_t *pci_dev) {
    // Phase 21: walk the pre-built table instead of re-scanning the bus.
    // Falls back to the legacy linear scan if the table hasn't been built yet
    // (i.e., this function is called before pci_enumerate_all in kmain — only
    // happens in unusual orderings).
    if (g_pci_table_count > 0) {
        pci_table_entry_t *e = pci_table_find_by_class(class_code, subclass_code);
        if (!e) return 0;
        pci_dev->bus       = e->bus;
        pci_dev->device    = e->device;
        pci_dev->function  = e->function;
        pci_dev->vendor_id = e->vendor_id;
        pci_dev->device_id = e->device_id;
        // Legacy bar5 field — populated for AHCI compatibility.
        pci_dev->bar5      = (uint32_t)(e->bars[5] & 0xFFFFFFFFu);
        return 1;
    }
    // Fallback: legacy on-demand scan (pre-pci_enumerate_all path).
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vendor_device = pci_read_config(bus, device, function, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;
                uint32_t class_reg = pci_read_config(bus, device, function, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t subclass = (class_reg >> 16) & 0xFF;
                if (base_class == class_code && subclass == subclass_code) {
                    pci_dev->bus = bus;
                    pci_dev->device = device;
                    pci_dev->function = function;
                    pci_dev->vendor_id = vendor_device & 0xFFFF;
                    pci_dev->device_id = (vendor_device >> 16) & 0xFFFF;
                    pci_dev->bar5 = pci_read_config(bus, device, function, 0x24);
                    return 1;
                }
            }
        }
    }
    return 0;
}

void pci_write_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function, int bar_index) {
    uint8_t offset = 0x10 + (bar_index * 4);
    return pci_read_config(bus, device, function, offset);
}

void pci_enable_bus_mastering(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t command = pci_read_config(bus, device, function, 0x04);
    // Bit 0: I/O Space, Bit 1: Memory Space, Bit 2: Bus Master
    command |= (1 << 0) | (1 << 1) | (1 << 2);
    pci_write_config(bus, device, function, 0x04, command);
}