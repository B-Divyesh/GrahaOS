// kernel/mm/slab.h — Phase 14: Bonwick slab allocator.
//
// A "cache" is a factory for fixed-size objects of one type. Each cache
// carves 4 KiB pmm pages into equal slots; each slab page's first 64
// bytes are the slab_header_t, the remaining bytes (up to PAGE_SIZE-64)
// are N objects. Free slots are threaded into a singly-linked list using
// the first sizeof(void*) bytes of each free object.
//
// Phase 14 layer model:
//   kheap  → kmem_cache (one per bucket 16..8192 B) → pmm_alloc_page
//   typed  → kmem_cache (one per kernel type)       → pmm_alloc_page
//   spill  → pmm_alloc_pages directly (>8 KiB)
//
// Phase 14 unit 2 ships the global (lock-protected) fast path. Unit 3
// adds per-CPU magazines of 8 objects on top; unit 4 adds shrink.
//
// Allocation tagging: every alloc/free bumps a subsys_counters[] bucket
// keyed by the Phase 13 subsystem id (SUBSYS_CORE=0 .. SUBSYS_TEST=9,
// plus 10..15 for userspace). The leak scanner diffs these to catch
// drift without touching the hot path.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../sync/spinlock.h"
#include "../percpu.h"

// --- Magic / limits ---
#define SLAB_MAGIC                0x5A5BCAFEu  // spec "SLABCAFE" rewritten as valid hex
#define SLAB_CACHE_NAME_LEN       32
#define SLAB_MAX_CACHES           KMEM_MAX_CACHES   // 32; also bounds magazines[]
#define SLAB_HEADER_SIZE          64
#define SLAB_MAX_OBJECT_SIZE      (PAGE_SIZE / 2)   // spec: > 2 KiB rejected
#define SLAB_MIN_OBJECT_SIZE      8                 // need room for free-list link
#define SLAB_EMPTY_RETAIN         1                 // 1 empty slab kept as cushion
#define SLAB_SUBSYS_BUCKETS       16                // 10 kernel + 6 userspace buckets

// Forward decls; defined below.
typedef struct kmem_cache   kmem_cache_t;
typedef struct slab_header  slab_header_t;

// --- Slab header ---
// Placed at the start of every slab page. Reverse lookup on free:
//   slab_header_t *h = (slab_header_t *)((uintptr_t)obj & ~(PAGE_SIZE-1));
// The magic field catches a "wrong pointer" free at very low cost.
struct slab_header {
    uint32_t         magic;        // SLAB_MAGIC
    uint32_t         free_count;   // Number of free objects in this slab
    kmem_cache_t    *cache;        // Owning cache
    slab_header_t   *next;         // Next in list (partial/full/empty)
    slab_header_t   *prev;         // Prev in list
    void            *free_head;    // First free object, links via first 8 bytes
    uint64_t         _pad[3];      // Pad to 64 bytes (offsets 40..63)
};

_Static_assert(sizeof(slab_header_t) == SLAB_HEADER_SIZE,
               "slab_header_t must be exactly SLAB_HEADER_SIZE bytes");

// --- Cache ---
struct kmem_cache {
    char            name[SLAB_CACHE_NAME_LEN];
    uint32_t        object_size;       // Each object's footprint in slab (already aligned)
    uint32_t        align;             // Object alignment (8, 16, 64, ...)
    uint32_t        objects_per_slab;  // (PAGE_SIZE - 64) / object_size
    uint32_t        cache_index;       // Slot in percpu_t.magazines[] (0..31)
    uint8_t         default_subsys;    // Tag applied by kmem_cache_alloc() / _free()
    uint8_t         _pad0[3];
    void          (*object_ctor)(void *);  // Optional; called on first slab carve
    spinlock_t      lock;              // Protects partial/full/empty lists + counters
    slab_header_t  *partial_slabs;     // At least one free object
    slab_header_t  *full_slabs;        // Zero free objects (no-op list; kept for stats)
    slab_header_t  *empty_slabs;       // All objects free (candidates for shrink)
    uint32_t        empty_slab_count;
    uint32_t        total_pages;       // PMM pages currently held by this cache
    uint64_t        total_allocated;   // Lifetime alloc count (monotonic)
    uint64_t        total_freed;       // Lifetime free count (monotonic)
    uint64_t        current_in_use;    // total_allocated - total_freed
    uint32_t        subsys_counters[SLAB_SUBSYS_BUCKETS];  // per-subsys in-use
    kmem_cache_t   *next_registry;     // Next in global registry list
};

// --- Public API ---

// Bootstrap the slab subsystem. Called exactly once, after pmm_init.
// No kmem_cache_create may be called before this.
void kmem_slab_init(void);

// Register a new cache. Returns NULL if:
//   - object_size < 8 or object_size > PAGE_SIZE/2
//   - too many caches (SLAB_MAX_CACHES reached)
//   - name too long (copies up to SLAB_CACHE_NAME_LEN-1 chars)
// ctor may be NULL (default behaviour: objects returned zero-initialised).
// default_subsys is the Phase 13 subsystem id tagged on allocs/frees
// when the caller doesn't override it.
kmem_cache_t *kmem_cache_create(const char *name,
                                uint32_t object_size,
                                uint32_t align,
                                void (*ctor)(void *),
                                uint8_t default_subsys);

// Allocate one object from `cache`, tagged under cache->default_subsys.
// Returns NULL if pmm is exhausted. Returned memory is zero-initialised
// (or ctor-initialised on first carve). O(1) amortized; grows by one
// pmm page when no partial slabs are available.
void *kmem_cache_alloc(kmem_cache_t *cache);

// Same as kmem_cache_alloc but tagged under an explicit subsystem id.
// Used by kheap to propagate the caller's subsys through the bucket.
void *kmem_cache_alloc_subsys(kmem_cache_t *cache, uint8_t subsys);

// Return `obj` to `cache`. The object must have been returned by a
// previous kmem_cache_alloc on the SAME cache. Validates the slab
// header's magic; mismatch → kpanic.
void kmem_cache_free(kmem_cache_t *cache, void *obj);

// Same but with an explicit subsys id (must match the alloc's subsys).
void kmem_cache_free_subsys(kmem_cache_t *cache, void *obj, uint8_t subsys);

// Walk empty_slabs and return all but SLAB_EMPTY_RETAIN pages to pmm.
// Returns the number of pages freed. Expensive: scans every CPU's
// magazine to verify no pointer references the slab being freed.
int kmem_cache_shrink(kmem_cache_t *cache);

// Iterate the global cache registry. Pass the return value back in to
// get the next cache; pass NULL to get the first. The list is append-
// only and stable across the lifetime of the kernel; no lock needed.
kmem_cache_t *kmem_cache_first(void);
kmem_cache_t *kmem_cache_next(kmem_cache_t *cache);

// Total registered cache count (for memstat's buffer-size check).
uint32_t kmem_cache_count(void);
