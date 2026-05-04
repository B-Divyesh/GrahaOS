// kernel/snap/snapshot.c
// Phase 24 — COW Snapshot Subsystem.
//
// W13 (skeleton): slab caches + barrier zero + ENOSYS stubs.
// W14 (partial, this iteration): lifecycle scaffolding.
//   - snap_create allocates a snapshot_t, assigns a unique monotonic id,
//     creates a CAP_KIND_SNAPSHOT cap_object, installs a handle in the
//     caller's handle table, links the snapshot into g_snap_live_head.
//   - snap_delete looks up the handle, revokes the cap_object, unlinks
//     the snapshot, returns the body to the slab cache.
//   - snap_list walks g_snap_live_head and copies snap_info_t records to
//     the caller's user buffer.
//
// What W14 does NOT do yet (deferred — each is its own focused unit so
// the scheduler/PML4 work can land carefully):
//   - W14.1 snap_begin_barrier / snap_end_barrier (atomic flag + IPI all
//     CPUs + 100 ms watchdog).
//   - W14.2 sched.c::schedule() barrier hook.
//   - W14.3 snap_capture_tasks (regs / FD-table / pledge copy).
//   - W14.4 snap_clone_task_pagetables (PML4 deep walk, mark RO + bump
//     cow_page_tracker_t refcount).
//   - W14.5 snap_capture_vmos (refcount bump + RO mark in mappers).
//   - W14.6 snap_capture_channels (freeze in-scope, drain out-of-scope).
//   - W14.7 snap_capture_fs_pins (grahafs_pin_version per open inode).
//
// snap_restore stays -ENOSYS until W16; cow_fault_handle stays -ENOSYS
// until W15; chan_freeze/thaw/drain stay -ENOSYS until W18.

#include "snapshot.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mm/slab.h"
#include "../log.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/token.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// Kernel uses raw -errno numeric constants (matches user/syscalls.h ABI).
#define SNAP_EPERM    1
#define SNAP_ENOMEM  12
#define SNAP_EINVAL  22
#define SNAP_EBUSY   16
#define SNAP_ENOSYS  38
#define SNAP_EFAULT  14
#define SNAP_ESTALE 116

// ---------------------------------------------------------------------------
// Globals (referenced by snapshot.h externs).
// ---------------------------------------------------------------------------
snap_barrier_state_t g_snap_barrier;
uint64_t             g_snap_next_id;
uint32_t             g_snap_live_count;

// Slab caches. Created in snap_init.
static kmem_cache_t *snap_cache;

// W15: cow_init() (in cow_fault.c) owns the cow_page_tracker_t cache + hash
// table + PF handler registration. snap_init() invokes it after slab setup.
extern void cow_init(void);

// Live-snapshot list head + lock.
static snapshot_t   *g_snap_live_head;
static spinlock_t    g_snap_live_lock;

// ---------------------------------------------------------------------------
// snap_init — one-time bootstrap.
// ---------------------------------------------------------------------------
void snap_init(void) {
    if (snap_cache) return;  // idempotent guard

    snap_cache = kmem_cache_create("snapshot_t",
                                   sizeof(snapshot_t),
                                   _Alignof(snapshot_t),
                                   /*ctor=*/NULL,
                                   SUBSYS_CORE);
    if (!snap_cache) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "snap_init: kmem_cache_create(snapshot_t) failed");
        return;
    }

    // W15: install the COW page-fault handler + cow_page_tracker hash.
    cow_init();

    memset(&g_snap_barrier, 0, sizeof(g_snap_barrier));
    spinlock_init(&g_snap_barrier.lock, "snap_barrier");
    g_snap_next_id    = 1;
    g_snap_live_count = 0;
    g_snap_live_head  = NULL;
    spinlock_init(&g_snap_live_lock, "snap_live");

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_init: slab caches registered, barrier zeroed, next_id=%lu",
         (unsigned long)g_snap_next_id);
}

