// kernel/cap/wasm.c
// Phase 26 Stage D — CAP_KIND_WASM_INSTANCE substrate implementation.
//
// Spec block (cap_kind_wasm_instance_t) is slab-allocated alongside the
// cap_object_t and stored as kind_data. cap_wasm_instance_create returns
// the cap_object slot index; the caller (wasmd) installs that into its
// own handle table via cap_handle_insert.
//
// task_exit hook walks the global cap_object array and revokes every
// CAP_KIND_WASM_INSTANCE whose owner_pid (worker) or parent_pid (wasmd)
// matches the dying PID — same pattern as txn_task_exit_cleanup.

#include "wasm.h"
#include "token.h"
#include "object.h"
#include "handle_table.h"

#include "../log.h"
#include "../mm/kheap.h"
#include "../mm/slab.h"
#include "../sync/spinlock.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// External slab cache for the spec blocks. Slab is convenient: every block
// is exactly 64 B, the allocation rate is bounded by MAX_CONCURRENT_INSTANCES
// (16 per spec test #9; pre-Phase-28 sweep may bump).
static kmem_cache_t *g_cap_wasm_spec_cache = NULL;
static spinlock_t    g_cap_wasm_init_lock  = SPINLOCK_INITIALIZER("cap_wasm_init");

void cap_wasm_instance_init(void) {
    spinlock_acquire(&g_cap_wasm_init_lock);
    if (g_cap_wasm_spec_cache) {
        spinlock_release(&g_cap_wasm_init_lock);
        return;  // idempotent
    }
    g_cap_wasm_spec_cache = kmem_cache_create(
        "cap_wasm_spec",
        sizeof(cap_kind_wasm_instance_t),
        _Alignof(cap_kind_wasm_instance_t),
        NULL,                  // ctor
        SUBSYS_CAP);           // default subsys tag
    spinlock_release(&g_cap_wasm_init_lock);
    if (!g_cap_wasm_spec_cache) {
        klog(KLOG_ERROR, SUBSYS_CAP,
             "cap_wasm_instance_init: kmem_cache_create failed");
        return;
    }
    klog(KLOG_INFO, SUBSYS_CAP,
         "cap_wasm_instance_init: spec slab ready (size=%u, align=%u)",
         (unsigned)sizeof(cap_kind_wasm_instance_t),
         (unsigned)_Alignof(cap_kind_wasm_instance_t));
}

// Tiny in-place memcpy + bounded copy with NUL-termination guarantee. Avoids
// pulling in libk just for this — same pattern as cap_system.c.
static void copy_module_name(char *dst, const char *src) {
    if (!dst) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    int i;
    for (i = 0; i < (WASM_INSTANCE_NAME_MAX - 1) && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int cap_wasm_instance_create(int32_t parent_pid, int32_t worker_pid,
                             uint64_t instance_id, const char *module_name,
                             uint64_t rights_summary) {
    if (!g_cap_wasm_spec_cache) {
        klog(KLOG_ERROR, SUBSYS_CAP,
             "cap_wasm_instance_create: subsystem not initialised");
        return CAP_V2_EINVAL;
    }

    cap_kind_wasm_instance_t *spec =
        (cap_kind_wasm_instance_t *)kmem_cache_alloc(g_cap_wasm_spec_cache);
    if (!spec) {
        klog(KLOG_ERROR, SUBSYS_CAP,
             "cap_wasm_instance_create: spec slab exhausted");
        return CAP_V2_ENOMEM;
    }

    spec->owner_pid      = worker_pid;
    spec->parent_pid     = parent_pid;
    spec->instance_id    = instance_id;
    spec->started_ns     = 0;  // wasmd fills in via SYS_KCLOCK_NS at minted time
    spec->rights_summary = rights_summary;
    copy_module_name(spec->module_name, module_name);

    // Audience starts as { parent_pid, worker_pid } — both processes can
    // resolve the handle. Sub-tokens derived later may narrow audience.
    int32_t aud[3] = { parent_pid, worker_pid, PID_NONE };

    int idx = cap_object_create(
        CAP_KIND_WASM_INSTANCE,
        rights_summary,
        aud,
        CAP_FLAG_EAGER_REVOKE,
        (uintptr_t)spec,
        parent_pid,
        CAP_OBJECT_IDX_NONE
    );
    if (idx < 1) {
        kmem_cache_free(g_cap_wasm_spec_cache, spec);
        klog(KLOG_ERROR, SUBSYS_CAP,
             "cap_wasm_instance_create: cap_object_create rc=%d", idx);
        return idx;
    }

    klog(KLOG_INFO, SUBSYS_CAP,
         "cap_wasm_instance_create: idx=%d module='%s' worker=%d parent=%d id=%llu",
         idx, spec->module_name, worker_pid, parent_pid,
         (unsigned long long)instance_id);
    return idx;
}

cap_kind_wasm_instance_t *cap_wasm_instance_resolve(int32_t caller_pid,
                                                    uint32_t handle) {
    (void)caller_pid;  // R6: future syscalls add a real handle resolution
    if (handle == 0 || handle >= CAP_OBJECT_CAPACITY) return NULL;
    cap_object_t *obj = cap_object_get(handle);
    if (!obj) return NULL;
    if (obj->kind != CAP_KIND_WASM_INSTANCE) return NULL;
    return (cap_kind_wasm_instance_t *)(uintptr_t)obj->kind_data;
}

void cap_wasm_task_exit_cleanup(int32_t dying_pid) {
    if (!g_cap_wasm_spec_cache) return;  // never initialised — nothing to do
    int revoked = 0;
    for (uint32_t i = 1; i < CAP_OBJECT_CAPACITY; i++) {
        cap_object_t *obj = g_cap_object_ptrs[i];
        if (!obj || obj->kind != CAP_KIND_WASM_INSTANCE) continue;
        cap_kind_wasm_instance_t *spec =
            (cap_kind_wasm_instance_t *)(uintptr_t)obj->kind_data;
        if (!spec) continue;
        if (spec->owner_pid != dying_pid && spec->parent_pid != dying_pid) {
            continue;
        }
        // Revoke the cap_object. CAP_FLAG_EAGER_REVOKE cascades to derived
        // tokens automatically. We do NOT free the spec block here — the
        // cap_object_revoke handler is the canonical owner; reaper will
        // free it after watch list drains. (This matches the txn pattern.)
        if (cap_object_revoke(i) == CAP_V2_OK) {
            revoked++;
        }
    }
    if (revoked > 0) {
        klog(KLOG_INFO, SUBSYS_CAP,
             "cap_wasm_task_exit_cleanup: dying pid=%d, revoked %d wasm caps",
             dying_pid, revoked);
    }
}
