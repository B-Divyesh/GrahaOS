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

// --- NEW: TSS Entry Structure ---
// A TSS descriptor is larger than a standard GDT entry.
struct tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid1;
    uint8_t  access;
    uint8_t  limit_high_and_flags;
    uint8_t  base_mid2;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

// --- NEW: TSS Structure ---
// This structure holds the information the CPU needs for tasks.
// For our purposes, we only care about the kernel stack pointer (RSP0).
struct tss {
    uint32_t reserved0;
    uint64_t rsp0; // The stack pointer to use when transitioning from user to kernel mode
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

// --- THE FIX ---
// Expose the kernel_tss so the scheduler can modify its rsp0 field.
extern struct tss kernel_tss;

/**
 * @brief Initialize the Global Descriptor Table
 * Sets up kernel code and data segments for x86_64 long mode
 * Now also sets up user segments and TSS for privilege switching
 */
void gdt_init(void);