// kernel/cap/can.c
// Phase 8b: Capability Activation Network (CAN) - Core Engine
// Phase 15a: renamed from kernel/capability.c; moved into kernel/cap/.
// 6-pass registration compiler, recursive activation, cascade deactivation.
#include "can.h"
#include "object.h"
#include "token.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/drivers/serial/serial.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../log.h"
#include "../panic.h"
#include "../mm/slab.h"
#include "../audit.h"

// --- Static string helpers (kernel has no libc) ---

static int cap_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void cap_strcpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int cap_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void cap_memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)c;
}

// --- Registry ---

// Phase 14: can_entry_t objects now live in can_entry_cache (Bonwick
// slab). The registry still indexes by cap_id (0..MAX_CAPABILITIES-1)
// via g_can_entry_ptrs; slot is NULL when free or deleted.
static can_entry_t *g_can_entry_ptrs[MAX_CAPABILITIES];
static kmem_cache_t *can_entry_cache = NULL;
static uint32_t     g_cap_count = 0;
static spinlock_t   g_cap_lock = SPINLOCK_INITIALIZER("cap_lock");

// Lazy-init the slab cache on first cap_register call.
static void can_entry_cache_init_once(void) {
    if (can_entry_cache) return;
    can_entry_cache = kmem_cache_create("can_entry_t",
                                        sizeof(can_entry_t),
                                        _Alignof(can_entry_t),
                                        /*ctor=*/NULL,
                                        SUBSYS_CAP);
    if (!can_entry_cache) {
        kpanic("cap/can.c: kmem_cache_create(can_entry_cache) failed");
    }
}

// --- Helper: cap_op_set ---

void cap_op_set(cap_op_t *op, const char *name, uint32_t param_count, uint32_t flags) {
    if (!op || !name) return;
    cap_strcpy(op->name, name, CAP_OP_NAME_LEN);
    op->param_count = param_count;
    op->flags = flags;
}

// --- Phase 8d: Internal notification helper ---
// Snapshots watcher list under g_cap_lock, then enqueues events (lock-free).
// This avoids holding g_cap_lock while calling into sched_enqueue_cap_event.
static void cap_notify_watchers(int cap_id, uint32_t old_state, uint32_t new_state) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return;

    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);
    if (cap->watcher_count == 0) return;

    // Build event on stack
    state_cap_event_t event;
    cap_memset(&event, 0, sizeof(event));

    // Determine event type
    if (new_state == CAP_STATE_ON)
        event.type = STATE_CAP_EVENT_ACTIVATED;
    else if (new_state == CAP_STATE_OFF)
        event.type = STATE_CAP_EVENT_DEACTIVATED;
    else if (new_state == CAP_STATE_ERROR)
        event.type = STATE_CAP_EVENT_ERROR;
    else
        return;  // Unknown transition, skip

    event.cap_id = (uint32_t)cap_id;
    event.old_state = old_state;
    event.new_state = new_state;
    cap_strcpy(event.cap_name, cap->name, 32);

    // Snapshot watcher PIDs under lock
    int32_t pids[8];
    uint32_t count;

    spinlock_acquire(&g_cap_lock);
    count = cap->watcher_count;
    for (uint32_t i = 0; i < count && i < 8; i++) {
        pids[i] = cap->watcher_pids[i];
    }
    spinlock_release(&g_cap_lock);

    // Deliver events (no CAN lock held)
    for (uint32_t i = 0; i < count; i++) {
        sched_enqueue_cap_event(pids[i], &event);
    }
}

// --- Phase 8d: Watch/Unwatch API ---

int cap_watch(int cap_id, int32_t pid) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    spinlock_acquire(&g_cap_lock);
    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    if (cap->deleted) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_DELETED;
    }

    // Check for duplicate
    for (uint32_t i = 0; i < cap->watcher_count; i++) {
        if (cap->watcher_pids[i] == pid) {
            spinlock_release(&g_cap_lock);
            return CAP_ERR_ALREADY_WATCH;
        }
    }

    // Check capacity
    if (cap->watcher_count >= 8) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_WATCH_FULL;
    }

    cap->watcher_pids[cap->watcher_count] = pid;
    cap->watcher_count++;

    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Watch: pid=%lu -> ", (unsigned long)(pid));
    klog(KLOG_INFO, SUBSYS_CAP, "%s", cap->name);

    spinlock_release(&g_cap_lock);
    return CAP_OK;
}

int cap_unwatch(int cap_id, int32_t pid) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    spinlock_acquire(&g_cap_lock);
    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    if (cap->deleted) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_DELETED;
    }

    // Find the watcher
    int found = -1;
    for (uint32_t i = 0; i < cap->watcher_count; i++) {
        if (cap->watcher_pids[i] == pid) {
            found = (int)i;
            break;
        }
    }

    if (found < 0) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_NOT_WATCHING;
    }

    // Compact: shift remaining entries down
    for (uint32_t i = (uint32_t)found; i < cap->watcher_count - 1; i++) {
        cap->watcher_pids[i] = cap->watcher_pids[i + 1];
    }
    cap->watcher_count--;

    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Unwatch: pid=%lu -> ", (unsigned long)(pid));
    klog(KLOG_INFO, SUBSYS_CAP, "%s", cap->name);

    spinlock_release(&g_cap_lock);
    return CAP_OK;
}

