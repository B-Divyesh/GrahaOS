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

// TSS Entry Structure
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

// TSS Structure
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

// Forward declaration of cpu_local_t
struct cpu_local_t;

// MODIFIED: Function now takes a cpu_id parameter
void gdt_init_for_cpu(uint32_t cpu_id);

// Legacy function for compatibility (will call gdt_init_for_cpu(0))
void gdt_init(void);