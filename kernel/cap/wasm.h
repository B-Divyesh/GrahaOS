// kernel/cap/wasm.h
//
// Phase 26 Stage D — CAP_KIND_WASM_INSTANCE substrate.
//
// One running WebAssembly module instance becomes one cap_object_t with
// kind=CAP_KIND_WASM_INSTANCE. The token's holder gets RIGHT_INSPECT (read
// status) and RIGHT_TERMINATE (kill the worker subprocess + revoke caps it
// inherited). wasmd is the only process expected to mint these tokens; it
// hands them out to operator CLI (`bin/wasm`) and to other daemons that
// need to monitor or terminate a sandbox.
//
// Per spec: when wasmd's worker process exits (clean or crashed), wasmd
// calls cap_object_revoke on the token; subsequent attempts to use the
// handle return -EREVOKED.
//
// task_exit hook (R5): when ANY task with active wasm instances dies,
// wasm_task_exit_cleanup walks objects with owner_pid==dying and revokes
// each. Same pattern as txn_task_exit_cleanup.
#pragma once

#include <stdint.h>

#define WASM_INSTANCE_NAME_MAX  32

// Specialisation block stored in cap_object_t.kind_data (cast to uintptr_t).
// 64 bytes (multiple of 8 for natural alignment); slab-allocated alongside
// the cap_object_t when CAP_KIND_WASM_INSTANCE is created.
//
// Phase 29 Session G (FU27.X.wasmd_audit_subscription): audit_subscription_slot
// tracks which AUDIT_SUB_MAX slot is reserved for this worker.  Set at
// cap_wasm_instance_create; -1 if subscribe failed (slots full) or after
// the worker's task_exit hook auto-unsubscribed via audit_unsubscribe_all_for_pid.
typedef struct cap_kind_wasm_instance_s {
    int32_t  owner_pid;       // wasmd-worker PID (the actual sandbox)
    int32_t  parent_pid;      // wasmd daemon PID (the cap minter)
    uint64_t instance_id;     // monotonic, assigned at create time
    uint64_t started_ns;      // boot-relative
    uint64_t rights_summary;  // copy of rights_bitmap for fast read
    int32_t  audit_subscription_slot;  // FU27.X.wasmd_audit_subscription
    uint32_t _pad0;
    char     module_name[WASM_INSTANCE_NAME_MAX];
} cap_kind_wasm_instance_t;

// _Static_assert may be unavailable in this header context; offset checks
// done in wasm.c instead.

// Initialise the wasm instance subsystem. Allocates the specialisation
// slab cache. Called once from kernel/main.c after cap_object_init.
void cap_wasm_instance_init(void);

// Mint a fresh CAP_KIND_WASM_INSTANCE cap_object. The bootcap is granted
// to wasmd (audience = [parent_pid]); wasmd derives sub-caps for clients.
// `module_name` is copied (truncated at WASM_INSTANCE_NAME_MAX-1).
//
// Returns the cap_object index on success, negative cap error on failure.
// Phase 26 Stage E will install the returned handle in wasmd's handle table.
int cap_wasm_instance_create(int32_t parent_pid, int32_t worker_pid,
                             uint64_t instance_id, const char *module_name,
                             uint64_t rights_summary);

// task_exit hook: walk cap_object slab, revoke every CAP_KIND_WASM_INSTANCE
// where owner_pid==dying or parent_pid==dying. Called from sched_task_exit
// after the txn cleanup hook (lock order: cap_object lock).
void cap_wasm_task_exit_cleanup(int32_t dying_pid);

// Resolve a handle to its cap_kind_wasm_instance_t spec block. Returns NULL
// on stale/wrong-kind. Used by future inspect/terminate syscalls (Phase 27+).
cap_kind_wasm_instance_t *cap_wasm_instance_resolve(int32_t caller_pid,
                                                    uint32_t handle);