void cap_unwatch_all_for_pid(int32_t pid) {
    spinlock_acquire(&g_cap_lock);

    for (uint32_t c = 0; c < g_cap_count; c++) {
        if (!g_can_entry_ptrs[c]) continue;
        if ((*g_can_entry_ptrs[c]).deleted) continue;
        can_entry_t *cap = g_can_entry_ptrs[c];

        for (uint32_t i = 0; i < cap->watcher_count; i++) {
            if (cap->watcher_pids[i] == pid) {
                // Compact: shift remaining entries down
                for (uint32_t j = i; j < cap->watcher_count - 1; j++) {
                    cap->watcher_pids[j] = cap->watcher_pids[j + 1];
                }
                cap->watcher_count--;
                i--;  // Re-check same index after shift
                break; // Each PID appears at most once per cap
            }
        }
    }

    spinlock_release(&g_cap_lock);
}

// --- Internal: Layer validation ---
// Returns 1 if a capability of type `my_type` is allowed to depend on a cap of type `dep_type`.
static int layer_allows_dep(uint32_t my_type, uint32_t dep_type) {
    switch (my_type) {
        case CAP_HARDWARE:
            return 0;  // HARDWARE has no deps at all
        case CAP_DRIVER:
            // DRIVER can depend on HARDWARE or DRIVER (same-layer OK)
            return (dep_type == CAP_HARDWARE || dep_type == CAP_DRIVER);
        case CAP_SERVICE:
            // SERVICE can depend on HARDWARE, DRIVER, or SERVICE (same-layer OK)
            return (dep_type <= CAP_SERVICE);
        case CAP_APPLICATION:
            // APPLICATION can depend on layers 0-2 only (no same-layer)
            return (dep_type < CAP_APPLICATION);
        case CAP_FEATURE:
            // FEATURE can depend on layers 0-3 (no same-layer)
            return (dep_type < CAP_FEATURE);
        case CAP_COMPOSITE:
            // COMPOSITE can depend on layers 0-4 (no same-layer)
            return (dep_type < CAP_COMPOSITE);
        default:
            return 0;
    }
}

// --- Internal: DFS helper for cycle detection and reachability ---
// Walks transitive deps from `start_id`, sets visited[i]=1 for each reachable cap.
static void dfs_walk(uint32_t start_id, uint8_t *visited) {
    if (start_id >= g_cap_count || (*g_can_entry_ptrs[start_id]).deleted || visited[start_id])
        return;
    visited[start_id] = 1;
    for (uint32_t i = 0; i < (*g_can_entry_ptrs[start_id]).dep_count; i++) {
        dfs_walk((*g_can_entry_ptrs[start_id]).dep_indices[i], visited);
    }
}

// --- Registration (6-pass compiler) ---

