#pragma once
#include <stdint.h>

// This structure represents the CPU state saved on the stack by SYSCALLS ONLY.
// The order of fields MUST EXACTLY match the order of `push` operations in the
// syscall assembly handler.
//
// CRITICAL: This struct MUST be packed to ensure the C layout matches the assembly's
// tightly-packed data with no padding bytes between fields.
//
// CRITICAL FIX: Fields are now in REVERSE PUSH ORDER to match stack memory layout
// Stack grows downward: push rax, push rbx creates [rax][rbx] where rbx is at lower address
// C struct fields must match this physical memory layout
struct syscall_frame {
    // General purpose registers - REVERSE ORDER of push sequence
    // Assembly: push rax, push rbx... push r15
    // C layout must be reverse: r15, r14... rax (r15 at lowest address)
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    // Pushed by assembly before GPRs
    uint64_t int_no;
    uint64_t err_code;

    // NOTE: For syscalls, the CPU does NOT automatically push RIP, CS, RFLAGS, etc.
    // Those values are passed in registers (RCX=RIP, R11=RFLAGS) and handled separately.
} __attribute__((packed));

// This structure represents the CPU state saved on the stack by HARDWARE INTERRUPTS.
// The order of fields MUST EXACTLY match the order of `push` operations in the
// interrupt assembly handlers.
//
// CRITICAL FIX: Fields are now in REVERSE PUSH ORDER to match stack memory layout
// Stack layout from low address to high address after all pushes:
// [...][r15][r14]...[rax][int_no][err_code][rip][cs][rflags][rsp][ss]
//
// CRITICAL: This struct MUST be packed to ensure the C layout matches the assembly's
// tightly-packed data with no padding bytes between fields.
struct interrupt_frame {
    // General purpose registers - REVERSE ORDER of push sequence  
    // Assembly: push rax, push rbx... push r15
    // C layout must be reverse: r15, r14... rax (r15 at lowest address)
    // Pushed by assembly
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15; 

    // Pushed by our ISR stubs before GPRs
    uint64_t int_no;
    uint64_t err_code;

    // Pushed automatically by the CPU on interrupt/exception
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

/**
 * @brief Main C interrupt handler
 * Called by assembly stubs for all interrupts
 * @param frame Pointer to the interrupt stack frame
 */
void interrupt_handler(struct interrupt_frame *frame);

/**
 * @brief Main syscall dispatcher
 * Called by assembly syscall handler
 * @param frame Pointer to the syscall stack frame
 */
void syscall_dispatcher(struct syscall_frame *frame);

/**
 * @brief Initialize IRQ handling
 * Remaps the PIC and enables interrupts
 */
void irq_init(void);