// ---------------------------------------------------------------------------
// Live-list helpers (caller holds g_snap_live_lock).
// ---------------------------------------------------------------------------
static void snap_link_locked(snapshot_t *s) {
    s->prev = NULL;
    s->next = g_snap_live_head;
    if (g_snap_live_head) g_snap_live_head->prev = s;
    g_snap_live_head = s;
    g_snap_live_count++;
}

static void snap_unlink_locked(snapshot_t *s) {
    if (s->prev) s->prev->next = s->next;
    else         g_snap_live_head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
    if (g_snap_live_count) g_snap_live_count--;
}

// ---------------------------------------------------------------------------
// snap_create_internal — Phase 25 kernel-internal entry.
//
// Same capture semantics as snap_create but does NOT issue a cap_object or
// install a handle in the caller's table. Returns 0 on success with
// *out_snap pointing at the fresh snapshot_t (already linked into
// g_snap_live_head); negative -errno on failure.
//
// txn_begin uses this so the caller's handle table sees only ONE handle
// (CAP_KIND_TRANSACTION) rather than two (CAP_KIND_SNAPSHOT +
// CAP_KIND_TRANSACTION) for what is logically a single primitive.
// ---------------------------------------------------------------------------
int snap_create_internal(uint32_t scope_flags, const char *name,
                         snapshot_t **out_snap) {
    if (out_snap) *out_snap = NULL;
    if (!snap_cache) return -SNAP_EINVAL;

    if (scope_flags & ~SNAP_SCOPE_VALID_MASK) return -SNAP_EINVAL;
    if ((scope_flags & (SNAP_SCOPE_SELF | SNAP_SCOPE_GLOBAL)) == 0) {
        return -SNAP_EINVAL;
    }

    task_t *current = sched_get_current_task();
    if (!current) return -SNAP_EPERM;

    snapshot_t *s = (snapshot_t *)kmem_cache_alloc(snap_cache);
    if (!s) return -SNAP_ENOMEM;

    memset(s, 0, sizeof(*s));
    s->scope_flags = scope_flags;
    s->state       = SNAP_STATE_ACTIVE;
    s->creator_pid = current->id;
    s->created_utc_ns = 0;

    if (name) {
        size_t n = 0;
        while (n < SNAP_NAME_MAX_LEN && name[n] != '\0') {
            s->name[n] = name[n];
            n++;
        }
        s->name[n] = '\0';
    }

    uint64_t id = __atomic_fetch_add(&g_snap_next_id, 1, __ATOMIC_RELAXED);
    s->id = id;

    // Capture under the scheduler barrier (same as snap_create).
    int barrier_rc = snap_begin_barrier();
    if (barrier_rc != 0) {
        kmem_cache_free(snap_cache, s);
        klog(KLOG_WARN, SUBSYS_CORE,
             "snap_create_internal: snap_begin_barrier rc=%d", barrier_rc);
        return barrier_rc;
    }

    int cap_rc = snap_run_capture(s, current);
    snap_end_barrier();

    if (cap_rc < 0) {
        snap_destroy_captures(s);
        kmem_cache_free(snap_cache, s);
        klog(KLOG_WARN, SUBSYS_CORE,
             "snap_create_internal: snap_run_capture rc=%d", cap_rc);
        return cap_rc;
    }

    spinlock_acquire(&g_snap_live_lock);
    snap_link_locked(s);
    spinlock_release(&g_snap_live_lock);

    if (out_snap) *out_snap = s;

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_create_internal: id=%lu pid=%d scope=0x%x tasks=%u pages=%lu fs_pins=%u name='%s'",
         (unsigned long)id, current->id, (unsigned)scope_flags,
         (unsigned)s->task_count, (unsigned long)s->pages_shared,
         (unsigned)s->fs_pin_count, s->name);
    return 0;
}

