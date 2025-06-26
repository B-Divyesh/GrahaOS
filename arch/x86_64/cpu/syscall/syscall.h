#pragma once
#include <stdint.h>
#include "../interrupts.h" // Keep for the struct definitions

// Define system call numbers
#define SYS_TEST 0
#define SYS_PUTC 1001
#define SYS_DEBUG 9999

/**
 * @brief Initializes the system call interface.
 */
void syscall_init(void);

/**
 * @brief The main C-level system call dispatcher.
 * @param frame The register state from the calling task.
 *          THIS MUST USE syscall_frame, NOT interrupt_frame.
 */
void syscall_dispatcher(struct syscall_frame *frame); // <-- CORRECTED TYPE