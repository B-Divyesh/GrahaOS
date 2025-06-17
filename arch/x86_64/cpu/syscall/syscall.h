#pragma once
#include <stdint.h>
#include "../sched/sched.h"

// Define our system call numbers
#define SYS_TEST 0

/**
 * @brief Initializes the system call interface.
 * Sets up the MSRs to enable the syscall/sysret instructions.
 */
void syscall_init(void);

/**
 * @brief The main C-level system call dispatcher.
 * @param frame The register state from the calling task.
 * @return The value to be returned to the caller in RAX.
 */
uint64_t syscall_dispatcher(register_state_t *frame);
