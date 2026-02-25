// kernel/capability.h
// Phase 8b-i: Capability Activation Network (CAN)
//
// Every system component - hardware, drivers, services, applications - is a
// "switch" with ON/OFF state and declared dependencies. To use something, you
// just activate it. The system recursively activates all dependencies first.
//
// KEY BENEFIT: When any dependency fails or goes offline, every component that
// depends on it is immediately identifiable. If the mouse driver dies, the system
// instantly knows the cursor, UI toolkit, and every application using the mouse
// are affected - no debugging, no guesswork. The dependency chain IS the
// diagnostic. `why_not` walks the chain and tells you exactly what's wrong.
//
// This is control-plane only - manages component lifecycle, not data-plane I/O.
// Zero overhead on hot paths (read/write/spawn).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "state.h"

// --- Capability Type (Layer) Constants ---
// Lower layers cannot depend on higher layers.
// Same-layer deps allowed ONLY for DRIVER and SERVICE.
#define CAP_HARDWARE    0   // Leaf nodes, kernel-only, no deps
#define CAP_DRIVER      1   // Kernel-only, deps on HW/DRIVER
#define CAP_SERVICE     2   // Kernel-only, deps on HW/DRIVER/SERVICE
#define CAP_APPLICATION 3   // Can be user-owned, deps on layers 0-2
#define CAP_FEATURE     4   // Can be user-owned, deps on layers 0-3
#define CAP_COMPOSITE   5   // Can be user-owned, deps on layers 0-4

// --- Capability State Constants ---
#define CAP_STATE_OFF      0   // Inactive
#define CAP_STATE_STARTING 1   // Activation in progress (cycle guard)
#define CAP_STATE_ON       2   // Active
#define CAP_STATE_ERROR    3   // Activation failed

// --- Limits ---
#define MAX_CAPABILITIES  64
#define MAX_CAP_DEPS       8
#define CAP_NAME_LEN      32
#define MAX_CAP_OPS        8
#define CAP_OP_NAME_LEN   24

// --- Error Codes ---
#define CAP_OK                 0
#define CAP_ERR_NAME_EMPTY    -1
#define CAP_ERR_NAME_DUPLICATE -2
#define CAP_ERR_DEP_UNRESOLVED -3
#define CAP_ERR_DEP_SELF      -4
#define CAP_ERR_LAYER_VIOLATION -5
#define CAP_ERR_REGISTRY_FULL -6
#define CAP_ERR_HW_HAS_DEPS  -7
#define CAP_ERR_NOT_FOUND     -8
#define CAP_ERR_ACTIVATE_FAIL -9
#define CAP_ERR_DEP_FAILED    -10
#define CAP_ERR_CYCLE         -11

// --- Operation Descriptor ---
typedef struct {
    char     name[CAP_OP_NAME_LEN];
    uint32_t param_count;
    uint32_t flags;   // 0=query, 1=mutating
} cap_op_t;

// --- Capability Structure ---
typedef struct {
    char     name[CAP_NAME_LEN];
    uint32_t type;               // CAP_HARDWARE..CAP_COMPOSITE
    uint32_t state;              // CAP_STATE_*
    int32_t  owner_pid;          // -1=kernel, >=0=user process
    uint32_t dep_count;
    uint32_t dep_indices[MAX_CAP_DEPS];  // resolved indices into registry
    char     dep_names[MAX_CAP_DEPS][CAP_NAME_LEN]; // original names

    // Callbacks
    int  (*activate_fn)(void);       // called on activation, NULL=always succeeds
    void (*deactivate_fn)(void);     // called on deactivation, NULL=noop

    // Stats (reuses driver framework pattern)
    int  (*get_stats_fn)(state_driver_stat_t *stats, int max);

    // Operation descriptors
    uint32_t op_count;
    cap_op_t ops[MAX_CAP_OPS];

    // Telemetry
    uint64_t activation_count;
    uint32_t error_dep;          // index of dep that caused last failure
    uint32_t compiled;           // 1=passed all compilation passes
} capability_t;

// --- Registration API ---

// Register a capability with the CAN.
// 3-pass compiler: (1) lexical, (2) dep resolution, (3) layer checking.
// HARDWARE caps are set to ON immediately. All others start OFF.
// Returns: cap_id (>=0) on success, negative CAP_ERR_* on failure.
int cap_register(const char *name, uint32_t type, int32_t owner,
                 const char **dep_names, int dep_count,
                 int (*activate_fn)(void), void (*deactivate_fn)(void),
                 cap_op_t *ops, int op_count,
                 int (*get_stats)(state_driver_stat_t*, int));

// --- Activation API ---

// Activate a capability by ID. Recursively activates dependencies first.
// Returns: 0 on success, negative error on failure.
int cap_activate(int cap_id);

// Deactivate a capability by ID. Cascades to dependents first.
// Returns: 0 on success, negative on failure.
int cap_deactivate(int cap_id);

// --- Query API ---

// Find a capability by name. Returns cap_id or CAP_ERR_NOT_FOUND.
int cap_find(const char *name);

// Get number of registered capabilities.
int cap_get_count(void);

// Get state of a capability by ID.
int cap_get_state(int cap_id);

// Explain why a capability cannot be activated.
// Writes human-readable explanation into buf.
// Returns: 0 on success (meaning it IS activatable), negative if not.
int cap_why_not(int cap_id, char *buf, int buflen);

// Snapshot all capabilities into user-space buffer.
// Returns: number of entries written.
int cap_query_all(state_cap_entry_t *out, int max);

// --- Helper ---

// Convenience function to set up a cap_op_t entry (avoids manual string copy in drivers)
void cap_op_set(cap_op_t *op, const char *name, uint32_t param_count, uint32_t flags);