int cap_register(const char *name, uint32_t type, uint32_t subtype, int32_t owner,
                 const char **dep_names, int dep_count,
                 int (*activate_fn)(void), int (*deactivate_fn)(void),
                 cap_op_t *ops, int op_count,
                 int (*get_stats)(state_driver_stat_t*, int)) {

    // Phase 14: ensure the slab-backed cap store is ready. Safe to call
    // from both early-boot (pmm/slab already up) and runtime registrations.
    can_entry_cache_init_once();

    spinlock_acquire(&g_cap_lock);

    // --- PASS 1: Lexical Validation ---

    // Check name is non-NULL and non-empty
    if (!name || name[0] == '\0') {
        klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: empty name");
        spinlock_release(&g_cap_lock);
        return CAP_ERR_NAME_EMPTY;
    }

    // Check for duplicate name (skip deleted slots)
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (!g_can_entry_ptrs[i]) continue;  // Phase 14: freed slot
        if ((*g_can_entry_ptrs[i]).deleted) continue;
        if (cap_strcmp((*g_can_entry_ptrs[i]).name, name) == 0) {
            klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: duplicate name: %s", name);
            spinlock_release(&g_cap_lock);
            return CAP_ERR_NAME_DUPLICATE;
        }
    }

    // Check registry capacity
    if (g_cap_count >= MAX_CAPABILITIES) {
        klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: registry full");
        spinlock_release(&g_cap_lock);
        return CAP_ERR_REGISTRY_FULL;
    }

    // Clamp dep_count
    if (dep_count < 0) dep_count = 0;
    if (dep_count > MAX_CAP_DEPS) dep_count = MAX_CAP_DEPS;

    // HARDWARE must have no deps
    if (type == CAP_HARDWARE && dep_count > 0) {
        klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: HARDWARE cap has deps: %s", name);
        spinlock_release(&g_cap_lock);
        return CAP_ERR_HW_HAS_DEPS;
    }

    // --- PASS 2: Dependency Resolution ---

    uint32_t resolved_deps[MAX_CAP_DEPS];
    cap_memset(resolved_deps, 0, sizeof(resolved_deps));

    for (int i = 0; i < dep_count; i++) {
        if (!dep_names || !dep_names[i] || dep_names[i][0] == '\0') {
            klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: empty dep name for: %s", name);
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_UNRESOLVED;
        }

        // Self-dependency check
        if (cap_strcmp(dep_names[i], name) == 0) {
            klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: self-dep: %s", name);
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_SELF;
        }

        // Resolve name to index (skip deleted slots)
        int found = -1;
        for (uint32_t j = 0; j < g_cap_count; j++) {
            if ((*g_can_entry_ptrs[j]).deleted) continue;
            if (cap_strcmp((*g_can_entry_ptrs[j]).name, dep_names[i]) == 0) {
                found = (int)j;
                break;
            }
        }

        if (found < 0) {
            klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: unresolved dep '%s' for: ", dep_names[i]);
            klog(KLOG_INFO, SUBSYS_CAP, "%s", name);
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_UNRESOLVED;
        }

        resolved_deps[i] = (uint32_t)found;
    }

    // --- PASS 3: Layer Checking ---

    for (int i = 0; i < dep_count; i++) {
        uint32_t dep_type = (*g_can_entry_ptrs[resolved_deps[i]]).type;
        if (!layer_allows_dep(type, dep_type)) {
            klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: layer violation: %s", name);
            klog(KLOG_INFO, SUBSYS_CAP, " (type=%lu) cannot depend on ", (unsigned long)(type));
            klog(KLOG_INFO, SUBSYS_CAP, "%s", (*g_can_entry_ptrs[resolved_deps[i]]).name);
            klog(KLOG_INFO, SUBSYS_CAP, " (type=%lu)", (unsigned long)(dep_type));
            spinlock_release(&g_cap_lock);
            return CAP_ERR_LAYER_VIOLATION;
        }
    }

    // --- PASS 4: Cycle Safety Check ---
    // Defense-in-depth: with "deps must already exist" rule, cycles cannot form.
    // This verifies that invariant by DFS from each dep to ensure the new cap's
    // name does not appear in the transitive closure.
    if (dep_count > 0) {
        uint8_t visited[MAX_CAPABILITIES];
        cap_memset(visited, 0, sizeof(visited));
        for (int i = 0; i < dep_count; i++) {
            dfs_walk(resolved_deps[i], visited);
        }
        // Since the new cap isn't committed yet, check if any visited cap
        // has the same name (shouldn't happen since pass 1 rejects duplicates,
        // but validates the graph structure)
        for (uint32_t i = 0; i < g_cap_count; i++) {
            if (!g_can_entry_ptrs[i]) continue;
            if (visited[i] && !(*g_can_entry_ptrs[i]).deleted && cap_strcmp((*g_can_entry_ptrs[i]).name, name) == 0) {
                klog(KLOG_INFO, SUBSYS_CAP, "[CAN] ERR: cycle detected for: %s", name);
                spinlock_release(&g_cap_lock);
                return CAP_ERR_CYCLE;
            }
        }
    }

    // --- PASS 5: Reachability Warning (advisory) ---
    // Check if any transitive dependency reaches a HARDWARE node.
    // Non-HARDWARE caps with no path to HARDWARE can never be truly grounded.
    if (type != CAP_HARDWARE && dep_count > 0) {
        uint8_t visited[MAX_CAPABILITIES];
        cap_memset(visited, 0, sizeof(visited));
        for (int i = 0; i < dep_count; i++) {
            dfs_walk(resolved_deps[i], visited);
        }
        int has_hw = 0;
        for (uint32_t i = 0; i < g_cap_count; i++) {
            if (!g_can_entry_ptrs[i]) continue;
            if (visited[i] && (*g_can_entry_ptrs[i]).type == CAP_HARDWARE && !(*g_can_entry_ptrs[i]).deleted) {
                has_hw = 1;
                break;
            }
        }
        if (!has_hw) {
            klog(KLOG_WARN, SUBSYS_CAP, "[CAN] PASS5 WARN: '%s", name);
            klog(KLOG_INFO, SUBSYS_CAP, "' has no path to HARDWARE");
        }
    }

    // --- PASS 6: Transitive Redundancy Detection (advisory) ---
    // If dep A is transitively reachable via dep B, the direct dep on A is redundant.
    if (dep_count > 1) {
        for (int i = 0; i < dep_count; i++) {
            for (int j = 0; j < dep_count; j++) {
                if (i == j) continue;
                // Check if resolved_deps[i] is reachable from resolved_deps[j]
                uint8_t visited[MAX_CAPABILITIES];
                cap_memset(visited, 0, sizeof(visited));
                // Walk from dep j's children (not dep j itself)
                for (uint32_t k = 0; k < (*g_can_entry_ptrs[resolved_deps[j]]).dep_count; k++) {
                    dfs_walk((*g_can_entry_ptrs[resolved_deps[j]]).dep_indices[k], visited);
                }
                if (visited[resolved_deps[i]]) {
                    klog(KLOG_WARN, SUBSYS_CAP, "[CAN] PASS6 WARN: dep '%s", dep_names[i]);
                    klog(KLOG_INFO, SUBSYS_CAP, "' in '%s' transitively redundant via '", name);
                    klog(KLOG_INFO, SUBSYS_CAP, "%s'", dep_names[j]);
                    break;  // Only warn once per redundant dep
                }
            }
        }
    }

    // --- All passes OK: commit to registry ---

    uint32_t id = g_cap_count;

    // Phase 14: allocate the can_entry_t from the slab. Zero-init is
    // automatic (NULL ctor path in the slab). Explicit cap_memset is
    // redundant but kept as a defensive clear.
    g_can_entry_ptrs[id] = kmem_cache_alloc(can_entry_cache);
    if (!g_can_entry_ptrs[id]) {
        spinlock_release(&g_cap_lock);
        klog(KLOG_ERROR, SUBSYS_CAP,
             "cap_register: can_entry_cache alloc failed for '%s'", name);
        return CAP_ERR_REGISTRY_FULL;
    }
    can_entry_t *cap = g_can_entry_ptrs[id];
    cap_memset(cap, 0, sizeof(*cap));

    cap_strcpy(cap->name, name, CAP_NAME_LEN);
    cap->type = type;
    cap->subtype = subtype;
    cap->owner_pid = owner;
    cap->dep_count = (uint32_t)dep_count;

    for (int i = 0; i < dep_count; i++) {
        cap->dep_indices[i] = resolved_deps[i];
        if (dep_names[i]) {
            cap_strcpy(cap->dep_names[i], dep_names[i], CAP_NAME_LEN);
        }
    }

    cap->activate_fn = activate_fn;
    cap->deactivate_fn = deactivate_fn;
    cap->get_stats_fn = get_stats;

    // Copy operation descriptors
    cap->op_count = (op_count > MAX_CAP_OPS) ? MAX_CAP_OPS : (uint32_t)op_count;
    if (ops) {
        for (uint32_t i = 0; i < cap->op_count; i++) {
            cap_strcpy(cap->ops[i].name, ops[i].name, CAP_OP_NAME_LEN);
            cap->ops[i].param_count = ops[i].param_count;
            cap->ops[i].flags = ops[i].flags;
        }
    }

    // HARDWARE is always ON (represents physical presence)
    if (type == CAP_HARDWARE) {
        cap->state = CAP_STATE_ON;
        cap->activation_count = 1;
    } else {
        cap->state = CAP_STATE_OFF;
    }

    cap->compiled = 1;
    cap->deleted = 0;
    cap->cap_object_idx = CAP_OBJECT_IDX_NONE;
    g_cap_count++;

    // Log
    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Registered: %s", name);
    klog(KLOG_INFO, SUBSYS_CAP, " (type=%lu, deps=%lu, ops=%lu, state=", (unsigned long)(type), (unsigned long)(dep_count), (unsigned long)(cap->op_count));
    klog(KLOG_INFO, SUBSYS_CAP, "%s)", cap->state == CAP_STATE_ON ? "ON" : "OFF");

    spinlock_release(&g_cap_lock);

    // Phase 15a: pair with a cap_object_t (kind=CAN, IMMORTAL|PUBLIC,
    // RIGHTS_ALL). Audience = [PID_PUBLIC] advertises the broadcast
    // intent to anyone inspecting later. Skip pairing until
    // cap_object_init has been called — on the bring-up path that
    // runs after kheap_init, before the first cap_register.
    int32_t public_audience[CAP_AUDIENCE_MAX] = {
        PID_PUBLIC, PID_NONE, PID_NONE, PID_NONE,
        PID_NONE,   PID_NONE, PID_NONE, PID_NONE
    };
    int obj_idx = cap_object_create(CAP_KIND_CAN,
                                    RIGHTS_ALL,
                                    public_audience,
                                    CAP_FLAG_IMMORTAL | CAP_FLAG_PUBLIC,
                                    /*kind_data=*/(uintptr_t)cap,
                                    /*owner_pid=*/PID_KERNEL,
                                    /*parent_idx=*/CAP_OBJECT_IDX_NONE);
    if (obj_idx < 0) {
        klog(KLOG_WARN, SUBSYS_CAP,
             "cap_register: cap_object_create failed for '%s' (err=%d); CAN entry live but unpaired",
             name, obj_idx);
    } else {
        cap->cap_object_idx = (uint32_t)obj_idx;
    }

    // Phase 15b: persistent audit record of this registration. obj_idx may
    // be CAP_OBJECT_IDX_NONE here (pairing failed) but we still want the
    // event logged with whatever we managed to allocate.
    audit_write_cap_register(cap->cap_object_idx, name, AUDIT_SRC_NATIVE);

    return (int)id;
}

