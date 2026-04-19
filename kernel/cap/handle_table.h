// kernel/cap/handle_table.h
// Phase 15a: Per-process capability handle table.
//
// Userspace never sees cap_object_t indexes directly. Instead it receives
// cap_token_t values that pack {generation, idx, flags}. On the kernel
// side, each process carries a cap_handle_table_t — a sparse array of
// slots where each slot records the cap_object_t idx, a per-slot
// local_generation (bumped on close so reused slots invalidate stale
// tokens), and the holder-view flags. The table grows on demand.
//
// Entries are allocated in 16-slot chunks from kheap (kheap_256 bucket
// at 16 bytes/entry). Capacity doubles on exhaustion up to
// CAP_HANDLE_MAX = 1024. Free list threaded via next_free.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "token.h"
#include "../sync/spinlock.h"

#define CAP_HANDLE_INITIAL_CAPACITY 16
#define CAP_HANDLE_MAX              1024
#define CAP_HANDLE_SLOT_NONE        0xFFFFFFFFu

// 16-byte entry.
typedef struct cap_handle_entry {
    uint32_t object_idx;       // Index into g_cap_object_ptrs. CAP_OBJECT_IDX_NONE = free.
    uint32_t local_generation; // Per-slot gen, bumped on close.
    uint8_t  token_flags;      // Holder-view flags (subset of object flags).
    uint8_t  reserved[3];
    uint32_t next_free;        // Free-list link (CAP_HANDLE_SLOT_NONE if not free).
} cap_handle_entry_t;

_Static_assert(sizeof(cap_handle_entry_t) == 16, "cap_handle_entry_t must be 16 bytes");

typedef struct cap_handle_table {
    cap_handle_entry_t *entries;
    uint32_t            capacity;   // Always a power of two, <= CAP_HANDLE_MAX
    uint32_t            count;      // Live (non-free) slots
    uint32_t            next_free;  // Head of free list
    uint32_t            reserved;
    spinlock_t          lock;
} cap_handle_table_t;

// ---- Lifecycle ----

// Init a freshly-zeroed table. Allocates the initial 16-slot chunk.
// Returns 0 on success, CAP_V2_ENOMEM if kmalloc fails.
int cap_handle_table_init(cap_handle_table_t *t);

// Release all slots + free the backing chunk. Called from sched_reap_zombie.
// Does NOT revoke the referenced objects — that's the caller's job via
// revoke_collect_orphans.
void cap_handle_table_free(cap_handle_table_t *t);

// ---- Insert / lookup / remove ----

// Insert object_idx at a new or reused slot with the given holder-view flags.
// On success stores the slot index in *slot_out (suitable for token idx) and
// returns the new slot's local_generation (suitable for token gen) as return.
// Returns negative CAP_V2_* on failure (ENOMEM if at CAP_HANDLE_MAX).
int cap_handle_insert(cap_handle_table_t *t, uint32_t object_idx,
                      uint8_t token_flags, uint32_t *slot_out);

// Look up a slot; returns pointer to the entry or NULL if invalid.
// No locks held across the return — caller must not dereference the
// pointer after releasing whatever context it operates in.
cap_handle_entry_t *cap_handle_lookup(cap_handle_table_t *t, uint32_t slot);

// Free a slot (returns it to the free list and bumps local_generation).
// Returns 0 on success, CAP_V2_EINVAL if slot is out-of-range or already free.
int cap_handle_remove(cap_handle_table_t *t, uint32_t slot);

// Grow the table by doubling capacity (up to CAP_HANDLE_MAX). Returns 0 on
// success, CAP_V2_ENOMEM if already at max or realloc fails.
int cap_handle_grow(cap_handle_table_t *t);
