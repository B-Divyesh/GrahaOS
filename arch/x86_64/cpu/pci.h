// arch/x86_64/cpu/pci.h
#pragma once
#include <stdint.h>

// PCI Configuration Space I/O Ports
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// PCI Device Classes
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06

// Structure to hold information about a found PCI device
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar5; // We are interested in BAR5 for AHCI
} pci_device_t;

/**
 * @brief Reads a 32-bit word from a PCI device's configuration space.
 * @param bus The PCI bus number.
 * @param device The device number on the bus.
 * @param function The function number of the device.
 * @param offset The register offset in the configuration space (must be 4-byte aligned).
 * @return The 32-bit value read from the configuration space.
 */
uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/**
 * @brief Scans the PCI bus for a device with a specific class and subclass.
 * @param class_code The class code to search for.
 * @param subclass_code The subclass code to search for.
 * @param pci_dev Pointer to a pci_device_t struct to store the found device's info.
 * @return 1 if a device is found, 0 otherwise.
 */
int pci_scan_for_device(uint8_t class_code, uint8_t subclass_code, pci_device_t *pci_dev);