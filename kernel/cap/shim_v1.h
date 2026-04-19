// kernel/cap/shim_v1.h
// Phase 15a: Legacy CAN-syscall shim declarations.
//
// SYS_CAP_ACTIVATE/DEACTIVATE/REGISTER/UNREGISTER/WATCH/UNWATCH/POLL were
// defined in Phase 8b/8d before cap_token_t existed. Phase 15a routes
// them through a thin shim that (1) synthesizes an ephemeral token over
// the underlying can_entry's paired cap_object_t, (2) validates it via
// cap_token_resolve (exercising the v2 path even though the user-facing
// API is unchanged), (3) dispatches to the original cap_activate /
// cap_deactivate / etc. helpers, and (4) emits a TRACE klog entry so
// Phase 16 can audit per-invocation deprecation signals.
//
// Net effect: zero behavior change for existing callers (cantest 21/21,
// eventtest, gash `activate display`, etc.), but every legacy syscall is
// now observable and routes through the new authority model.

#pragma once

#include <stdint.h>
#include "token.h"

// Activate by name. Returns 0 on success, CAP_ERR_* on failure.
int shim_cap_activate_by_name(const char *name, int32_t caller_pid);

// Deactivate by name. Returns 0 or CAP_ERR_*.
int shim_cap_deactivate_by_name(const char *name, int32_t caller_pid);

// Watch by name. Adds caller_pid to cap's watcher list.
int shim_cap_watch_by_name(const char *name, int32_t caller_pid);

// Unwatch by name.
int shim_cap_unwatch_by_name(const char *name, int32_t caller_pid);
