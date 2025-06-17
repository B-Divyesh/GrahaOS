#pragma once
#include <stdint.h>
#include "../interrupts.h"

#define MAX_TASKS 128
#define KERNEL_STACK_SIZE 8192 // 8 KiB kernel stack per task

typedef enum {
    TASK_STATE_RUNNING,
    TASK_STATE_READY,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

// Use the interrupt_frame structure from interrupts.h for register state
typedef struct {
    struct interrupt_frame regs;
    uint64_t kernel_stack_top;
    task_state_t state;
    int id;
} task_t;

/**
 * @brief Initializes the scheduler and creates the initial kernel task.
 */
void sched_init(void);

/**
 * @brief Creates a new kernel thread.
 * @param entry_point The function pointer where the new thread will start execution.
 * @return The ID of the new task, or -1 on failure.
 */
int sched_create_task(void (*entry_point)(void));

/**
 * @brief The main scheduler function, called by the timer interrupt.
 * @param frame The register state of the interrupted task.
 */
void schedule(struct interrupt_frame *frame);

/**
 * @brief Get the currently running task.
 * @return Pointer to the current task structure.
 */
task_t* sched_get_current_task(void);

/**
 * @brief Get a task by its ID.
 * @param id The task ID to look for.
 * @return Pointer to the task structure, or NULL if not found.
 */
task_t* sched_get_task(int id);
