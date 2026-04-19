// kernel/mm/kheap.h — Phase 14: power-of-two kernel heap.
//
// Layered on the slab: 10 buckets at sizes 16/32/64/128/256/512/1024/
// 2048/4096/8192 bytes, each backed by a dedicated kmem_cache_t. A 16-
// byte header precedes every allocation body and records:
//   - magic (0xBEEFBEEF): corruption / double-free detector
//   - bucket_index: 0..9 for slab buckets, 255 for direct-pmm spill
//   - subsys: Phase 13 subsystem id (for leak attribution / memstat)
//   - size: requested size in bytes (debug only)
//
// Requests > 8192 B take the spill path: pmm_alloc_pages(ceil((size+16)/PAGE_SIZE)),
// header at page base, body at base+16. kfree() reads the header and
// routes to the right backend.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --- Magic / shape ---
#define KHEAP_MAGIC         0xBEEFBEEFu  // spec "KHEAPBEE" rewritten as valid hex
#define KHEAP_HEADER_SIZE   16
// Phase 14 slab caps object_size at PAGE_SIZE/2 = 2048 (single-page slabs,
// simple reverse lookup via obj & ~0xFFF). That forbids slab-backed
// buckets at 4096 and 8192. We keep the spec's "power-of-two kheap"
// semantics by routing requests > 2048-16 through the pmm-spill path
// (tracked under the synthetic kheap_spill entry in memstat). Out-of-
// spec addition #11 in plans/scalable-stirring-gem.md.
#define KHEAP_BUCKET_COUNT  8
#define KHEAP_SPILL_BUCKET  255           // sentinel for >2048 direct-pmm allocations
#define KHEAP_STATS_SUBSYS_BUCKETS 16     // matches slab.h SLAB_SUBSYS_BUCKETS

// Bucket sizes: 2^(4+i) for i in 0..7. Each size is the full slab
// allocation footprint (body + 16-byte header).
// Exposed so tests can iterate deterministically.
extern const uint32_t kheap_bucket_sizes[KHEAP_BUCKET_COUNT];

// --- Header (prepended to every kmalloc'd body) ---
typedef struct kheap_header {
    uint32_t magic;          // KHEAP_MAGIC
    uint8_t  bucket_index;   // 0..9, or KHEAP_SPILL_BUCKET (255)
    uint8_t  subsys;         // Phase 13 subsystem id
    uint16_t size;           // Caller's requested byte count (debug)
    uint8_t  _pad[8];        // Pad to 16 bytes
} kheap_header_t;

_Static_assert(sizeof(kheap_header_t) == KHEAP_HEADER_SIZE,
               "kheap_header_t must be exactly 16 bytes");

// --- Stats entry (SYS_KHEAP_STATS output row) ---
// Exactly 128 bytes, no compiler-inserted padding. Matched byte-for-byte
// in user/syscalls.h so memstat and kheap_basic can parse without a
// shared kernel header.
typedef struct kheap_stats_entry {
    char     name[32];                                   // offset   0
    uint32_t object_size;                                // offset  32
    uint32_t _pad0;                                      // offset  36 — align
    uint64_t in_use;                                     // offset  40
    uint64_t free;                                       // offset  48
    uint32_t pages;                                      // offset  56
    uint32_t _pad1;                                      // offset  60 — align
    uint32_t subsys_counters[KHEAP_STATS_SUBSYS_BUCKETS]; // offset  64 (16*4)
} kheap_stats_entry_t;

_Static_assert(sizeof(kheap_stats_entry_t) == 128,
               "kheap_stats_entry_t must be exactly 128 bytes for ABI stability");

// --- Public API ---

// Initialise 10 buckets as kmem_cache_t. Must be called exactly once at
// boot, AFTER kmem_slab_init() and BEFORE any kmalloc caller.
void kheap_init(void);

// Variable-size kernel allocation. `size` is rounded up to the next
// power-of-two bucket (min 16 B). `subsys` is a Phase 13 id used for
// leak attribution; SUBSYS_MM is a reasonable default for code that
// doesn't have a more specific owner.
//
// kmalloc(0, _) returns a 16-byte allocation (never NULL for a valid
// request). Returns NULL only on pmm exhaustion.
void *kmalloc(size_t size, uint8_t subsys);

// Zero-initialised variant. Computes `n * size` and delegates to
// kmalloc, then memsets to 0. Returns NULL on overflow or pmm failure.
void *kcalloc(size_t n, size_t size, uint8_t subsys);

// Grow or shrink an allocation. NULL `p` is equivalent to kmalloc.
// `new_size == 0` is equivalent to kfree (returns NULL).
void *krealloc(void *p, size_t new_size, uint8_t subsys);

// Free an allocation returned by kmalloc / kcalloc / krealloc.
// NULL is a no-op. Corrupted header → kpanic.
void kfree(void *p);

// Snapshot current allocator state into `out` (up to `max` entries).
// Order: 10 kheap bucket caches first (always), then any typed caches
// registered via kmem_cache_create (task_cache, can_entry_cache, …).
// Returns number of entries written.
uint32_t kheap_stats_snapshot(kheap_stats_entry_t *out, uint32_t max);

// Exposed for unit tests and memstat. Bucket index for a requested
// size, or KHEAP_SPILL_BUCKET when size > 8 KiB.
uint8_t kheap_bucket_for_size(size_t size);
