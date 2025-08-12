#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../interrupts.h"
#include "../../mm/vmm.h" // For vmm_address_space_t
#include "../interrupts.h"
#include "../smp.h"
#include "../../../../kernel/sync/spinlock.h"

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
    struct interrupt_frame regs;
    uint64_t cr3;
    int parent_id;
    int exit_status;
    // Track what child we're waiting for (-1 = any child)
    int waiting_for_child;
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

/**
 * @brief Check if a process has exited children to reap
 * @param parent_id Parent task ID
 * @param status Pointer to store exit status (can be NULL)
 * @return Child PID if zombie child found, -1 otherwise
 */
int sched_check_children(int parent_id, int *status);

/**
 * @brief Mark children as orphans when parent dies
 * @param parent_id Parent task ID
 */
void sched_orphan_children(int parent_id);

/**
 * @brief Reap a zombie task and free its resources
 * @param task_id Task ID to reap
 */
void sched_reap_zombie(int task_id);

/**
 * @brief Wake up parent waiting for a child
 * @param child_id Child task ID that exited
 */
void wake_waiting_parent(int child_id);

