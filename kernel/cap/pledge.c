// kernel/cap/pledge.c
//
// Phase 15b — pledge_init / pledge_narrow / pledge_mask_describe.
// See pledge.h for the semantic overview.
#include "pledge.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "token.h"                           // CAP_V2_EPERM / EINVAL
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"  // task_t full definition
#include "../sync/spinlock.h"
#include "../audit.h"                        // AUDIT_* event types, audit_write

// Initialise a new task's pledge mask. No locking: caller holds the task
// construction sequence serially.
void pledge_init(task_t *task, pledge_mask_t initial) {
    task->pledge_mask = initial;
    spinlock_init(&task->pledge_lock, "pledge");
}

// Atomically narrow a task's pledge mask. Subset rule: (cur & new) == new.
// Rejects widening with CAP_V2_EPERM, reserved-bit / zero masks with
// CAP_V2_EINVAL. On success, writes AUDIT_PLEDGE_NARROW carrying old/new.
// On failure, writes AUDIT_CAP_VIOLATION so that attempted widens are
// visible in the audit log.
int pledge_narrow(task_t *task, pledge_mask_t new_mask) {
    // Quick input validation outside the lock.
    if (new_mask.raw == 0) {
        audit_write_pledge_violation(task->id,
                                     /*old*/ task->pledge_mask.raw,
                                     /*new*/ new_mask.raw,
                                     "pledge mask zero");
        return CAP_V2_EINVAL;
    }
    if ((new_mask.raw & PLEDGE_RESERVED_MASK) != 0) {
        audit_write_pledge_violation(task->id,
                                     task->pledge_mask.raw,
                                     new_mask.raw,
                                     "reserved bits set");
        return CAP_V2_EINVAL;
    }

    spinlock_acquire(&task->pledge_lock);
    pledge_mask_t old = task->pledge_mask;
    if ((old.raw & new_mask.raw) != new_mask.raw) {
        // Widening attempt: one or more bits in new are not in old.
        spinlock_release(&task->pledge_lock);
        audit_write_pledge_violation(task->id,
                                     old.raw,
                                     new_mask.raw,
                                     "pledge widen attempt");
        return CAP_V2_EPERM;
    }
    task->pledge_mask = new_mask;
    spinlock_release(&task->pledge_lock);

    // Announce the narrow. Non-failing: audit_write queues; no lock ordering
    // concern because the pledge_lock is already released.
    audit_write_pledge_narrow(task->id, old.raw, new_mask.raw);
    return 0;
}

// Class-name table for describe. Order MUST match PLEDGE_CLASS_* bit values.
// Extended in Phase 21 with storage_server (12) and input_server (13).
static const char *const k_class_names[14] = {
    "fs_read",
    "fs_write",
    "net_client",
    "net_server",
    "spawn",
    "ipc_send",
    "ipc_recv",
    "sys_query",
    "sys_control",
    "ai_call",
    "compute",
    "time",
    "storage_server",
    "input_server",
};

int pledge_mask_describe(pledge_mask_t mask, char *buf, int buflen) {
    if (buflen <= 0) return 0;
    int written = 0;
    bool first = true;
    for (int i = 0; i < 14; i++) {
        if ((mask.raw & (1u << i)) == 0) continue;
        const char *name = k_class_names[i];
        int name_len = 0;
        while (name[name_len] != '\0') name_len++;
        int needed = (first ? 0 : 1) + name_len;
        if (written + needed + 1 > buflen) break;
        if (!first) buf[written++] = ',';
        for (int k = 0; k < name_len; k++) buf[written++] = name[k];
        first = false;
    }
    buf[written] = '\0';
    return written;
}
