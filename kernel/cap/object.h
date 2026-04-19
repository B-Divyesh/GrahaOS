// kernel/cap/object.h
// Phase 15a: Capability Objects v2 — kernel-side authority record.
//
// Every file, CAN entry, channel (Phase 17), VMO (Phase 17), snapshot
// (Phase 24) is a cap_object_t with a distinct `kind`. Each object is
// indexed by a flat u32 slot in g_cap_object_ptrs[CAP_OBJECT_CAPACITY];
// userspace never sees the pointer — it holds a cap_token_t that packs
// {generation, idx, flags} and is verified on every syscall via
// cap_token_resolve.
//
// The 96-byte layout is preserved by reordering (kind_data moved after
// rights_bitmap so both u64 fields are naturally aligned). This differs
// from the spec's textual field order but matches the spec's total size
// and field-name-and-meaning contract. Documented as an out-of-spec item.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "token.h"
#include "../sync/spinlock.h"

// ------------------------------------------------------------------------
// Object registry limits.
// ------------------------------------------------------------------------
#define CAP_OBJECT_CAPACITY   65536u  // BSS: 65536 * 8 B = 512 KiB pointer array
#define CAP_OBJECT_IDX_NULL   0u      // Slot 0 is the kernel-owner sentinel;
                                      //  idx==0 in a token always fails resolve.
#define CAP_OBJECT_IDX_NONE   0xFFFFFFFFu  // "no parent" / "no child" marker.

// Audience set fixed width — spec.
#define CAP_AUDIENCE_MAX  8
// Watchers per object — Phase 15a reduces from Phase 8d's 8 to save slab space.
#define CAP_WATCH_MAX     4

// ------------------------------------------------------------------------
// cap_object_t — 96 bytes, slab-allocated from cap_object_cache.
// Layout reordered for natural alignment; field names + semantics match
// specs/phase-15a-cap-objects-v2.yml.
// ------------------------------------------------------------------------
typedef struct cap_object {
    // Word 0 (offset 0..7): kind + deleted + reserved + generation
    uint16_t  kind;                // offset 0  (CAP_KIND_*)
    uint8_t   deleted;             // offset 2  (1 = pending free)
    uint8_t   reserved1;           // offset 3  (pad)
    uint32_t  generation;          // offset 4  (atomic; bumped on revoke)

    // Word 1 (offset 8..15): rights
    uint64_t  rights_bitmap;       // offset 8  (RIGHT_* mask)

    // Word 2 (offset 16..23): kind-specific payload pointer
    uintptr_t kind_data;           // offset 16 (can_entry_t* when kind==CAN)

    // Words 3..6 (offset 24..55): audience pids
    int32_t   audience_set[CAP_AUDIENCE_MAX];  // offset 24..55

    // Word 7 (offset 56..63): misc packed + parent_idx
    uint8_t   audience_count;      // offset 56
    uint8_t   flags;               // offset 57  (CAP_FLAG_*)
    uint8_t   reserved2[2];        // offset 58..59
    uint32_t  parent_idx;          // offset 60  (CAP_OBJECT_IDX_NONE if root)

    // Word 8 (offset 64..71): owner + first child linkage
    int32_t   owner_pid;           // offset 64  (PID_KERNEL for boot caps)
    uint32_t  first_child_idx;     // offset 68  (linked-list head)

    // Word 9 (offset 72..79): sibling + first watcher pid
    uint32_t  next_sibling_idx;    // offset 72  (for parent's children list)
    int32_t   watch_list_head;     // offset 76  (part of watch_list[4])

    // Words 10..11 (offset 80..95): remaining watch_list slots + misc
    int32_t   watch_list_tail[CAP_WATCH_MAX - 1];  // offset 80..91
    uint8_t   watch_count;         // offset 92
    uint8_t   reserved3[3];        // offset 93..95
} cap_object_t;

// Enforce sizeof + critical offsets.
_Static_assert(sizeof(cap_object_t) == 96, "cap_object_t must be exactly 96 bytes");
_Static_assert(offsetof(cap_object_t, generation) == 4, "generation offset");
_Static_assert(offsetof(cap_object_t, rights_bitmap) == 8, "rights_bitmap offset");
_Static_assert(offsetof(cap_object_t, kind_data) == 16, "kind_data offset");
_Static_assert(offsetof(cap_object_t, audience_set) == 24, "audience_set offset");
_Static_assert(offsetof(cap_object_t, parent_idx) == 60, "parent_idx offset");
_Static_assert(offsetof(cap_object_t, owner_pid) == 64, "owner_pid offset");
_Static_assert(offsetof(cap_object_t, watch_count) == 92, "watch_count offset");

