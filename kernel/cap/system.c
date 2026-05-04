// kernel/cap/system.c
// Phase 26 FU25.F — CAP_KIND_SYSTEM bootcap implementation.
//
// See system.h for the public-API contract. This file implements:
//   - cap_system_init       (boot-time bootcap creation)
//   - cap_system_bootcap_idx (accessor)
//   - cap_system_install_to_pid (derive + insert into pid's handle table)
//   - cap_system_resolve    (hot-path check used by txn_begin et al)
//
// The bootcap is created with audience = [PID_KERNEL] only — userspace
// cannot resolve it directly. Sub-tokens minted by cap_system_install_to_pid
// have audience = [pid] and RIGHT_INSPECT|RIGHT_REVOKE|RIGHT_DERIVE|
// RIGHT_INVOKE (or whatever the caller passes in rights_subset). Revoke
// at the bootcap cascades through CAP_FLAG_EAGER_REVOKE.

#include "system.h"
#include "token.h"
#include "object.h"
#include "handle_table.h"

#include "../log.h"
#include "../sync/spinlock.h"

#include "../../arch/x86_64/cpu/sched/sched.h"

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Module-private state.
// ----------------------------------------------------------------------------
// The bootcap idx (slot in g_cap_object_ptrs). 0 means uninitialised — slot
// 0 in the object registry is the kernel-owner sentinel and is never used as
// a real bootcap.
static uint32_t   g_cap_system_bootcap_idx = 0;
static spinlock_t g_cap_system_lock = SPINLOCK_INITIALIZER("cap_system_init");

// Default rights mask granted to init at boot. Sub-tokens derived later may
// carry a strict subset.
#define CAP_SYSTEM_INIT_RIGHTS \
    (RIGHT_INSPECT | RIGHT_REVOKE | RIGHT_DERIVE | RIGHT_INVOKE)

// ----------------------------------------------------------------------------
// cap_system_init — boot-time bootcap creation.
// ----------------------------------------------------------------------------
void cap_system_init(void) {
    spinlock_acquire(&g_cap_system_lock);
    if (g_cap_system_bootcap_idx != 0) {
        spinlock_release(&g_cap_system_lock);
        return;  // already initialised; idempotent
    }

    // Audience = [PID_KERNEL] only. Userspace cannot resolve this directly;
    // sub-tokens minted by cap_system_install_to_pid carry the per-task
    // audience.
    int32_t audience[2] = { PID_KERNEL, PID_NONE };

    int idx = cap_object_create(
        CAP_KIND_SYSTEM,
        CAP_SYSTEM_INIT_RIGHTS,
        audience,
        CAP_FLAG_EAGER_REVOKE,   // revoke cascades to children
        (uintptr_t)0,             // no kind_data
        PID_KERNEL,
        CAP_OBJECT_IDX_NONE       // no parent — bootcap is its own root
    );

    if (idx < 1) {
        spinlock_release(&g_cap_system_lock);
        klog(KLOG_ERROR, SUBSYS_CORE,
             "cap_system_init: bootcap creation failed (rc=%d)", idx);
        return;
    }

    g_cap_system_bootcap_idx = (uint32_t)idx;
    spinlock_release(&g_cap_system_lock);

    klog(KLOG_INFO, SUBSYS_CORE,
         "cap_system_init: bootcap idx=%u rights=0x%llx",
         g_cap_system_bootcap_idx,
         (unsigned long long)CAP_SYSTEM_INIT_RIGHTS);
}

uint32_t cap_system_bootcap_idx(void) {
    return g_cap_system_bootcap_idx;
}

