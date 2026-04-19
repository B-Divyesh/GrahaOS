// kernel/cap/shim_v1.c
// Phase 15a: Legacy CAN-syscall shim.
//
// Every helper here follows the same 4-step pattern:
//   (1) can_find(name) -> can_id
//   (2) read can_entry->cap_object_idx -> obj_idx
//   (3) synthesize an ephemeral cap_token_t from {obj->generation, obj_idx,
//       CAP_FLAG_SHIM_EPHEMERAL}; cap_token_resolve validates it. Bootstrap
//       caps are IMMORTAL|PUBLIC|RIGHTS_ALL so resolve always succeeds.
//   (4) emit klog(TRACE) + dispatch to the original cap_* helper.
//
// Phase 16 deletes this file and rewrites callers to use the v2 syscalls
// directly (SYS_CAP_DERIVE + SYS_CAP_REVOKE_V2 + SYS_CAP_GRANT).

#include "shim_v1.h"
#include "can.h"
#include "object.h"
#include "token.h"
#include "../log.h"

// Phase 16: shim_validate deleted along with the actual shim dispatch. The
// stubs below return -EDEPRECATED directly. File exists as a graveyard
// marker; Phase 17 removes it entirely once no callers remain.

// Phase 16: each shim entry now returns -EDEPRECATED unconditionally. The
// syscall dispatcher (syscall.c) catches the deprecated numbers directly and
// never reaches these; the stubs remain so that out-of-tree / kernel-internal
// callers (if any emerge) still see a coherent "this path is gone" signal.
// The shim_validate helper + klog TRACE is kept so the deprecation is still
// observable at that klog subsystem.
int shim_cap_activate_by_name(const char *name, int32_t caller_pid) {
    (void)name; (void)caller_pid;
    return -78;  // -EDEPRECATED. Literal to keep this file standalone.
}

int shim_cap_deactivate_by_name(const char *name, int32_t caller_pid) {
    (void)name; (void)caller_pid;
    return -78;
}

int shim_cap_watch_by_name(const char *name, int32_t caller_pid) {
    (void)name; (void)caller_pid;
    return -78;
}

int shim_cap_unwatch_by_name(const char *name, int32_t caller_pid) {
    (void)name; (void)caller_pid;
    return -78;
}
