#pragma once
#include <stdint.h>

// This struct MUST be packed. The fields are now in REVERSE PUSH order
// to match the stack's physical memory layout.
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t user_rsp;
} __attribute__((packed));

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

void interrupt_handler(struct interrupt_frame *frame);
void syscall_dispatcher(struct syscall_frame *frame);
void irq_init(void);