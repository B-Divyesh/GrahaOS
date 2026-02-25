// kernel/state.h
// Phase 8a: System state structures shared between kernel and user-space
// These structs define the format of data returned by SYS_GET_SYSTEM_STATE
#pragma once

#include <stdint.h>

// State query categories for SYS_GET_SYSTEM_STATE
#define STATE_CAT_MEMORY     0
#define STATE_CAT_PROCESSES  1
#define STATE_CAT_FILESYSTEM 2
#define STATE_CAT_SYSTEM     3
#define STATE_CAT_DRIVERS        4
#define STATE_CAT_CAPABILITIES   5
#define STATE_CAT_ALL            255

// Limits (must match kernel defines)
#define STATE_MAX_TASKS       32
#define STATE_PROC_NAME_LEN   32
#define STATE_MAX_DRIVERS     16
#define STATE_DRIVER_NAME_LEN 24
#define STATE_MAX_DRIVER_OPS  8
#define STATE_OP_NAME_LEN     24
#define STATE_STAT_KEY_LEN    16
#define STATE_MAX_DRIVER_STATS 8
#define STATE_MAX_CPUS        16
#define STATE_MAX_CAPS        64
#define STATE_CAP_NAME_LEN    32
#define STATE_MAX_CAP_DEPS    8

// Process state values (match task_state_t in sched.h)
#define STATE_PROC_ZOMBIE     0
#define STATE_PROC_READY      1
#define STATE_PROC_RUNNING    2
#define STATE_PROC_BLOCKED    3

// Driver type values (match DRIVER_TYPE_* in driver.h)
#define STATE_DRIVER_BLOCK    0
#define STATE_DRIVER_INPUT    1
#define STATE_DRIVER_DISPLAY  2
#define STATE_DRIVER_TIMER    3
#define STATE_DRIVER_SERIAL   4
#define STATE_DRIVER_FS       5
#define STATE_DRIVER_OTHER    255

// --- Per-process snapshot ---
typedef struct state_process {
    int32_t  pid;
    int32_t  parent_pid;
    int32_t  pgid;
    uint32_t state;
    char     name[STATE_PROC_NAME_LEN];
    uint64_t heap_start;
    uint64_t brk;
    uint64_t stack_top;
    uint64_t heap_used;
    uint32_t pending_signals;
    int32_t  exit_status;
} state_process_t;

// --- Process list snapshot ---
typedef struct {
    uint32_t count;
    uint32_t _pad;
    state_process_t procs[STATE_MAX_TASKS];
} state_process_list_t;

// --- Memory snapshot ---
typedef struct {
    uint64_t total_physical;
    uint64_t free_physical;
    uint64_t used_physical;
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t page_size;
} state_memory_t;

// --- Filesystem snapshot ---
typedef struct {
    uint32_t open_files;
    uint32_t max_open_files;
    uint32_t block_devices;
    uint32_t max_block_devices;
    uint32_t mounted_fs;
    uint32_t max_filesystems;
    uint32_t grahafs_mounted;
    uint32_t grahafs_total_blocks;
    uint32_t grahafs_free_blocks;
    uint32_t grahafs_free_inodes;
    uint32_t grahafs_max_inodes;
    uint32_t grahafs_block_size;
} state_filesystem_t;

// --- System/CPU snapshot ---
typedef struct {
    uint32_t cpu_count;
    uint32_t bsp_lapic_id;
    uint32_t schedule_count;
    uint32_t context_switches;
    struct {
        uint32_t lapic_id;
        uint32_t active;
    } cpus[STATE_MAX_CPUS];
    uint32_t cpu_entries;
    uint32_t _pad;
} state_system_t;

// --- Driver operation descriptor ---
typedef struct {
    char     name[STATE_OP_NAME_LEN];
    uint32_t param_count;
    uint32_t flags;   // 0=query (read-only), 1=mutating (write)
} state_driver_op_t;

// --- Driver key-value stat ---
typedef struct {
    char     key[STATE_STAT_KEY_LEN];
    uint64_t value;
} state_driver_stat_t;

// --- Per-driver snapshot ---
typedef struct {
    char     name[STATE_DRIVER_NAME_LEN];
    uint32_t type;
    uint32_t initialized;
    uint32_t op_count;
    uint32_t stat_count;
    state_driver_op_t  ops[STATE_MAX_DRIVER_OPS];
    state_driver_stat_t stats[STATE_MAX_DRIVER_STATS];
} state_driver_info_t;

// --- Driver list ---
typedef struct {
    uint32_t count;
    uint32_t _pad;
    state_driver_info_t drivers[STATE_MAX_DRIVERS];
} state_driver_list_t;

// --- Per-capability snapshot (Phase 8b) ---
typedef struct {
    char     name[STATE_CAP_NAME_LEN];
    uint32_t type;           // CAP_HARDWARE..CAP_COMPOSITE
    uint32_t state;          // CAP_STATE_*
    int32_t  owner_pid;      // -1=kernel
    uint32_t dep_count;
    uint32_t dep_indices[STATE_MAX_CAP_DEPS];
    uint32_t op_count;
    uint64_t activation_count;
} state_cap_entry_t;

// --- Capability list ---
typedef struct {
    uint32_t count;
    uint32_t _pad;
    state_cap_entry_t caps[STATE_MAX_CAPS];
} state_cap_list_t;

// --- Combined full snapshot ---
typedef struct {
    uint32_t version;
    uint32_t _pad;
    state_memory_t       memory;
    state_process_list_t processes;
    state_filesystem_t   filesystem;
    state_system_t       system;
    state_driver_list_t  drivers;
    state_cap_list_t     capabilities;
} state_snapshot_t;
