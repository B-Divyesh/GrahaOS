// kernel/cap/handle_table.c
// Phase 15a: Per-process capability handle table.
//
// Growing scheme: start at 16 slots, double on exhaustion up to 1024.
// Backing storage is a plain `cap_handle_entry_t[capacity]` array obtained
// via kmalloc (SUBSYS_CAP tag). On grow we kmalloc a fresh larger array,
// memcpy entries, re-thread the free list to include the new slots, and
// kfree the old array.
//
// Free list: `next_free` field in each free entry chains to the next free
// slot. Head is `table->next_free`. When free list is empty, we grow (if
// capacity < CAP_HANDLE_MAX) or return ENOMEM.

#include "handle_table.h"
#include "object.h"
#include "token.h"

#include <stdint.h>
#include <stddef.h>

#include "../mm/kheap.h"
#include "../log.h"

// Local helpers.
static void rebuild_free_list_in_range(cap_handle_entry_t *entries,
                                       uint32_t start, uint32_t end,
                                       uint32_t *head_inout) {
    // Thread entries [start..end) into the free list, pushing onto head.
    for (uint32_t i = end; i-- > start; ) {
        entries[i].object_idx        = CAP_OBJECT_IDX_NONE;
        entries[i].local_generation  = 1;
        entries[i].token_flags       = 0;
        entries[i].reserved[0]       = 0;
        entries[i].reserved[1]       = 0;
        entries[i].reserved[2]       = 0;
        entries[i].next_free         = *head_inout;
        *head_inout                  = i;
    }
}

int cap_handle_table_init(cap_handle_table_t *t) {
    if (!t) return CAP_V2_EFAULT;

    t->capacity  = CAP_HANDLE_INITIAL_CAPACITY;
    t->count     = 0;
    t->next_free = CAP_HANDLE_SLOT_NONE;
    t->reserved  = 0;
    spinlock_init(&t->lock, "cap_handle_table");

    size_t bytes = (size_t)t->capacity * sizeof(cap_handle_entry_t);
    t->entries = (cap_handle_entry_t *)kmalloc(bytes, SUBSYS_CAP);
    if (!t->entries) {
        t->capacity = 0;
        return CAP_V2_ENOMEM;
    }

    rebuild_free_list_in_range(t->entries, 0, t->capacity, &t->next_free);
    return CAP_V2_OK;
}

void cap_handle_table_free(cap_handle_table_t *t) {
    if (!t) return;
    spinlock_acquire(&t->lock);
    if (t->entries) {
        kfree(t->entries);
        t->entries = NULL;
    }
    t->capacity  = 0;
    t->count     = 0;
    t->next_free = CAP_HANDLE_SLOT_NONE;
    spinlock_release(&t->lock);
}

int cap_handle_grow(cap_handle_table_t *t) {
    if (!t) return CAP_V2_EFAULT;
    if (t->capacity >= CAP_HANDLE_MAX) return CAP_V2_ENOMEM;

    uint32_t new_cap = t->capacity * 2;
    if (new_cap > CAP_HANDLE_MAX) new_cap = CAP_HANDLE_MAX;

    size_t new_bytes = (size_t)new_cap * sizeof(cap_handle_entry_t);
    cap_handle_entry_t *new_entries = (cap_handle_entry_t *)kmalloc(new_bytes, SUBSYS_CAP);
    if (!new_entries) return CAP_V2_ENOMEM;

    // Copy existing entries.
    size_t old_bytes = (size_t)t->capacity * sizeof(cap_handle_entry_t);
    uint8_t *dst = (uint8_t *)new_entries;
    const uint8_t *src = (const uint8_t *)t->entries;
    for (size_t i = 0; i < old_bytes; i++) dst[i] = src[i];

    // Thread new slots [old_cap..new_cap) onto the free list.
    uint32_t head = t->next_free;
    rebuild_free_list_in_range(new_entries, t->capacity, new_cap, &head);

    cap_handle_entry_t *old = t->entries;
    t->entries   = new_entries;
    t->capacity  = new_cap;
    t->next_free = head;

    if (old) kfree(old);
    return CAP_V2_OK;
}

int cap_handle_insert(cap_handle_table_t *t, uint32_t object_idx,
                      uint8_t token_flags, uint32_t *slot_out) {
    if (!t || !slot_out) return CAP_V2_EFAULT;
    if (object_idx == CAP_OBJECT_IDX_NONE) return CAP_V2_EINVAL;

    spinlock_acquire(&t->lock);

    if (t->next_free == CAP_HANDLE_SLOT_NONE) {
        int r = cap_handle_grow(t);
        if (r != CAP_V2_OK) {
            spinlock_release(&t->lock);
            return r;
        }
    }

    uint32_t slot = t->next_free;
    t->next_free  = t->entries[slot].next_free;

    t->entries[slot].object_idx = object_idx;
    t->entries[slot].token_flags = token_flags;
    // local_generation persists across reuse so stale tokens fail.
    t->entries[slot].next_free = CAP_HANDLE_SLOT_NONE;
    t->count++;

    uint32_t local_gen = t->entries[slot].local_generation;
    spinlock_release(&t->lock);

    *slot_out = slot;
    return (int)local_gen;
}

cap_handle_entry_t *cap_handle_lookup(cap_handle_table_t *t, uint32_t slot) {
    if (!t || !t->entries) return NULL;
    if (slot >= t->capacity) return NULL;
    cap_handle_entry_t *e = &t->entries[slot];
    if (e->object_idx == CAP_OBJECT_IDX_NONE) return NULL;
    return e;
}

int cap_handle_remove(cap_handle_table_t *t, uint32_t slot) {
    if (!t) return CAP_V2_EFAULT;
    if (slot >= t->capacity) return CAP_V2_EINVAL;

    spinlock_acquire(&t->lock);
    cap_handle_entry_t *e = &t->entries[slot];
    if (e->object_idx == CAP_OBJECT_IDX_NONE) {
        spinlock_release(&t->lock);
        return CAP_V2_EINVAL;
    }
    e->object_idx = CAP_OBJECT_IDX_NONE;
    e->local_generation++;
    e->token_flags = 0;
    e->next_free   = t->next_free;
    t->next_free   = slot;
    t->count--;
    spinlock_release(&t->lock);
    return CAP_V2_OK;
}
