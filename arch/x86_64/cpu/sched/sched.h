#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../interrupts.h"
#include "../../mm/vmm.h" // For vmm_address_space_t
#include "../smp.h"
#include "../../../../kernel/sync/spinlock.h"
#include "../../../../kernel/state.h"

#define MAX_TASKS 32
#define KERNEL_STACK_SIZE 16384

// Signal definitions (Phase 7d)
#define SIGTERM   1   // Terminate process (catchable)
#define SIGKILL   2   // Kill process (uncatchable)
#define SIGUSR1   3   // User-defined signal 1
#define SIGUSR2   4   // User-defined signal 2
#define SIGCHLD   5   // Child process exited
#define MAX_SIGNALS 16

// Default signal actions
#define SIG_DFL  ((void (*)(int))0)   // Default action (terminate)
#define SIG_IGN  ((void (*)(int))1)   // Ignore signal

// Task states
typedef enum {
    TASK_STATE_ZOMBIE,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED
} task_state_t;

// Spawn attributes for sys_spawn (Phase 7d)
typedef struct {
    int inherit_fds;      // Whether to inherit parent's file descriptors
    int priority;         // Scheduling priority (reserved for future use)
    uint32_t flags;       // Additional flags (reserved)
} spawn_attrs_t;

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

    // Heap management (Phase 7c)
    uint64_t heap_start;     // Start of heap region
    uint64_t brk;            // Current program break (end of heap)
    uint64_t stack_top;      // Top of user stack (to prevent heap collision)

    // Process management (Phase 7d)
    char name[32];                          // Process name
    int pgid;                               // Process group ID
    uint32_t pending_signals;               // Bitmask of pending signals
    void (*signal_handlers[MAX_SIGNALS])(int); // Signal handler function pointers
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

/**
 * @brief Spawn a new process from an ELF binary (Phase 7d)
 * @param path Path to ELF binary in initrd
 * @param parent_id ID of the parent process
 * @return Child PID on success, -1 on failure
 */
int sched_spawn_process(const char *path, int parent_id);

/**
 * @brief Send a signal to a process
 * @param pid Target process ID
 * @param signal Signal number
 * @return 0 on success, -1 on failure
 */
int sched_send_signal(int pid, int signal);

/**
 * @brief Register a signal handler for the current process
 * @param signal Signal number
 * @param handler Handler function pointer
 * @return Previous handler, or SIG_DFL on error
 */
void* sched_set_signal_handler(int signal, void (*handler)(int));

/**
 * @brief Check and deliver pending signals for a task
 * @param task Task to check
 * @return 1 if a signal was delivered, 0 otherwise
 */
int sched_deliver_signals(task_t *task);

/**
 * @brief Get a task by ID (including zombies)
 * @param id Task ID
 * @return Pointer to the task, or NULL if not found
 */
task_t* sched_get_task_any(int id);

/**
 * @brief Get the current task index
 * @return Current task index
 */
int sched_get_current_task_index(void);

// Phase 8a: Scheduler statistics (read-only, volatile)
extern volatile uint32_t schedule_count;
extern volatile uint32_t context_switches;

/**
 * @brief Snapshot all processes for system state reporting (Phase 8a)
 * @param out Array of state_process_t to fill
 * @param max_count Maximum entries to write
 * @return Number of entries written
 */
int sched_snapshot_processes(state_process_t *out, int max_count);