// ---------------------------------------------------------------------------
// snap_destroy_internal — Phase 25 kernel-internal counterpart of snap_delete.
// Caller has already dropped any cap_handle / cap_object linkage.
// ---------------------------------------------------------------------------
void snap_destroy_internal(snapshot_t *s) {
    if (!s) return;
    if (!snap_cache) return;

    spinlock_acquire(&g_snap_live_lock);
    if (s->state != SNAP_STATE_DELETED) {
        s->state = SNAP_STATE_DELETED;
        snap_unlink_locked(s);
    }
    spinlock_release(&g_snap_live_lock);

    snap_destroy_captures(s);
    kmem_cache_free(snap_cache, s);
}

// ---------------------------------------------------------------------------
// snap_create — Phase 24 syscall entry. Now a thin wrapper around
// snap_create_internal that adds a CAP_KIND_SNAPSHOT cap_object + inserts
// a handle in the caller's table. Stage D refactor: every behavioural
// change goes in snap_create_internal so txn_begin shares it.
// ---------------------------------------------------------------------------
int snap_create(uint32_t scope_flags, const char *name) {
    snapshot_t *s = NULL;
    int rc = snap_create_internal(scope_flags, name, &s);
    if (rc < 0) return rc;
    if (!s) return -SNAP_EINVAL;

    task_t *current = sched_get_current_task();
    if (!current) {
        // Should not happen — snap_create_internal already checked. Guard
        // anyway so we don't leak the snapshot on the impossible path.
        snap_destroy_internal(s);
        return -SNAP_EPERM;
    }

    // Allocate the kernel-side cap_object. W19.4: snapshot tokens carry
    // RIGHT_RESTORE + RIGHT_DELETE alongside RIGHT_INSPECT / RIGHT_REVOKE
    // / RIGHT_DERIVE.
    int32_t audience[2] = { current->id, PID_NONE };
    int obj_idx = cap_object_create(CAP_KIND_SNAPSHOT,
                                    /*rights=*/RIGHT_INSPECT | RIGHT_REVOKE
                                              | RIGHT_DERIVE
                                              | RIGHT_RESTORE | RIGHT_DELETE,
                                    audience,
                                    /*flags=*/0,
                                    /*kind_data=*/(uintptr_t)s,
                                    current->id,
                                    CAP_OBJECT_IDX_NONE);
    if (obj_idx < 0) {
        snap_destroy_internal(s);
        return -SNAP_ENOMEM;
    }

    uint32_t slot = CAP_HANDLE_SLOT_NONE;
    int ins = cap_handle_insert(&current->cap_handles,
                                (uint32_t)obj_idx,
                                /*token_flags=*/0,
                                &slot);
    if (ins < 0) {
        cap_object_revoke((uint32_t)obj_idx);
        cap_object_destroy((uint32_t)obj_idx);
        snap_destroy_internal(s);
        return ins == CAP_V2_ENOMEM ? -SNAP_ENOMEM : -SNAP_EINVAL;
    }

    cap_object_t *obj = cap_object_get((uint32_t)obj_idx);
    s->cap_token = cap_token_pack(obj ? obj->generation : 1u,
                                  (uint32_t)obj_idx, /*flags=*/0);

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_create: id=%lu slot=%u obj=%u name='%s'",
         (unsigned long)s->id, (unsigned)slot, (unsigned)obj_idx, s->name);

    return (int)slot;
}

