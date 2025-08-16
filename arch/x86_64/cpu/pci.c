// arch/x86_64/cpu/pci.c
#include "pci.h"
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
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vendor_device = pci_read_config(bus, device, function, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) {
                    // Device doesn't exist
                    continue;
                }

                uint32_t class_reg = pci_read_config(bus, device, function, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t subclass = (class_reg >> 16) & 0xFF;

                if (base_class == class_code && subclass == subclass_code) {
                    // Device found!
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
    return 0; // Device not found
}