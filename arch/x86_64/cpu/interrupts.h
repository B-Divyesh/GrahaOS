#pragma once
#include <stdint.h>

// CRITICAL: The struct members must be in REVERSE order of the push sequence
// because the stack grows DOWN on x86_64.
// 
// If assembly does:
//   push A
//   push B
//   push C
// 
// Then memory layout is (from low to high):
//   [C][B][A]
//   ^
//   RSP
// 
// So struct should be: { C, B, A }

// Based on your CURRENT syscall.S which pushes in this order:
// push user_rsp, push err_code, push int_no, push rax...push r15
struct syscall_frame {
    // GPRs - in REVERSE order of pushes
    // Your syscall.S pushes: r15 first, then r14...then rax last
    // So in memory: rax is at lowest address
    uint64_t rax;    // offset 0 - pushed last
    uint64_t rbx;    // offset 8
    uint64_t rcx;    // offset 16 - contains user RIP
    uint64_t rdx;    // offset 24
    uint64_t rsi;    // offset 32
    uint64_t rdi;    // offset 40 - first syscall arg
    uint64_t rbp;    // offset 48
    uint64_t r8;     // offset 56
    uint64_t r9;     // offset 64
    uint64_t r10;    // offset 72
    uint64_t r11;    // offset 80 - contains user RFLAGS
    uint64_t r12;    // offset 88
    uint64_t r13;    // offset 96
    uint64_t r14;    // offset 104
    uint64_t r15;    // offset 112 - pushed first
    
    // System call info
    uint64_t int_no;    // offset 120 - syscall number
    uint64_t err_code;  // offset 128 - always 0 for syscalls
    uint64_t user_rsp;  // offset 136 - user stack pointer
} __attribute__((packed));

// For interrupts, we need to check interrupts.S push order
// The isr_common in interrupts.S pushes: r15, r14...rax
// So same order as syscalls
struct interrupt_frame {
    // GPRs - in REVERSE order of pushes
    uint64_t rax;    // offset 0
    uint64_t rbx;    // offset 8
    uint64_t rcx;    // offset 16
    uint64_t rdx;    // offset 24
    uint64_t rsi;    // offset 32
    uint64_t rdi;    // offset 40
    uint64_t rbp;    // offset 48
    uint64_t r8;     // offset 56
    uint64_t r9;     // offset 64
    uint64_t r10;    // offset 72
    uint64_t r11;    // offset 80
    uint64_t r12;    // offset 88
    uint64_t r13;    // offset 96
    uint64_t r14;    // offset 104
    uint64_t r15;    // offset 112
    
    // Interrupt info
    uint64_t int_no;    // offset 120
    uint64_t err_code;  // offset 128
    
    // CPU-pushed values (only for interrupts)
    uint64_t rip;       // offset 136
    uint64_t cs;        // offset 144
    uint64_t rflags;    // offset 152
    uint64_t rsp;       // offset 160
    uint64_t ss;        // offset 168
} __attribute__((packed));

void interrupt_handler(struct interrupt_frame *frame);
void syscall_dispatcher(struct syscall_frame *frame);
void irq_init(void);

/**
 * @brief Disables the legacy PIC by masking all interrupts
 */
void pic_disable(void);