// Convenience: full watch_list as a contiguous int32_t[4] view. Callers should
// access via (&obj->watch_list_head)[i], i in [0..CAP_WATCH_MAX).
_Static_assert(offsetof(cap_object_t, watch_list_tail) ==
               offsetof(cap_object_t, watch_list_head) + 4,
               "watch_list must be contiguous");

// ------------------------------------------------------------------------
// cap_inspect_result_t — 64 bytes, filled by cap_object_inspect, copied
// to user via SYS_CAP_INSPECT.
// ------------------------------------------------------------------------
typedef struct cap_inspect_result {
    uint16_t  kind;                           // 0
    uint8_t   flags;                          // 2
    uint8_t   reserved1;                      // 3
    uint32_t  generation;                     // 4
    uint64_t  rights_bitmap;                  // 8
    int32_t   audience_pids[CAP_AUDIENCE_MAX];// 16..47 (filtered by caller intersect)
    cap_token_t parent_token;                 // 48 (0 if caller doesn't hold parent)
    int32_t   owner_pid;                      // 56
    uint8_t   reserved2[4];                   // 60..63
} cap_inspect_result_t;

_Static_assert(sizeof(cap_inspect_result_t) == 64, "cap_inspect_result_t must be 64 bytes");

// ------------------------------------------------------------------------
// Registry (defined in object.c).
// ------------------------------------------------------------------------
extern cap_object_t *g_cap_object_ptrs[CAP_OBJECT_CAPACITY];
extern uint32_t      g_cap_object_count;    // High-water mark (for scan loops)
extern spinlock_t    g_cap_object_lock;

// ------------------------------------------------------------------------
// Lifecycle prototypes. Full implementation lands in unit 6.
// ------------------------------------------------------------------------

// One-time initializer: creates cap_object_cache (slab), reserves slot 0 as
// the kernel-owner sentinel with IMMORTAL | PUBLIC | RIGHTS_ALL. Called from
// kernel/main.c after kheap_init and before any cap_register call.
void cap_object_init(void);

// Create a fresh cap_object_t. `audience` is an array terminated by PID_NONE
// (-1), or NULL for a default audience of [owner_pid]. Returns the chosen
// slot idx on success (>= 1), or a negative CAP_V2_* error.
// If kind_data is non-NULL the kernel stores the pointer verbatim.
// If parent_idx != CAP_OBJECT_IDX_NONE the new object is linked as a child.
int cap_object_create(uint16_t kind, uint64_t rights, const int32_t *audience,
                      uint8_t flags, uintptr_t kind_data, int32_t owner_pid,
                      uint32_t parent_idx);

// Derive a child cap_object_t with rights/audience/flags that are strict
// subsets of the parent's. Returns the new object's idx on success.
int cap_object_derive(uint32_t parent_idx, int32_t caller_pid,
                      uint64_t rights_subset, const int32_t *audience_subset,
                      uint8_t flags_subset);

// Revoke by bumping generation atomically. Returns >= 1 (count invalidated)
// on success, or a negative CAP_V2_* error. If CAP_FLAG_EAGER_REVOKE is set
// on the target, cascades through children via revoke_cascade.
int cap_object_revoke(uint32_t idx);

// Destroy (slab-free) an object that is fully revoked and has no live
// references. Called from the handle-table reference-drop path.
void cap_object_destroy(uint32_t idx);

// Snapshot for SYS_CAP_INSPECT. Filters audience_pids so the caller only
// sees pids it already knows about (privacy preservation — see AW-15a.5).
int cap_object_inspect(uint32_t idx, int32_t caller_pid,
                       cap_inspect_result_t *out);

// Low-level: link child_idx into parent_idx's first_child_idx chain.
// Takes g_cap_object_lock. Caller must hold no other cap lock.
void cap_object_link_child(uint32_t parent_idx, uint32_t child_idx);

// ------------------------------------------------------------------------
// Revoke helpers (defined in revoke.c).
// ------------------------------------------------------------------------

// BFS descendant cascade for CAP_FLAG_EAGER_REVOKE. Returns count of
// descendants whose generation was bumped (0 on empty tree).
int revoke_cascade(uint32_t root_idx);

// Iterate g_cap_object_ptrs for owner_pid match, revoke each. Called
// from sched_reap_zombie. Returns count of objects revoked.
int revoke_collect_orphans(int32_t pid);

// Return the object pointer for a slot, or NULL if free/out-of-range.
// No validation beyond array bounds — callers needing generation/audience/
// rights checks should use cap_token_resolve.
static inline cap_object_t *cap_object_get(uint32_t idx) {
    if (idx >= CAP_OBJECT_CAPACITY) return NULL;
    return g_cap_object_ptrs[idx];
}
