#include "gdt.h"

#define GDT_ENTRIES 3

// GDT entries: NULL, Kernel Code, Kernel Data
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_pointer;

// Assembly function to load GDT (defined in gdt.S)
extern void gdt_load(struct gdt_ptr *gdt_ptr);

// Helper function to set up a GDT entry
static void gdt_set_gate(int num, uint8_t access, uint8_t granularity) {
    // In x86_64 long mode, base and limit are ignored for code/data segments
    gdt[num].base_low    = 0x0000;
    gdt[num].base_middle = 0x00;
    gdt[num].base_high   = 0x00;
    gdt[num].limit_low   = 0x0000;
    gdt[num].granularity = granularity;
    gdt[num].access      = access;
}

void gdt_init(void) {
    // Set up GDT pointer
    gdt_pointer.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gdt_pointer.base  = (uint64_t)&gdt;

    // Entry 0: NULL descriptor (required by x86 architecture)
    gdt_set_gate(0, 0x00, 0x00);

    // Entry 1: Kernel Code Segment (0x08)
    // Access: 0x9A = Present, Ring 0, Code segment, Executable, Readable
    // Granularity: 0xAF = 4KB granularity, 64-bit code segment
    gdt_set_gate(1, 0x9A, 0xAF);

    // Entry 2: Kernel Data Segment (0x10)
    // Access: 0x92 = Present, Ring 0, Data segment, Writable
    // Granularity: 0xCF = 4KB granularity, 32-bit operands
    gdt_set_gate(2, 0x92, 0xCF);

    // Load the new GDT
    gdt_load(&gdt_pointer);
}
