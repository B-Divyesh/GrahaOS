#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../interrupts.h"
#include "../../mm/vmm.h" // For vmm_address_space_t

#define MAX_TASKS 32
#define KERNEL_STACK_SIZE 16384

// Task states
typedef enum {
    TASK_STATE_ZOMBIE,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED
} task_state_t;

// Task structure
typedef struct {
    int id;
    task_state_t state;
    uint64_t kernel_stack_top;
    struct interrupt_frame regs; // CPU register state from interrupt/syscall
    // --- FIX: Store the address space (CR3) separately ---
    uint64_t cr3; // Physical address of the task's PML4 table
} task_t;

/**
 * @brief Initialize the scheduler
 */
void sched_init(void);

/**
 * @brief Create a new kernel task
 * @param entry_point Function pointer to the task's entry point
 * @return Task ID on success, -1 on failure
 */
int sched_create_task(void (*entry_point)(void));

/**
 * @brief Create a new user-mode process
 * @param rip Entry point address for the process
 * @param cr3 Physical address of the process's PML4 table
 * @return Task ID on success, -1 on failure
 */
int sched_create_user_process(uint64_t rip, uint64_t cr3);

/**
 * @brief Schedule the next task to run
 * @param frame Pointer to the interrupt frame (will be modified)
 */
void schedule(struct interrupt_frame *frame);

/**
 * @brief Get the currently running task
 * @return Pointer to the current task
 */
task_t* sched_get_current_task(void);

/**
 * @brief Get a task by ID
 * @param id Task ID
 * @return Pointer to the task, or NULL if not found
 */
task_t* sched_get_task(int id);