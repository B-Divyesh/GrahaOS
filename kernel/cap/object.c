// kernel/cap/object.c
// Phase 15a: Capability Objects v2 — registry + lifecycle + hot-path resolver.
//
// All cap_object_t bodies are slab-allocated from cap_object_cache (Phase 14).
// The pointer array g_cap_object_ptrs[] is static BSS (65536 * 8 B = 512 KiB),
// indexed by the token's 24-bit idx field. Slot 0 is the kernel-owner
// sentinel (IMMORTAL | PUBLIC | RIGHTS_ALL), reserved at init so idx==0
// in any token is a definitive "null token" signal.
//
// Locking:
//   - Read path (cap_token_resolve): no locks. Generation is loaded with
//     __ATOMIC_ACQUIRE. An in-progress revoke on another CPU either
//     already-bumped (we see the new gen → fail correctly) or not-yet
//     (we see the old gen → succeed; the caller proceeds with the
//     pre-revoke state, which is the correct semantic for a racing
//     revoke).
//   - Write paths (create / derive / revoke / destroy / link_child):
//     hold g_cap_object_lock. Fine-grained per-object locks are a
//     Phase 20 concern if contention shows up.
//
// Lock order (documented here, enforced by convention):
//     g_cap_object_lock  →  g_cap_lock (can.c)  →  task->cap_handles.lock

#include "object.h"
#include "token.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../sync/spinlock.h"
#include "../mm/slab.h"
#include "../log.h"
#include "../panic.h"
#include "../audit.h"
#include "../ipc/channel.h"
#include "../mm/vmo.h"

// Phase 18: stream endpoint deactivator. Forward declared here — stream.h is
// added in U2, then the deactivator lands in U4. Until then a weak no-op stub
// keeps the link clean; no CAP_KIND_STREAM objects exist before U4 anyway, so
// the case below is unreachable pre-U4.
struct cap_object;
void stream_endpoint_deactivate(struct cap_object *obj);
__attribute__((weak)) void stream_endpoint_deactivate(struct cap_object *obj) {
    (void)obj;
}

// ------------------------------------------------------------------------
// Global state.
// ------------------------------------------------------------------------
cap_object_t *g_cap_object_ptrs[CAP_OBJECT_CAPACITY];
uint32_t      g_cap_object_count = 0;   // High-water mark — bumped on create
spinlock_t    g_cap_object_lock = SPINLOCK_INITIALIZER("cap_object_lock");

static kmem_cache_t *cap_object_cache = NULL;

// Forward declarations for revoke.c (unit 7).
extern int  revoke_cascade(uint32_t root_idx);

// ------------------------------------------------------------------------
// Helpers (file-local).
// ------------------------------------------------------------------------

static void cap_object_zero(cap_object_t *o) {
    uint8_t *p = (uint8_t *)o;
    for (size_t i = 0; i < sizeof(*o); i++) p[i] = 0;
}

// Returns the number of audience entries copied (0..CAP_AUDIENCE_MAX).
// `src` is a PID_NONE-terminated array, or NULL to default to
// single-audience=[owner_pid]. Entries with PID_PUBLIC (0xFFFF) imply that
// CAP_FLAG_PUBLIC should be set on the object by the caller.
static uint8_t cap_object_copy_audience(int32_t *dst, const int32_t *src,
                                        int32_t fallback_pid) {
    if (!src) {
        dst[0] = fallback_pid;
        for (int i = 1; i < CAP_AUDIENCE_MAX; i++) dst[i] = PID_NONE;
        return 1;
    }
    uint8_t n = 0;
    for (int i = 0; i < CAP_AUDIENCE_MAX; i++) {
        if (src[i] == PID_NONE) break;
        dst[n++] = src[i];
    }
    for (uint8_t i = n; i < CAP_AUDIENCE_MAX; i++) dst[i] = PID_NONE;
    return n;
}

