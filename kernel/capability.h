// kernel/capability.h
// Phase 8b: Capability Activation Network (CAN)
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
// 6-pass registration compiler:
//   Pass 1: Lexical validation (name format, uniqueness)
//   Pass 2: Dependency resolution (names -> indices)
//   Pass 3: Layer checking (no upward dependencies)
//   Pass 4: Cycle detection (DFS safety check)
//   Pass 5: Reachability analysis (advisory: warns if no path to HARDWARE)
//   Pass 6: Transitive redundancy detection (advisory: warns about redundant deps)
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

// --- Driver Subtype Constants ---
// Used in capability_t.subtype for CAP_DRIVER/CAP_SERVICE caps.
// Replaces the old DRIVER_TYPE_* constants from driver.h.
#define CAP_SUBTYPE_BLOCK    0
#define CAP_SUBTYPE_INPUT    1
#define CAP_SUBTYPE_DISPLAY  2
#define CAP_SUBTYPE_TIMER    3
#define CAP_SUBTYPE_SERIAL   4
#define CAP_SUBTYPE_FS       5
#define CAP_SUBTYPE_OTHER    255

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
#define CAP_ERR_KERNEL_OWNED  -12
#define CAP_ERR_DELETED       -14
#define CAP_ERR_WATCH_FULL    -15   // Watcher list full (8 max)
#define CAP_ERR_ALREADY_WATCH -16   // Already watching this cap
#define CAP_ERR_NOT_WATCHING  -17   // Not watching this cap

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
    uint32_t subtype;            // CAP_SUBTYPE_* for DRIVER/SERVICE caps, 0 otherwise
    uint32_t deleted;            // 1=slot marked deleted by cap_unregister

    // Phase 8d: Event watchers
    int32_t  watcher_pids[8];    // PIDs watching this capability
    uint32_t watcher_count;      // Number of active watchers (slots 0..watcher_count-1)
} capability_t;

// --- Registration API ---

// Register a capability with the CAN.
// 6-pass compiler: (1) lexical, (2) dep resolution, (3) layer checking,
// (4) cycle detection, (5) reachability analysis, (6) transitive redundancy.
// HARDWARE caps are set to ON immediately. All others start OFF.
// subtype: CAP_SUBTYPE_* for DRIVER/SERVICE caps, 0 for others.
// Returns: cap_id (>=0) on success, negative CAP_ERR_* on failure.
int cap_register(const char *name, uint32_t type, uint32_t subtype, int32_t owner,
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

// --- Unregistration API ---

// Unregister a capability. Only user-owned APPLICATION/FEATURE/COMPOSITE.
// Deactivates first if active (cascading). Marks slot as deleted.
// Returns: 0 on success, negative error on failure.
int cap_unregister(int cap_id);

// Unregister all capabilities owned by a process. Called from SYS_EXIT.
void cap_unregister_by_owner(int32_t owner_pid);

// Get owner PID for a capability. Returns -1 if not found or kernel-owned.
int32_t cap_get_owner(int cap_id);

// --- Driver Compatibility ---

// Snapshot driver-type capabilities into state_driver_info_t format.
// Replaces the old driver_snapshot_all() from driver.c.
// Iterates DRIVER/SERVICE caps, calls get_stats_fn for live stats.
// Returns: number of entries written.
int cap_snapshot_drivers(state_driver_info_t *out, int max);

// --- Phase 8d: Event Watching ---

// Watch a capability for state changes. Events delivered to process event queue.
int cap_watch(int cap_id, int32_t pid);

// Stop watching a capability.
int cap_unwatch(int cap_id, int32_t pid);

// Remove all watchers owned by a process. Called from SYS_EXIT.
void cap_unwatch_all_for_pid(int32_t pid);

// --- Helper ---

// Convenience function to set up a cap_op_t entry (avoids manual string copy in drivers)
void cap_op_set(cap_op_t *op, const char *name, uint32_t param_count, uint32_t flags);