// --- Activation ---

// Phase 16 helper: grab the currently-running task's pid, or -1 if none
// (pre-sched_init boot, or detached kernel context).
static int32_t cap_caller_pid(void) {
    task_t *t = sched_get_current_task();
    return t ? (int32_t)t->id : -1;
}

int cap_activate(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    // Deleted caps cannot be activated
    if (cap->deleted) return CAP_ERR_DELETED;

    // Already ON - idempotent (no audit, no state flip)
    if (cap->state == CAP_STATE_ON) return CAP_OK;

    // STARTING means we're in a recursive activation chain - runtime cycle
    if (cap->state == CAP_STATE_STARTING) return CAP_ERR_CYCLE;

    // Mark as starting (cycle guard)
    cap->state = CAP_STATE_STARTING;

    // Activate all dependencies first (recursive DFS)
    for (uint32_t i = 0; i < cap->dep_count; i++) {
        int dep_id = (int)cap->dep_indices[i];
        int result = cap_activate(dep_id);
        if (result != CAP_OK) {
            cap->state = CAP_STATE_ERROR;
            cap->error_dep = cap->dep_indices[i];
            if (cap->watcher_count > 0)
                cap_notify_watchers(cap_id, CAP_STATE_STARTING, CAP_STATE_ERROR);
            klog(KLOG_ERROR, SUBSYS_CAP, "[CAN] Activation failed for '%s", cap->name);
            klog(KLOG_INFO, SUBSYS_CAP, "': dep '%s", (*g_can_entry_ptrs[dep_id]).name);
            klog(KLOG_ERROR, SUBSYS_CAP, "' failed");
            audit_write_cap_activate(cap_caller_pid(), cap->cap_object_idx,
                                     CAP_ERR_DEP_FAILED, cap->name, AUDIT_SRC_NATIVE);
            return CAP_ERR_DEP_FAILED;
        }
    }

    // Call activate function if provided
    if (cap->activate_fn) {
        int result = cap->activate_fn();
        if (result != 0) {
            cap->state = CAP_STATE_ERROR;
            if (cap->watcher_count > 0)
                cap_notify_watchers(cap_id, CAP_STATE_STARTING, CAP_STATE_ERROR);
            klog(KLOG_ERROR, SUBSYS_CAP, "[CAN] Activation callback failed for '%s", cap->name);
            klog(KLOG_INFO, SUBSYS_CAP, "'");
            audit_write_cap_activate(cap_caller_pid(), cap->cap_object_idx,
                                     CAP_ERR_ACTIVATE_FAIL, cap->name, AUDIT_SRC_NATIVE);
            return CAP_ERR_ACTIVATE_FAIL;
        }
    }

    // Success
    cap->state = CAP_STATE_ON;
    cap->activation_count++;

    if (cap->watcher_count > 0)
        cap_notify_watchers(cap_id, CAP_STATE_STARTING, CAP_STATE_ON);

    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Activated: %s", cap->name);
    // Phase 16: audit success-path only for user-initiated activations. The
    // kernel boot path activates every cap with pid=-1 and would otherwise
    // emit ~14 pre-FS-attach audit entries that each stall on serial mirror,
    // stretching the activation loop past the first timer tick. Failure and
    // cascade paths already audit unconditionally since they carry useful
    // forensic detail regardless of caller.
    int32_t caller = cap_caller_pid();
    if (caller != -1) {
        audit_write_cap_activate(caller, cap->cap_object_idx, 0,
                                 cap->name, AUDIT_SRC_NATIVE);
    }

    return CAP_OK;
}

