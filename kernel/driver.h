// kernel/driver.h
// Phase 8a: Driver registration framework
// All drivers MUST register with this framework at init time.
// This enables AI-driven system introspection and (in Phase 8b) RPC control.
#pragma once

#include <stdint.h>
#include "state.h"

// Driver type constants
#define DRIVER_TYPE_BLOCK    0
#define DRIVER_TYPE_INPUT    1
#define DRIVER_TYPE_DISPLAY  2
#define DRIVER_TYPE_TIMER    3
#define DRIVER_TYPE_SERIAL   4
#define DRIVER_TYPE_FS       5
#define DRIVER_TYPE_OTHER    255

#define MAX_REGISTERED_DRIVERS 16

// Operation flags
#define DRIVER_OP_QUERY    0   // Read-only operation
#define DRIVER_OP_MUTATING 1   // Modifies state

// Function signature for driver state snapshot callback.
// The driver fills stats[] with key-value pairs describing its current state.
// Returns: number of stats written (0 to max).
typedef int (*driver_get_stats_fn)(state_driver_stat_t *stats, int max);

// Operation descriptor (registered at init time, describes what the driver can do)
typedef struct {
    const char *name;       // e.g. "read", "write", "flush"
    uint32_t param_count;   // number of parameters the operation takes
    uint32_t flags;         // DRIVER_OP_QUERY or DRIVER_OP_MUTATING
} driver_op_desc_t;

// Driver descriptor (passed to driver_register at init time)
typedef struct {
    const char *name;                          // e.g. "ahci", "keyboard"
    uint32_t type;                             // DRIVER_TYPE_*
    driver_get_stats_fn get_stats;             // callback to get current stats
    int op_count;                              // number of operations
    driver_op_desc_t ops[STATE_MAX_DRIVER_OPS]; // operation descriptors
} driver_descriptor_t;

// --- Registration API ---

// Register a driver with the framework. Called during driver init.
// Returns: driver ID (>= 0) on success, -1 if registry is full.
int driver_register(const driver_descriptor_t *desc);

// Get the number of registered drivers.
int driver_get_count(void);

// Snapshot all registered drivers into an output array.
// Calls each driver's get_stats callback to collect current state.
// Returns: number of drivers written to out[].
int driver_snapshot_all(state_driver_info_t *out, int max_count);