// ----------------------------------------------------------------------------
// cap_system_install_to_pid — derive + insert into pid's handle table.
// ----------------------------------------------------------------------------
int cap_system_install_to_pid(int32_t pid, uint64_t rights_subset) {
    if (g_cap_system_bootcap_idx == 0) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "cap_system_install_to_pid: bootcap not initialised; skipping pid=%d",
             (int)pid);
        return CAP_V2_EINVAL;
    }
    if (rights_subset == 0) return CAP_V2_EINVAL;
    if ((rights_subset & ~CAP_SYSTEM_INIT_RIGHTS) != 0) {
        // Rights superset of bootcap's — cap_object_derive would also reject,
        // but check here to give a clearer error message.
        klog(KLOG_WARN, SUBSYS_CORE,
             "cap_system_install_to_pid: rights 0x%llx exceeds bootcap 0x%llx",
             (unsigned long long)rights_subset,
             (unsigned long long)CAP_SYSTEM_INIT_RIGHTS);
        return CAP_V2_EPERM;
    }

    task_t *t = sched_get_task_any(pid);
    if (!t) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "cap_system_install_to_pid: pid=%d not found", (int)pid);
        return CAP_V2_EINVAL;
    }

    // Derive a sub-cap from the bootcap. cap_object_derive validates that
    // rights_subset is a strict subset of the parent's rights, audience is
    // a subset of (or equal to) the parent's audience-or-PUBLIC. Since the
    // bootcap's audience = [PID_KERNEL] and pid != PID_KERNEL, the derived
    // sub-cap will have audience = [pid] explicitly.
    int32_t derived_audience[2] = { pid, PID_NONE };
    int new_idx = cap_object_derive(
        g_cap_system_bootcap_idx,
        PID_KERNEL,                // caller_pid (kernel-initiated derive)
        rights_subset,
        derived_audience,
        CAP_FLAG_INHERITABLE       // child task may carry it forward via spawn
    );
    if (new_idx < 1) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "cap_system_install_to_pid: derive failed pid=%d rc=%d",
             (int)pid, new_idx);
        return new_idx;
    }

    // Install the derived idx into pid's handle table. Token flags = 0
    // (the derived sub-cap is not PUBLIC; it's audience-restricted to pid).
    uint32_t slot = 0;
    int gen = cap_handle_insert(&t->cap_handles, (uint32_t)new_idx,
                                /*token_flags=*/0, &slot);
    if (gen < 0) {
        // Insertion failed — revoke the derived cap to avoid leaking it.
        cap_object_revoke((uint32_t)new_idx);
        klog(KLOG_ERROR, SUBSYS_CORE,
             "cap_system_install_to_pid: handle_insert failed pid=%d rc=%d",
             (int)pid, gen);
        return gen;
    }

    klog(KLOG_INFO, SUBSYS_CORE,
         "cap_system: granted CAP_KIND_SYSTEM to pid=%d (obj_idx=%d slot=%u rights=0x%llx)",
         (int)pid, new_idx, slot, (unsigned long long)rights_subset);
    return 0;
}

// ----------------------------------------------------------------------------
// cap_system_resolve — hot-path check for system-privileged ops.
// ----------------------------------------------------------------------------
int cap_system_resolve(int32_t caller_pid, uint64_t required_rights) {
    if (g_cap_system_bootcap_idx == 0) return CAP_V2_EPERM;
    if (required_rights == 0) return CAP_V2_EINVAL;

    task_t *caller = sched_get_task_any(caller_pid);
    if (!caller) return CAP_V2_EPERM;

    cap_handle_table_t *t = &caller->cap_handles;

    // Walk every live entry. Typical handle tables are small (16-128 slots);
    // this is bounded by CAP_HANDLE_MAX = 1024 in pathological cases. The
    // common case for an init-derived sub-cap is one slot near the top of
    // init's table — under 100 ns to find.
    spinlock_acquire(&t->lock);
    uint32_t cap = t->capacity;
    for (uint32_t i = 0; i < cap; i++) {
        cap_handle_entry_t *e = &t->entries[i];
        if (e->object_idx == CAP_OBJECT_IDX_NONE) continue;

        cap_object_t *obj = cap_object_get(e->object_idx);
        if (!obj) continue;
        if (obj->kind != CAP_KIND_SYSTEM) continue;
        if (obj->deleted) continue;
        if ((obj->rights_bitmap & required_rights) != required_rights) continue;

        // Match: caller holds a CAP_KIND_SYSTEM cap with the required rights.
        spinlock_release(&t->lock);
        return 0;
    }
    spinlock_release(&t->lock);
    return CAP_V2_EPERM;
}