// --- Deactivation ---

// Internal recursive worker. Assumes caller has already validated cap_id.
// Increments *out_count by the number of caps whose state was genuinely flipped
// from !OFF to OFF during this call (self + any dependents that were flipped
// by the cascade). Idempotent re-entries (already-OFF caps) do NOT bump the
// counter and do NOT emit audit.
static int cap_deactivate_inner(int cap_id, uint32_t *out_count) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;
    if (!g_can_entry_ptrs[cap_id]) return CAP_ERR_NOT_FOUND;

    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    if (cap->deleted) return CAP_ERR_DELETED;

    // Already OFF - idempotent, no count, no audit
    if (cap->state == CAP_STATE_OFF) return CAP_OK;

    // Phase 16: give the driver a chance to refuse. The refuse predicate is
    // called BEFORE we touch any dependents — so if it refuses, nothing is
    // altered anywhere in the graph.
    if (cap->refuse_deactivate_fn) {
        int refuse = cap->refuse_deactivate_fn();
        if (refuse != 0) {
            klog(KLOG_WARN, SUBSYS_CAP,
                 "[CAN] Deactivate refused for '%s' (rc=%d)", cap->name, refuse);
            audit_write_cap_deactivate(cap_caller_pid(), cap->cap_object_idx,
                                       CAP_ERR_BUSY, cap->name, AUDIT_SRC_NATIVE);
            return CAP_ERR_BUSY;
        }
    }

    // Cascade: recursively deactivate everything depending on this cap.
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (!g_can_entry_ptrs[i]) continue;  // Phase 14: freed slot
        if ((*g_can_entry_ptrs[i]).deleted) continue;
        if ((*g_can_entry_ptrs[i]).state != CAP_STATE_ON && (*g_can_entry_ptrs[i]).state != CAP_STATE_STARTING)
            continue;
        for (uint32_t j = 0; j < (*g_can_entry_ptrs[i]).dep_count; j++) {
            if ((*g_can_entry_ptrs[i]).dep_indices[j] == (uint32_t)cap_id) {
                // A failed dependent cascade propagates as -EBUSY/-EIO up; the
                // current cap stays ON so the system remains coherent.
                int dep_rc = cap_deactivate_inner((int)i, out_count);
                if (dep_rc != CAP_OK) {
                    klog(KLOG_WARN, SUBSYS_CAP,
                         "[CAN] Cascade failed for dependent of '%s' (rc=%d)",
                         cap->name, dep_rc);
                    audit_write_cap_deactivate(cap_caller_pid(), cap->cap_object_idx,
                                               dep_rc, cap->name, AUDIT_SRC_NATIVE);
                    return dep_rc;
                }
                break;
            }
        }
    }

    // Call deactivate function if provided. If it signals failure, leave state
    // ON (we've already returned dependents to OFF — those stay OFF, which is
    // safe; driver is still usable).
    if (cap->deactivate_fn) {
        int rc = cap->deactivate_fn();
        if (rc != 0) {
            klog(KLOG_ERROR, SUBSYS_CAP,
                 "[CAN] Deactivate fn failed for '%s' (rc=%d); state stays ON",
                 cap->name, rc);
            audit_write_cap_deactivate(cap_caller_pid(), cap->cap_object_idx,
                                       rc, cap->name, AUDIT_SRC_NATIVE);
            return rc;
        }
    }

    uint32_t prev_state = cap->state;
    cap->state = CAP_STATE_OFF;
    if (out_count) (*out_count)++;

    if (cap->watcher_count > 0)
        cap_notify_watchers(cap_id, prev_state, CAP_STATE_OFF);

    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Deactivated: %s", cap->name);
    int32_t caller_d = cap_caller_pid();
    if (caller_d != -1) {
        audit_write_cap_deactivate(caller_d, cap->cap_object_idx, 0,
                                   cap->name, AUDIT_SRC_NATIVE);
    }

    return CAP_OK;
}