// ---------------------------------------------------------------------------
// snap_delete — W14 partial: revoke the cap_object, unlink, free body.
// Page reclamation (W17) lands once W14.4 has actually populated the COW
// trackers.
// ---------------------------------------------------------------------------
int snap_delete(uint32_t handle) {
    if (!snap_cache) return -SNAP_EINVAL;

    task_t *current = sched_get_current_task();
    if (!current) return -SNAP_EPERM;

    cap_handle_entry_t *entry = cap_handle_lookup(&current->cap_handles, handle);
    if (!entry) return -SNAP_EINVAL;

    cap_object_t *obj = cap_object_get(entry->object_idx);
    if (!obj || obj->kind != CAP_KIND_SNAPSHOT) return -SNAP_EINVAL;

    snapshot_t *s = (snapshot_t *)obj->kind_data;
    if (!s) return -SNAP_EINVAL;
    if (s->state == SNAP_STATE_RESTORING) return -SNAP_EBUSY;

    // Capture identifiers before any frees: entry points into a slot that
    // we're about to release, and obj/s themselves get freed below.
    uint32_t obj_idx = entry->object_idx;
    uint64_t snap_id = s->id;

    // 1. Drop the caller's handle FIRST so concurrent syscalls on this
    //    pid cannot resolve the token after this point.
    cap_handle_remove(&current->cap_handles, handle);

    // 2. Mark deleted + unlink under the live list lock. Concurrent
    //    snap_list readers either see state == ACTIVE before this point
    //    (and copy out the record) or state == DELETED after (and skip).
    spinlock_acquire(&g_snap_live_lock);
    s->state = SNAP_STATE_DELETED;
    snap_unlink_locked(s);
    spinlock_release(&g_snap_live_lock);

    // 3. Revoke + destroy the cap_object. cap_object_destroy slab-frees
    //    the kernel-side cap_object_t; the snapshot body is freed last
    //    so nothing dereferences a stale obj->kind_data.
    cap_object_revoke(obj_idx);
    cap_object_destroy(obj_idx);

    // Phase 24 W17.1: drop every capture-side ref (cow_page_tracker_put,
    // pmm_page_unref, grahafs_unpin_version). After this the captured
    // physical pages are reclaimable by the page allocator and the
    // grahafs version chains can be GC'd by gc_prune_inode.
    snap_destroy_captures(s);

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_delete: id=%lu pid=%d slot=%u",
         (unsigned long)snap_id, current->id, (unsigned)handle);

    // 4. Free the snapshot body itself.
    kmem_cache_free(snap_cache, s);
    return 0;
}

// ---------------------------------------------------------------------------
// snap_list — W14 partial: enumerate live snapshots into user buffer.
// ---------------------------------------------------------------------------
int snap_list(snap_info_t *user_buf, size_t count) {
    if (!snap_cache) return -SNAP_EINVAL;
    if (count > 0 && user_buf == NULL) return -SNAP_EINVAL;

    size_t written = 0;
    spinlock_acquire(&g_snap_live_lock);
    for (snapshot_t *s = g_snap_live_head; s && written < count; s = s->next) {
        if (s->state != SNAP_STATE_ACTIVE) continue;
        snap_info_t info;
        memset(&info, 0, sizeof(info));
        info.id              = s->id;
        info.created_utc_ns  = s->created_utc_ns;
        info.creator_pid     = s->creator_pid;
        info.scope_flags     = s->scope_flags;
        info.state           = s->state;
        info.task_count      = s->task_count;
        info.vmo_count       = s->vmo_count;
        info.chan_count      = s->chan_count;
        info.pages_shared    = s->pages_shared;
        info.pages_diverged  = s->pages_diverged;
        memcpy(info.name, s->name, sizeof(info.name));
        // Direct memcpy: the caller's syscall handler already validated
        // the buffer via is_user_pointer. user_buf is mapped in the
        // current address space.
        memcpy(&user_buf[written], &info, sizeof(info));
        written++;
    }
    spinlock_release(&g_snap_live_lock);
    return (int)written;
}

// ---------------------------------------------------------------------------
// W14.1 / W14.2 / W15-W18 stubs — bodies arrive in their own units.
// ---------------------------------------------------------------------------

// snap_restore lives in kernel/snap/restore.c (W16).
// snap_begin_barrier / snap_end_barrier live in kernel/snap/barrier.c (W14.1).

// cow_fault_handle / cow_page_tracker_get / _put / _bump live in cow_fault.c
// (W15). The stubs that used to live here returned -SNAP_ENOSYS; now we
// have real implementations.

// chan_freeze / chan_thaw / chan_drain_to_vmo / chan_redrain_from_vmo
// live in kernel/snap/chan_freeze.c (W18). chan_freeze + chan_thaw have
// real implementations; the drain helpers stay -ENOSYS until W14.6
// captures need them.
