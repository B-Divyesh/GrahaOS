// kernel/cap/system.h
// Phase 26 FU25.F — CAP_KIND_SYSTEM bootcap + system-privileged-op gate.
//
// CAP_KIND_SYSTEM is held by privileged tasks (init at boot; can be derived
// to children via SYS_CAP_DERIVE + SYS_CAP_GRANT). It gates operations
// whose blast radius is system-wide:
//   - TXN_FLAG_GLOBAL_SCOPE in txn_begin (Phase 26 first consumer)
//   - Future: WASM privileged module instantiation, system-monitor caps, etc.
//
// Lifecycle:
//   1. cap_system_init()                         — boot; creates the bootcap
//      cap_object owned by PID_KERNEL with audience = [PID_KERNEL].
//   2. cap_system_install_to_pid(init_pid, …)    — autorun_register_init_pid
//      hook; derives a sub-token with RIGHT_INSPECT|RIGHT_REVOKE|
//      RIGHT_DERIVE|RIGHT_INVOKE, audience = [init_pid], inserts into
//      init's cap_handle_table.
//   3. cap_system_resolve(caller_pid, rights)    — hot-path check used by
//      txn_begin and any future CAP_KIND_SYSTEM-gated syscall. Walks the
//      caller's handle table; returns 0 on a match with sufficient rights,
//      negative CAP_V2_EPERM otherwise.
//
// The bootcap is the cascade root for diminishing-derive — init can grant
// narrowed sub-tokens to trusted daemons (wasmd, future system-monitor)
// without ever lending RIGHTS_ALL. Revoke at the bootcap cascades.
//
// Rationale for gating in the kernel rather than userspace:
//   - txn_begin runs entirely in the kernel; the syscall handler is the
//     only place to enforce.
//   - Userspace cannot synthesise CAP_KIND_SYSTEM tokens — only cap_derive
//     of an existing one.
//   - The check is cheap: O(handle-table size); typical < 1 µs.
#pragma once

#include <stdint.h>

// One-time init at boot (called from kernel/main.c after cap_object_init).
// Idempotent: repeated calls return without re-creating the bootcap.
void cap_system_init(void);

// Returns the cap_object idx of the bootcap (CAP_KIND_SYSTEM root). Returns
// 0 if cap_system_init has not run or failed. Used by cap_system_install_to_pid
// internally and by future inspection paths.
uint32_t cap_system_bootcap_idx(void);

// Derive a sub-cap from the bootcap with rights_subset (must be a subset of
// the bootcap's rights), audience = [pid], and install it into pid's
// cap_handle_table. Called from autorun_register_init_pid for init; can be
// called later for trusted daemons (e.g. wasmd) once they're spawned.
//
// Returns 0 on success; negative CAP_V2_* on failure (no bootcap, target pid
// not found, derive failed, handle-table full, etc.).
int cap_system_install_to_pid(int32_t pid, uint64_t rights_subset);

// Walk the calling task's cap_handle_table looking for any CAP_KIND_SYSTEM
// cap_object whose rights cover required_rights. Returns 0 on match;
// CAP_V2_EPERM if no match (caller lacks the cap or the cap was revoked).
//
// `caller_pid` is the calling task's PID; we look up the task via
// sched_get_task_any() (handles dispatching from kernel context with a
// possibly-zombie caller — same convention as txn_force_drop).
int cap_system_resolve(int32_t caller_pid, uint64_t required_rights);