int cap_deactivate_count(int cap_id, uint32_t *out_count) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;
    uint32_t local_count = 0;
    int rc = cap_deactivate_inner(cap_id, &local_count);
    if (out_count) *out_count = local_count;
    return rc;
}

int cap_deactivate(int cap_id) {
    uint32_t discard = 0;
    return cap_deactivate_count(cap_id, &discard);
}

int cap_set_refuse_hook(int cap_id, int (*fn)(void)) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;
    if (!g_can_entry_ptrs[cap_id]) return CAP_ERR_NOT_FOUND;
    if (g_can_entry_ptrs[cap_id]->deleted) return CAP_ERR_DELETED;
    g_can_entry_ptrs[cap_id]->refuse_deactivate_fn = fn;
    return CAP_OK;
}

int cap_can_by_object_idx(uint32_t obj_idx) {
    if (obj_idx == CAP_OBJECT_IDX_NONE) return CAP_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (!g_can_entry_ptrs[i]) continue;
        if (g_can_entry_ptrs[i]->deleted) continue;
        if (g_can_entry_ptrs[i]->cap_object_idx == obj_idx) return (int)i;
    }
    return CAP_ERR_NOT_FOUND;
}

// --- Unregistration ---

int cap_unregister(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    spinlock_acquire(&g_cap_lock);
    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    if (cap->deleted) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_DELETED;
    }

    // Only APPLICATION, FEATURE, COMPOSITE can be unregistered
    if (cap->type < CAP_APPLICATION) {
        spinlock_release(&g_cap_lock);
        return CAP_ERR_KERNEL_OWNED;
    }

    // Deactivate first if active (cascade to dependents)
    if (cap->state == CAP_STATE_ON || cap->state == CAP_STATE_STARTING) {
        spinlock_release(&g_cap_lock);
        cap_deactivate(cap_id);
        spinlock_acquire(&g_cap_lock);
    }

    // Clear watcher list before marking deleted
    cap->watcher_count = 0;

    cap->deleted = 1;
    cap->state = CAP_STATE_OFF;

    uint32_t paired_obj_idx = cap->cap_object_idx;
    cap->cap_object_idx = CAP_OBJECT_IDX_NONE;

    // Phase 15b: snapshot the name for the audit entry before the slab frees
    // the underlying can_entry_t below.
    char name_snapshot[CAP_NAME_LEN];
    for (size_t i = 0; i < CAP_NAME_LEN; i++) {
        name_snapshot[i] = cap->name[i];
        if (cap->name[i] == '\0') break;
    }
    name_snapshot[CAP_NAME_LEN - 1] = '\0';

    klog(KLOG_INFO, SUBSYS_CAP, "[CAN] Unregistered: %s", cap->name);

    // Phase 14: return the can_entry_t to the slab so memstat shows the
    // count drop. The slot stays reserved (id remains stable) but the
    // backing storage is reclaimed; walkers must treat g_can_entry_ptrs[id]==NULL
    // as "deleted".
    kmem_cache_free(can_entry_cache, cap);
    g_can_entry_ptrs[cap_id] = NULL;

    spinlock_release(&g_cap_lock);

    // Phase 15a: drop the paired cap_object. User-owned caps (APPLICATION/
    // FEATURE/COMPOSITE) are the only ones reaching this path, and those
    // get non-IMMORTAL object pairings in a later phase. Today all CAN
    // cap_objects are IMMORTAL so cap_object_revoke returns -EPERM —
    // bypass the revoke and directly destroy so the slot is reclaimed.
    if (paired_obj_idx != CAP_OBJECT_IDX_NONE) {
        cap_object_destroy(paired_obj_idx);
    }

    // Phase 15b: audit the unregistration. Fired after the lock-released
    // destroy so audit_enqueue's lock ordering doesn't cross g_cap_lock.
    audit_write_cap_unregister(paired_obj_idx, name_snapshot, AUDIT_SRC_NATIVE);

    return CAP_OK;
}

