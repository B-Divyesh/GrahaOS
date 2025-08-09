#include "idt.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_pointer;

// Assembly function to load IDT (defined in idt.S)
extern void idt_load(struct idt_ptr *idt_ptr);

// Array of interrupt handler stubs (defined in interrupts.S)
extern void *isr_stub_table[IDT_ENTRIES];

// Helper function to set up an IDT entry
static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_1 = base & 0xFFFF;
    idt[num].offset_2 = (base >> 16) & 0xFFFF;
    idt[num].offset_3 = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;  // No separate interrupt stack for now
    idt[num].type_attributes = flags;
    idt[num].zero = 0;
}

void idt_init(void) {
    // Set up IDT pointer
    idt_pointer.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idt_pointer.base = (uint64_t)&idt;

    // Kernel code segment selector
    uint16_t code_segment = 0x08;

    // Set up all IDT entries to point to their respective handlers
    for (int i = 0; i < IDT_ENTRIES; i++) {
        // Type attributes: 0x8E = Present, Ring 0, 64-bit Interrupt Gate
        idt_set_gate(i, (uint64_t)isr_stub_table[i], code_segment, 0x8E);
    }

    // Load the IDT
    idt_load(&idt_pointer);
}