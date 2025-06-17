#pragma once
#include <stdint.h>

#define MAX_TASKS 128
#define KERNEL_STACK_SIZE 8192 // 8 KiB kernel stack per task

typedef enum {
    TASK_STATE_RUNNING,
    TASK_STATE_READY,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

// Simple register state structure for syscalls
typedef struct {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) register_state_t;

// Represents a single task (or process/thread) in the system.
typedef struct {
    register_state_t regs;       // Saved registers when not running
    uint64_t kernel_stack;       // Virtual address of kernel stack top
    task_state_t state;          // Current state of the task
    int id;                      // Unique task ID
} task_t;

/**
 * @brief Initializes the scheduler and creates the initial kernel task.
 */
void sched_init(void);

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