// Find first free slot in [1..CAP_OBJECT_CAPACITY). O(N) linear scan;
// acceptable for Phase 15a (tests touch dozens). Must be called with
// g_cap_object_lock held.
static uint32_t cap_object_find_free_slot_locked(void) {
    for (uint32_t i = 1; i < CAP_OBJECT_CAPACITY; i++) {
        if (!g_cap_object_ptrs[i]) return i;
    }
    return CAP_OBJECT_IDX_NONE;
}

// Validate that `audience_subset` is a subset of parent's audience. With
// CAP_FLAG_PUBLIC on parent, any subset is allowed. Otherwise every non-
// PID_NONE entry must appear in parent.audience_set.
static bool audience_is_subset(const cap_object_t *parent,
                               const int32_t *subset) {
    if (parent->flags & CAP_FLAG_PUBLIC) return true;
    if (!subset) return true;  // NULL means default=caller only; checked elsewhere
    for (int i = 0; i < CAP_AUDIENCE_MAX; i++) {
        int32_t p = subset[i];
        if (p == PID_NONE) break;
        if (p == PID_PUBLIC) return false;  // can't widen to public
        bool found = false;
        for (uint8_t j = 0; j < parent->audience_count && j < CAP_AUDIENCE_MAX; j++) {
            if (parent->audience_set[j] == p) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// ------------------------------------------------------------------------
// cap_object_init — called from kernel/main.c after kheap_init().
// ------------------------------------------------------------------------
void cap_object_init(void) {
    if (cap_object_cache) return;  // idempotent guard

    cap_object_cache = kmem_cache_create("cap_object_t",
                                         sizeof(cap_object_t),
                                         _Alignof(cap_object_t),
                                         /*ctor=*/NULL,
                                         SUBSYS_CAP);
    if (!cap_object_cache) {
        kpanic("cap_object_init: kmem_cache_create failed");
    }

    // Reserve slot 0 as the kernel-owner sentinel. Any token with idx==0
    // is a null token and fails resolve (audience check below never
    // matches an ordinary process pid because owner is kernel and
    // audience is PID_PUBLIC — but the actual guard is cap_token_is_null
    // at the start of resolve).
    cap_object_t *sentinel = (cap_object_t *)kmem_cache_alloc(cap_object_cache);
    if (!sentinel) {
        kpanic("cap_object_init: slab alloc for slot 0 failed");
    }
    cap_object_zero(sentinel);
    sentinel->kind             = CAP_KIND_NONE;
    sentinel->generation       = 1;           // non-zero so unused gen=0 tokens always fail
    sentinel->rights_bitmap    = RIGHTS_ALL;
    sentinel->audience_set[0]  = PID_PUBLIC;
    for (int i = 1; i < CAP_AUDIENCE_MAX; i++) sentinel->audience_set[i] = PID_NONE;
    sentinel->audience_count   = 1;
    sentinel->flags            = CAP_FLAG_IMMORTAL | CAP_FLAG_PUBLIC;
    sentinel->parent_idx       = CAP_OBJECT_IDX_NONE;
    sentinel->owner_pid        = PID_KERNEL;
    sentinel->kind_data        = 0;
    sentinel->first_child_idx  = CAP_OBJECT_IDX_NONE;
    sentinel->next_sibling_idx = CAP_OBJECT_IDX_NONE;
    sentinel->watch_list_head  = PID_NONE;
    for (int i = 0; i < CAP_WATCH_MAX - 1; i++) sentinel->watch_list_tail[i] = PID_NONE;
    sentinel->watch_count      = 0;

    g_cap_object_ptrs[0] = sentinel;
    g_cap_object_count   = 1;

    klog(KLOG_INFO, SUBSYS_CAP, "cap_object: init complete (slot 0 reserved, capacity=%u)", CAP_OBJECT_CAPACITY);
}

// ------------------------------------------------------------------------
// Hot path: cap_token_resolve. Lock-free; ~20-30 cycles typical.
// ------------------------------------------------------------------------
cap_object_t *cap_token_resolve(int32_t calling_pid, cap_token_t tok,
                                uint64_t required_rights) {
    if (cap_token_is_null(tok)) return NULL;

    uint32_t idx = cap_token_idx(tok);
    if (idx == 0 || idx >= CAP_OBJECT_CAPACITY) return NULL;

    cap_object_t *obj = __atomic_load_n(&g_cap_object_ptrs[idx], __ATOMIC_ACQUIRE);
    if (!obj) return NULL;

    if (obj->deleted) return NULL;

    uint32_t obj_gen = __atomic_load_n(&obj->generation, __ATOMIC_ACQUIRE);
    if (obj_gen != cap_token_gen(tok)) return NULL;

    // Audience check.
    if (!(obj->flags & CAP_FLAG_PUBLIC)) {
        bool in_audience = false;
        for (uint8_t i = 0; i < obj->audience_count && i < CAP_AUDIENCE_MAX; i++) {
            if (obj->audience_set[i] == calling_pid) {
                in_audience = true;
                break;
            }
        }
        if (!in_audience) return NULL;
    }

    // Rights check — every bit in required_rights must be present.
    if ((obj->rights_bitmap & required_rights) != required_rights) return NULL;

    return obj;
}

// ------------------------------------------------------------------------
// Lifecycle.
// ------------------------------------------------------------------------

int cap_object_create(uint16_t kind, uint64_t rights, const int32_t *audience,
                      uint8_t flags, uintptr_t kind_data, int32_t owner_pid,
                      uint32_t parent_idx) {
    if (!cap_object_cache) return CAP_V2_EINVAL;  // init not called

    cap_object_t *obj = (cap_object_t *)kmem_cache_alloc(cap_object_cache);
    if (!obj) return CAP_V2_ENOMEM;
    cap_object_zero(obj);

    obj->kind           = kind;
    obj->generation     = 1;
    obj->rights_bitmap  = rights;
    obj->audience_count = cap_object_copy_audience(obj->audience_set, audience, owner_pid);
    // Detect PID_PUBLIC sentinel → automatically mark PUBLIC.
    for (uint8_t i = 0; i < obj->audience_count; i++) {
        if ((uint32_t)obj->audience_set[i] == (uint32_t)PID_PUBLIC) {
            flags |= CAP_FLAG_PUBLIC;
            break;
        }
    }
    obj->flags            = flags;
    obj->parent_idx       = parent_idx;
    obj->owner_pid        = owner_pid;
    obj->kind_data        = kind_data;
    obj->first_child_idx  = CAP_OBJECT_IDX_NONE;
    obj->next_sibling_idx = CAP_OBJECT_IDX_NONE;
    obj->watch_list_head  = PID_NONE;
    for (int i = 0; i < CAP_WATCH_MAX - 1; i++) obj->watch_list_tail[i] = PID_NONE;
    obj->watch_count      = 0;

    int err;
    spinlock_acquire(&g_cap_object_lock);
    uint32_t slot = cap_object_find_free_slot_locked();
    if (slot == CAP_OBJECT_IDX_NONE) {
        spinlock_release(&g_cap_object_lock);
        kmem_cache_free(cap_object_cache, obj);
        return CAP_V2_ENOMEM;
    }
    g_cap_object_ptrs[slot] = obj;
    if (slot >= g_cap_object_count) g_cap_object_count = slot + 1;
    spinlock_release(&g_cap_object_lock);

    // If linking to a parent, splice into parent's child list. This takes
    // the lock again (briefly) rather than doing it under one hold — we
    // intentionally avoid nesting since link_child is a public API and
    // may be called on its own.
    if (parent_idx != CAP_OBJECT_IDX_NONE && parent_idx < CAP_OBJECT_CAPACITY) {
        cap_object_link_child(parent_idx, slot);
    }

    err = (int)slot;
    return err;
}

int cap_object_derive(uint32_t parent_idx, int32_t caller_pid,
                      uint64_t rights_subset, const int32_t *audience_subset,
                      uint8_t flags_subset) {
    if (parent_idx == 0 || parent_idx >= CAP_OBJECT_CAPACITY) return CAP_V2_EINVAL;

    cap_object_t *parent = __atomic_load_n(&g_cap_object_ptrs[parent_idx], __ATOMIC_ACQUIRE);
    if (!parent) return CAP_V2_EINVAL;
    if (parent->deleted) return CAP_V2_EREVOKED;

    // Caller must be in the parent's audience (or parent is PUBLIC).
    if (!cap_token_validate_audience(parent, caller_pid)) return CAP_V2_EPERM;
    // Caller's view of the parent must include RIGHT_DERIVE.
    if ((parent->rights_bitmap & RIGHT_DERIVE) == 0) return CAP_V2_EPERM;

    // Rights subset check.
    if ((parent->rights_bitmap & rights_subset) != rights_subset) return CAP_V2_EPERM;
    // Flags subset check — strict only for privileged flags (PUBLIC + IMMORTAL).
    // Non-privileged flags (EAGER_REVOKE, INHERITABLE) may be freely set by
    // the derive caller: they only affect the new subtree and don't elevate
    // the child's effective authority over the parent.
    const uint8_t PRIVILEGED_FLAGS = CAP_FLAG_PUBLIC | CAP_FLAG_IMMORTAL;
    const uint8_t KERNEL_MANAGED_FLAGS = CAP_FLAG_SHIM_EPHEMERAL | CAP_FLAG_CASCADE_TRUNCATED;
    uint8_t priv_sub = flags_subset & PRIVILEGED_FLAGS;
    if ((parent->flags & priv_sub) != priv_sub) return CAP_V2_EPERM;
    // Callers may not set kernel-managed flags.
    if (flags_subset & KERNEL_MANAGED_FLAGS) return CAP_V2_EPERM;
    // Audience subset check.
    if (!audience_is_subset(parent, audience_subset)) return CAP_V2_EPERM;

    int32_t default_audience[CAP_AUDIENCE_MAX];
    const int32_t *aud = audience_subset;
    if (!aud) {
        default_audience[0] = caller_pid;
        for (int i = 1; i < CAP_AUDIENCE_MAX; i++) default_audience[i] = PID_NONE;
        aud = default_audience;
    }

    int new_idx = cap_object_create(parent->kind, rights_subset, aud, flags_subset,
                                    parent->kind_data, caller_pid, parent_idx);
    // Phase 15b: audit successful derives. Failed ones already get a
    // CAP_VIOLATION entry from the syscall dispatcher when pledge or
    // ownership blocks them.
    if (new_idx >= 0) {
        audit_write_cap_derive(caller_pid, parent_idx,
                               (uint32_t)new_idx, rights_subset,
                               AUDIT_SRC_NATIVE);
    }
    return new_idx;
}

int cap_object_revoke(uint32_t idx) {
    if (idx == 0 || idx >= CAP_OBJECT_CAPACITY) return CAP_V2_EINVAL;

    spinlock_acquire(&g_cap_object_lock);
    cap_object_t *obj = g_cap_object_ptrs[idx];
    if (!obj) {
        spinlock_release(&g_cap_object_lock);
        return CAP_V2_EINVAL;
    }
    if (obj->flags & CAP_FLAG_IMMORTAL) {
        spinlock_release(&g_cap_object_lock);
        return CAP_V2_EPERM;
    }
    if (obj->deleted) {
        spinlock_release(&g_cap_object_lock);
        return CAP_V2_EREVOKED;
    }
    bool eager = (obj->flags & CAP_FLAG_EAGER_REVOKE) != 0;
    // Mark deleted first, then bump generation atomically. Readers
    // observe deleted==1 or gen mismatch, both of which fail resolve.
    obj->deleted = 1;
    (void)__atomic_add_fetch(&obj->generation, 1, __ATOMIC_SEQ_CST);
    spinlock_release(&g_cap_object_lock);

    int count = 1;
    if (eager) {
        int cascade_count = revoke_cascade(idx);
        if (cascade_count > 0) count += cascade_count;
    }
    // Phase 15b: audit the revoke. result_code = number of objects affected.
    audit_write_cap_revoke(/*caller_pid unknown here*/ -1, idx, count, AUDIT_SRC_NATIVE);
    return count;
}

void cap_object_destroy(uint32_t idx) {
    if (idx == 0 || idx >= CAP_OBJECT_CAPACITY) return;

    cap_object_t *obj = NULL;
    spinlock_acquire(&g_cap_object_lock);
    obj = g_cap_object_ptrs[idx];
    if (obj) g_cap_object_ptrs[idx] = NULL;
    spinlock_release(&g_cap_object_lock);

    if (!obj) return;

    // Phase 17: kind-specific deactivate hooks. A channel endpoint drops
    // the backing channel's refcount (freeing at 0); a VMO cap drops the
    // VMO's refcount. Called OUTSIDE of g_cap_object_lock so the hooks can
    // themselves acquire cap locks without inversion.
    switch (obj->kind) {
        case CAP_KIND_CHANNEL:
            chan_endpoint_deactivate(obj);
            break;
        case CAP_KIND_VMO:
            vmo_cap_deactivate(obj);
            break;
        case CAP_KIND_STREAM:
            stream_endpoint_deactivate(obj);
            break;
        default:
            break;
    }

    kmem_cache_free(cap_object_cache, obj);
}

int cap_object_inspect(uint32_t idx, int32_t caller_pid,
                       cap_inspect_result_t *out) {
    if (!out) return CAP_V2_EFAULT;
    if (idx == 0 || idx >= CAP_OBJECT_CAPACITY) return CAP_V2_EINVAL;

    cap_object_t *obj = __atomic_load_n(&g_cap_object_ptrs[idx], __ATOMIC_ACQUIRE);
    if (!obj) return CAP_V2_EINVAL;
    if (obj->deleted) return CAP_V2_EREVOKED;

    // Caller must have visibility into the object (be in audience or PUBLIC).
    if (!cap_token_validate_audience(obj, caller_pid)) return CAP_V2_EPERM;

    out->kind          = obj->kind;
    out->flags         = obj->flags;
    out->reserved1     = 0;
    out->generation    = __atomic_load_n(&obj->generation, __ATOMIC_ACQUIRE);
    out->rights_bitmap = obj->rights_bitmap;
    out->owner_pid     = obj->owner_pid;

    // Audience privacy filter: caller sees only pids the caller "knows" —
    // heuristic for Phase 15a: caller knows its own pid plus PID_PUBLIC
    // marker. Anything else is hidden. Phase 15b may extend via pledge
    // classes.
    int filled = 0;
    for (uint8_t i = 0; i < obj->audience_count && i < CAP_AUDIENCE_MAX; i++) {
        int32_t p = obj->audience_set[i];
        if (p == caller_pid || (uint32_t)p == (uint32_t)PID_PUBLIC) {
            out->audience_pids[filled++] = p;
        }
    }
    for (int i = filled; i < CAP_AUDIENCE_MAX; i++) out->audience_pids[i] = PID_NONE;

    // Parent-token is non-zero only if the caller is also in the parent's
    // audience. We don't look up the handle table here — Phase 15a returns
    // 0 and the userspace helper (libcap_v2) can re-inspect if needed.
    out->parent_token.raw = 0;
    for (int i = 0; i < 4; i++) out->reserved2[i] = 0;

    return CAP_V2_OK;
}

void cap_object_link_child(uint32_t parent_idx, uint32_t child_idx) {
    if (parent_idx == CAP_OBJECT_IDX_NONE || parent_idx >= CAP_OBJECT_CAPACITY) return;
    if (child_idx == 0 || child_idx >= CAP_OBJECT_CAPACITY) return;

    spinlock_acquire(&g_cap_object_lock);
    cap_object_t *parent = g_cap_object_ptrs[parent_idx];
    cap_object_t *child  = g_cap_object_ptrs[child_idx];
    if (parent && child) {
        // Insert at head of parent's child list.
        child->next_sibling_idx  = parent->first_child_idx;
        parent->first_child_idx  = child_idx;
    }
    spinlock_release(&g_cap_object_lock);
}
