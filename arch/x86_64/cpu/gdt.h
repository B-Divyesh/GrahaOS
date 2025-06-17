#pragma once
#include <stdint.h>

// GDT Entry structure - must be packed to prevent compiler padding
struct gdt_entry {
    uint16_t limit_low;    // Lower 16 bits of limit
    uint16_t base_low;     // Lower 16 bits of base
    uint8_t  base_middle;  // Next 8 bits of base
    uint8_t  access;       // Access flags
    uint8_t  granularity;  // Granularity flags and upper 4 bits of limit
    uint8_t  base_high;    // Upper 8 bits of base
} __attribute__((packed));

// GDT Pointer structure for LGDT instruction
struct gdt_ptr {
    uint16_t limit;        // Size of GDT minus 1
    uint64_t base;         // Linear address of GDT
} __attribute__((packed));

/**
 * @brief Initialize the Global Descriptor Table
 * Sets up kernel code and data segments for x86_64 long mode
 */
void gdt_init(void);
