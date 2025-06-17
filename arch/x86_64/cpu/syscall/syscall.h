#pragma once
#include <stdint.h>
#include "../interrupts.h" // <-- Include interrupts.h for the struct definition

// Define our system call numbers
#define SYS_TEST 0

/**
 * @brief Initializes the system call interface.
 */
void syscall_init(void);

/**
 * @brief The main C-level system call dispatcher.
 * @param frame The register state from the calling task.
 *              --- FIX: Changed type to the standard interrupt_frame ---
 * @return The value to be returned to the caller in RAX.
 */
uint64_t syscall_dispatcher(struct interrupt_frame *frame);