void cap_unregister_by_owner(int32_t owner_pid) {
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (!g_can_entry_ptrs[i]) continue;
        if (!(*g_can_entry_ptrs[i]).deleted && (*g_can_entry_ptrs[i]).owner_pid == owner_pid) {
            cap_unregister((int)i);
        }
    }
}

uint32_t cap_get_object_idx(int can_id) {
    if (can_id < 0 || (uint32_t)can_id >= g_cap_count) return CAP_OBJECT_IDX_NONE;
    if (!g_can_entry_ptrs[can_id]) return CAP_OBJECT_IDX_NONE;
    if (g_can_entry_ptrs[can_id]->deleted) return CAP_OBJECT_IDX_NONE;
    return g_can_entry_ptrs[can_id]->cap_object_idx;
}

int32_t cap_get_owner(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return -1;
    if ((*g_can_entry_ptrs[cap_id]).deleted) return -1;
    return (*g_can_entry_ptrs[cap_id]).owner_pid;
}

// --- Query Functions ---

int cap_find(const char *name) {
    if (!name || name[0] == '\0') return CAP_ERR_NOT_FOUND;

    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (!g_can_entry_ptrs[i]) continue;  // Phase 14: freed slot
        if ((*g_can_entry_ptrs[i]).deleted) continue;
        if (cap_strcmp((*g_can_entry_ptrs[i]).name, name) == 0) {
            return (int)i;
        }
    }
    return CAP_ERR_NOT_FOUND;
}

int cap_get_count(void) {
    return (int)g_cap_count;
}

int cap_get_state(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return -1;
    return (int)(*g_can_entry_ptrs[cap_id]).state;
}

int cap_why_not(int cap_id, char *buf, int buflen) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) {
        if (buf && buflen > 0) cap_strcpy(buf, "not found", buflen);
        return CAP_ERR_NOT_FOUND;
    }

    can_entry_t *cap = &(*g_can_entry_ptrs[cap_id]);

    if (cap->deleted) {
        if (buf && buflen > 0) cap_strcpy(buf, "deleted", buflen);
        return CAP_ERR_DELETED;
    }

    // Already ON
    if (cap->state == CAP_STATE_ON) {
        if (buf && buflen > 0) cap_strcpy(buf, "already active", buflen);
        return CAP_OK;
    }

    // Check each dep
    int pos = 0;
    int found_issue = 0;
    for (uint32_t i = 0; i < cap->dep_count; i++) {
        uint32_t dep_id = cap->dep_indices[i];
        if (dep_id >= g_cap_count) continue;
        if ((*g_can_entry_ptrs[dep_id]).deleted || (*g_can_entry_ptrs[dep_id]).state != CAP_STATE_ON) {
            if (buf && pos < buflen - 1) {
                if (found_issue && pos < buflen - 3) {
                    buf[pos++] = ',';
                    buf[pos++] = ' ';
                }
                const char *prefix = "dep OFF: ";
                if (!found_issue) {
                    for (int k = 0; prefix[k] && pos < buflen - 1; k++)
                        buf[pos++] = prefix[k];
                }
                for (int k = 0; (*g_can_entry_ptrs[dep_id]).name[k] && pos < buflen - 1; k++)
                    buf[pos++] = (*g_can_entry_ptrs[dep_id]).name[k];
                found_issue = 1;
            }
        }
    }

    if (!found_issue) {
        // All deps are ON but we're still OFF - might be error state
        if (cap->state == CAP_STATE_ERROR) {
            if (buf && buflen > 0) {
                if (cap->error_dep < g_cap_count) {
                    cap_strcpy(buf, "dep failed: ", buflen);
                    int l = cap_strlen(buf);
                    cap_strcpy(buf + l, (*g_can_entry_ptrs[cap->error_dep]).name, buflen - l);
                } else {
                    cap_strcpy(buf, "activation callback failed", buflen);
                }
            }
            return CAP_ERR_ACTIVATE_FAIL;
        }
        // All deps ON and state is OFF - can be activated
        if (buf && buflen > 0) cap_strcpy(buf, "ready to activate", buflen);
        return CAP_OK;
    }

    if (buf && pos < buflen) buf[pos] = '\0';
    return CAP_ERR_DEP_FAILED;
}

