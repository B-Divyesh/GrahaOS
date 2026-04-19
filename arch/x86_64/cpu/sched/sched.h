#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../interrupts.h"
#include "../../mm/vmm.h" // For vmm_address_space_t
#include "../smp.h"
#include "../../../../kernel/sync/spinlock.h"
#include "../../../../kernel/state.h"
#include "../../../../kernel/cap/handle_table.h"
#include "../../../../kernel/cap/pledge.h"

#define MAX_TASKS 64
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

// Phase 10a: Per-process file descriptor types
#define FD_TYPE_UNUSED      0
#define FD_TYPE_CONSOLE     1   // serial+FB output / keyboard input
#define FD_TYPE_FILE        2   // VFS file (ref = global open_file_table index)
#define FD_TYPE_PIPE_READ   3   // Read end of pipe (ref = pipe index)
#define FD_TYPE_PIPE_WRITE  4   // Write end of pipe (ref = pipe index)

#define PROC_MAX_FDS 16

typedef struct {
    uint8_t type;     // FD_TYPE_*
    int16_t ref;      // Index into global file table, pipe table, or 0 for console
    uint8_t flags;    // Reserved
} proc_fd_t;

// Task states
typedef enum {
    TASK_STATE_ZOMBIE,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    // Phase 17: blocked on a channel endpoint (read or write waiter list).
    // Distinct from generic BLOCKED so the scheduler can enumerate IPC-wait
    // tasks separately and apply deadline-driven wakeups.
    TASK_STATE_CHAN_WAIT
} task_state_t;

// Phase 17: reason a task entered TASK_STATE_CHAN_WAIT.
#define CHAN_WAIT_READ   1
#define CHAN_WAIT_WRITE  2
// Phase 18: stream-specific wait reasons. The state is still CHAN_WAIT so the
// existing per-tick deadline scan in schedule() handles timeouts uniformly;
// wait_channel points at the stream or global work queue rather than a
// channel_t.
#define WAIT_STREAM_REAP    3   // task blocked in SYS_STREAM_REAP min_complete
#define WAIT_STREAM_SUBMIT  4   // reserved: blocking submit (Phase 18 does not use)
#define WAIT_STREAM_WORKER  5   // stream worker kernel thread idle, no jobs

// Spawn attributes for sys_spawn (Phase 7d). Extended in Phase 17 with
// handle-inheritance and VMO-backed-executable fields. Existing callers
// that zero-initialize the struct get backward-compatible behavior.
typedef struct {
    int inherit_fds;      // Whether to inherit parent's file descriptors
    int priority;         // Scheduling priority (reserved for future use)
    uint32_t flags;       // Additional flags (reserved)
    // Phase 15b: child process will have pledge_mask = parent->pledge_mask &
    // pledge_subset. Default PLEDGE_ALL means "inherit the parent unchanged".
    pledge_mask_t pledge_subset;
    // Phase 17: up to 16 cap_tokens transferred from parent to child at spawn
    // time. nhandles_to_inherit == 0 means no transfer. Each transferred
    // token is installed in the child's cap_handle_table; the parent's entry
    // is removed (all-or-nothing). If nhandles > 0 but exec_vmo is zero the
    // classic disk-ELF path runs with the transferred handles bolted on.
    uint64_t handles_to_inherit[16];   // opaque cap_token_t raw values
    uint8_t  nhandles_to_inherit;
    // Phase 17: boot the child from an in-memory VMO rather than a disk
    // path. When nonzero, path= may be NULL and exec_vmo is resolved in the
    // parent's handle table as a CAP_KIND_VMO. Requires PLEDGE_COMPUTE on
    // the parent.
    uint64_t exec_vmo;                 // cap_token_t raw
} spawn_attrs_t;

// Task structure
typedef struct task_struct {
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

    // Phase 8d: CAN event queue (circular buffer)
    state_cap_event_t event_queue[STATE_CAP_EVENT_QUEUE_SIZE];
    uint32_t event_head;            // Next read position
    uint32_t event_tail;            // Next write position
    uint32_t event_count;           // Current events in queue
    uint32_t event_waiting;         // 1=blocked waiting for events

    // Phase 10a: Per-process file descriptor table
    proc_fd_t fd_table[PROC_MAX_FDS];

    // Phase 15a: per-process capability handle table. Initialized on
    // task creation; freed + orphan-revoked on sched_reap_zombie.
    cap_handle_table_t cap_handles;

    // Phase 15b: pledge mask (monotonic narrow). Every sensitive syscall
    // entry calls pledge_allows(current, PLEDGE_*). pledge_lock serialises
    // concurrent SYS_PLEDGE calls against the same task.
    pledge_mask_t pledge_mask;
    spinlock_t    pledge_lock;

    // Phase 17: channel-wait plumbing. wait_next links the task into a
    // channel_t.read_waiters or .write_waiters list when state ==
    // TASK_STATE_CHAN_WAIT. wait_reason is CHAN_WAIT_READ or CHAN_WAIT_WRITE.
    // deadline_tsc (0 = infinite) gates the scheduler's per-tick deadline
    // scan. wait_channel is an opaque pointer to the channel this task is
    // parked on (needed when sched_reap_zombie yanks the task off).
    struct task_struct *wait_next;
    uint8_t             wait_reason;
    uint64_t            deadline_tsc;
    void               *wait_channel;
    // Phase 17: wait_result is set by the waker BEFORE transitioning the
    // task to READY. The blocking syscall reads this on resume to decide
    // between "woke on data" (0) and "woke on timeout" (-ETIMEDOUT) or
    // "woke on channel close" (-EPIPE).
    int32_t             wait_result;
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

// ---------------------------------------------------------------------------
// Phase 17: channel-wait primitives. A task blocks on a channel endpoint
// (read or write direction) until another task sends/receives, the channel
// is destroyed (EPIPE wake), or the deadline_tsc expires. The channel
// struct owns the linked-list head (typed as void* here to avoid pulling
// in kernel/ipc/channel.h from the scheduler header).
// ---------------------------------------------------------------------------

// Block current task on channel. dir is CHAN_WAIT_READ or CHAN_WAIT_WRITE;
// channel holds the matching waiter-list head. Timeout 0 means wait forever;
// otherwise kernel arms a deadline (nanoseconds from now). Returns after
// the task is woken: 0 on successful wake-by-data, -ETIMEDOUT on deadline,
// -EPIPE on channel close.
int sched_block_on_channel(void *channel, uint8_t dir, uint64_t timeout_ns,
                           struct task_struct **list_head);

// Wake one task off the given waiter list. Returns the woken task or NULL
// if the list was empty.
task_t *sched_wake_one_on_channel(struct task_struct **list_head,
                                  int32_t wait_result);

// Wake every task on the list (channel teardown / EPIPE). Each gets the
// given wait_result. Returns the count of woken tasks.
int sched_wake_all_on_channel(struct task_struct **list_head,
                              int32_t wait_result);

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

// Phase 8d: CAN event queue operations
void sched_enqueue_cap_event(int32_t pid, const state_cap_event_t *event);
int sched_dequeue_cap_event(int task_id, state_cap_event_t *out);
int sched_pending_event_count(int task_id);

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

