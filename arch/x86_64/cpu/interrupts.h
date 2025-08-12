#pragma once
#include <stdint.h>

// CRITICAL: This must match the EXACT order of pushes in interrupts.S
// Stack grows DOWN, so first push ends up at highest address
struct interrupt_frame {
    // Pushed by our assembly code (isr_common)
    uint64_t ds;        // Data segment
    
    // General purpose registers (pushed in reverse order)
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
    
    // Pushed by interrupt stub
    uint64_t int_no;    // Interrupt number
    uint64_t err_code;  // Error code (or 0)
    
    // Pushed by CPU on interrupt
    uint64_t rip;       // Instruction pointer
    uint64_t cs;        // Code segment
    uint64_t rflags;    // CPU flags
    uint64_t rsp;       // Stack pointer
    uint64_t ss;        // Stack segment
} __attribute__((packed));

// Syscall frame structure (different from interrupts)
struct syscall_frame {
    // GPRs pushed by syscall.S
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;    // Contains user RIP
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;    // First syscall argument
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;    // Contains user RFLAGS
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    
    // Syscall info
    uint64_t int_no;    // Syscall number
    uint64_t err_code;  // Always 0 for syscalls
    uint64_t user_rsp;  // User stack pointer
} __attribute__((packed));

void interrupt_handler(struct interrupt_frame *frame);
void syscall_dispatcher(struct syscall_frame *frame);
void irq_init(void);
void pic_disable(void);