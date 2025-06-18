#pragma once
#include <stdint.h>

// Define our system call numbers
#define SYS_TEST 0

// This struct defines the register state passed to the C syscall handler.
// It contains all the general-purpose registers that the user program
// might use to pass arguments to the kernel.
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} syscall_frame_t;

/**
 * @brief Initializes the system call interface.
 */
void syscall_init(void);

/**
 * @brief The main C-level system call dispatcher.
 * @param frame The register state from the calling task.
 */
void syscall_dispatcher(syscall_frame_t *frame);