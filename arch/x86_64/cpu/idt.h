#pragma once
#include <stdint.h>

#define IDT_ENTRIES 256

// IDT Entry structure for x86_64
struct idt_entry {
    uint16_t offset_1;        // Offset bits 0-15
    uint16_t selector;        // Code segment selector
    uint8_t  ist;             // Interrupt Stack Table offset (0 for now)
    uint8_t  type_attributes; // Gate type, DPL, and Present bit
    uint16_t offset_2;        // Offset bits 16-31
    uint32_t offset_3;        // Offset bits 32-63
    uint32_t zero;            // Reserved (must be zero)
} __attribute__((packed));

// IDT Pointer structure for LIDT instruction
struct idt_ptr {
    uint16_t limit;           // Size of IDT minus 1
    uint64_t base;            // Linear address of IDT
} __attribute__((packed));

/**
 * @brief Initialize the Interrupt Descriptor Table
 * Sets up handlers for all 256 possible interrupts
 */
void idt_init(void);
