// kernel/cap/revoke.c
// Phase 15a: Revocation cascade + owner-exit orphan collection.
//
// cap_object_revoke() handles the single-object case: bump generation +
// mark deleted. If the target has CAP_FLAG_EAGER_REVOKE, it also calls
// revoke_cascade() which BFS-walks the descendant tree via
// first_child_idx / next_sibling_idx linkage and bumps every descendant's
// generation.
//
// The cascade uses a bounded work queue (1024 uint32_t) on the kernel
// stack. If the subtree exceeds that, we emit CAP_FLAG_CASCADE_TRUNCATED
// on the root + klog WARN. Operators must re-revoke the remaining
// descendants themselves.
//
// revoke_collect_orphans(pid) is called from sched_reap_zombie; it scans
// g_cap_object_ptrs[] for owner_pid match and revokes each (non-eager;
// the cascade on the root already took care of its children if the
// dying process owned them).

#include "object.h"
#include "token.h"

#include <stdint.h>
#include <stdbool.h>

#include "../sync/spinlock.h"
#include "../log.h"

#define REVOKE_CASCADE_MAX 1024

// Breadth-first walk of the descendant tree. Bumps generation on every
// non-immortal descendant. Returns the number of descendants invalidated
// (not including the root, which cap_object_revoke bumped). Negative on
// error (but in practice always returns >= 0).
int revoke_cascade(uint32_t root_idx) {
    if (root_idx == 0 || root_idx >= CAP_OBJECT_CAPACITY) return 0;

    uint32_t queue[REVOKE_CASCADE_MAX];
    int head = 0, tail = 0;
    bool truncated = false;

    // Seed queue with root's children (root itself already bumped by caller).
    spinlock_acquire(&g_cap_object_lock);
    cap_object_t *root = g_cap_object_ptrs[root_idx];
    if (!root) {
        spinlock_release(&g_cap_object_lock);
        return 0;
    }
    uint32_t cur = root->first_child_idx;
    while (cur != CAP_OBJECT_IDX_NONE && tail < REVOKE_CASCADE_MAX) {
        queue[tail++] = cur;
        cap_object_t *c = g_cap_object_ptrs[cur];
        if (!c) break;
        cur = c->next_sibling_idx;
    }
    if (cur != CAP_OBJECT_IDX_NONE) truncated = true;
    spinlock_release(&g_cap_object_lock);

    int count = 0;

    while (head < tail) {
        uint32_t idx = queue[head++];

        spinlock_acquire(&g_cap_object_lock);
        cap_object_t *obj = g_cap_object_ptrs[idx];
        if (!obj) {
            spinlock_release(&g_cap_object_lock);
            continue;
        }
        // Immortal objects are not revoked even on cascade.
        if (obj->flags & CAP_FLAG_IMMORTAL) {
            spinlock_release(&g_cap_object_lock);
            continue;
        }
        // Already revoked — skip but still enqueue children (bumped once is enough,
        // but children may have unprotected generations).
        if (!obj->deleted) {
            obj->deleted = 1;
            (void)__atomic_add_fetch(&obj->generation, 1, __ATOMIC_SEQ_CST);
            count++;
        }

        // Enqueue children.
        uint32_t c = obj->first_child_idx;
        while (c != CAP_OBJECT_IDX_NONE) {
            if (tail >= REVOKE_CASCADE_MAX) { truncated = true; break; }
            queue[tail++] = c;
            cap_object_t *cobj = g_cap_object_ptrs[c];
            if (!cobj) break;
            c = cobj->next_sibling_idx;
        }
        spinlock_release(&g_cap_object_lock);
    }

    if (truncated) {
        spinlock_acquire(&g_cap_object_lock);
        cap_object_t *root2 = g_cap_object_ptrs[root_idx];
        if (root2) root2->flags |= CAP_FLAG_CASCADE_TRUNCATED;
        spinlock_release(&g_cap_object_lock);
        klog(KLOG_WARN, SUBSYS_CAP,
             "revoke_cascade: truncated at %u descendants for root idx=%u",
             REVOKE_CASCADE_MAX, root_idx);
    }

    return count;
}

// Scan every live slot for owner_pid match; revoke each non-immortal.
// Returns number of objects revoked.
int revoke_collect_orphans(int32_t pid) {
    int count = 0;
    if (pid == PID_KERNEL) return 0;  // never revoke kernel-owned caps

    // Iterate under the lock but release it around the actual revoke call
    // so revoke_cascade (if triggered by EAGER flag) doesn't re-enter.
    uint32_t upper = __atomic_load_n(&g_cap_object_count, __ATOMIC_ACQUIRE);

    for (uint32_t i = 1; i < upper && i < CAP_OBJECT_CAPACITY; i++) {
        bool should_revoke = false;
        bool eager = false;
        spinlock_acquire(&g_cap_object_lock);
        cap_object_t *obj = g_cap_object_ptrs[i];
        if (obj && !obj->deleted && obj->owner_pid == pid &&
            !(obj->flags & CAP_FLAG_IMMORTAL)) {
            should_revoke = true;
            eager = (obj->flags & CAP_FLAG_EAGER_REVOKE) != 0;
            // Bump under the same lock to avoid races.
            obj->deleted = 1;
            (void)__atomic_add_fetch(&obj->generation, 1, __ATOMIC_SEQ_CST);
            count++;
        }
        spinlock_release(&g_cap_object_lock);

        if (should_revoke && eager) {
            int c = revoke_cascade(i);
            if (c > 0) count += c;
        }
    }

    if (count > 0) {
        klog(KLOG_INFO, SUBSYS_CAP,
             "revoke_collect_orphans: pid=%d revoked=%d", pid, count);
    }
    return count;
}
