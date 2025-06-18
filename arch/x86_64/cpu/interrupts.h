#pragma once
#include <stdint.h>

// Structure representing the CPU state when an interrupt occurs.
// The order of members EXACTLY matches the order of registers on the stack
// from low address to high address, as pushed by our assembly stubs.
struct interrupt_frame {
    // Registers pushed by our stub (isr_common)
    // These are in the order they appear on the stack (lowest to highest address)
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    // Interrupt number and error code (pushed by isr_stub macro)
    uint64_t int_no;
    uint64_t err_code;

    // Pushed automatically by the CPU on interrupt
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/**
 * @brief Main C interrupt handler
 * Called by assembly stubs for all interrupts
 * @param frame Pointer to the interrupt stack frame
 */
void interrupt_handler(struct interrupt_frame *frame);

/**
 * @brief Initialize IRQ handling
 * Remaps the PIC and enables interrupts
 */
void irq_init(void);