// Snapshot all capabilities (including deleted slots to preserve index stability)
int cap_query_all(state_cap_entry_t *out, int max) {
    if (!out || max <= 0) return 0;

    spinlock_acquire(&g_cap_lock);

    int count = (int)g_cap_count;
    if (count > max) count = max;

    for (int i = 0; i < count; i++) {
        if (!g_can_entry_ptrs[i]) {
            // Freed (Phase 14) slot — report as a deleted entry so the
            // caller can still see the stable cap_id → deleted mapping.
            cap_memset(&out[i], 0, sizeof(out[i]));
            out[i].deleted = 1;
            continue;
        }
        cap_strcpy(out[i].name, (*g_can_entry_ptrs[i]).name, STATE_CAP_NAME_LEN);
        out[i].type = (*g_can_entry_ptrs[i]).type;
        out[i].state = (*g_can_entry_ptrs[i]).state;
        out[i].owner_pid = (*g_can_entry_ptrs[i]).owner_pid;
        out[i].dep_count = (*g_can_entry_ptrs[i]).dep_count;
        for (uint32_t j = 0; j < (*g_can_entry_ptrs[i]).dep_count && j < STATE_MAX_CAP_DEPS; j++) {
            out[i].dep_indices[j] = (*g_can_entry_ptrs[i]).dep_indices[j];
        }
        out[i].op_count = (*g_can_entry_ptrs[i]).op_count;
        out[i].activation_count = (*g_can_entry_ptrs[i]).activation_count;
        out[i].deleted = (*g_can_entry_ptrs[i]).deleted;
        out[i].subtype = (*g_can_entry_ptrs[i]).subtype;
    }

    spinlock_release(&g_cap_lock);
    return count;
}

// --- Driver Compatibility ---
// Snapshot driver-type capabilities into the old state_driver_info_t format.
// This replaces driver_snapshot_all() from the deleted driver.c.
int cap_snapshot_drivers(state_driver_info_t *out, int max) {
    if (!out || max <= 0) return 0;

    spinlock_acquire(&g_cap_lock);

    int written = 0;
    for (uint32_t i = 0; i < g_cap_count && written < max; i++) {
        if (!g_can_entry_ptrs[i]) continue;
        if ((*g_can_entry_ptrs[i]).deleted) continue;
        // Include DRIVER and SERVICE caps (these are the "drivers" in the old sense)
        if ((*g_can_entry_ptrs[i]).type != CAP_DRIVER && (*g_can_entry_ptrs[i]).type != CAP_SERVICE) continue;

        state_driver_info_t *d = &out[written];
        cap_memset(d, 0, sizeof(*d));

        cap_strcpy(d->name, (*g_can_entry_ptrs[i]).name, STATE_DRIVER_NAME_LEN);
        d->type = (*g_can_entry_ptrs[i]).subtype;
        d->initialized = ((*g_can_entry_ptrs[i]).state == CAP_STATE_ON) ? 1 : 0;

        // Copy operation descriptors
        d->op_count = (*g_can_entry_ptrs[i]).op_count;
        for (uint32_t j = 0; j < (*g_can_entry_ptrs[i]).op_count && j < STATE_MAX_DRIVER_OPS; j++) {
            cap_strcpy(d->ops[j].name, (*g_can_entry_ptrs[i]).ops[j].name, STATE_OP_NAME_LEN);
            d->ops[j].param_count = (*g_can_entry_ptrs[i]).ops[j].param_count;
            d->ops[j].flags = (*g_can_entry_ptrs[i]).ops[j].flags;
        }

        // Call stats callback for live data
        if ((*g_can_entry_ptrs[i]).get_stats_fn) {
            d->stat_count = (*g_can_entry_ptrs[i]).get_stats_fn(d->stats, STATE_MAX_DRIVER_STATS);
        } else {
            d->stat_count = 0;
        }

        written++;
    }

    spinlock_release(&g_cap_lock);
    return written;
}
