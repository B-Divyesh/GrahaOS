// kernel/mm/slab.c — Phase 14: Bonwick slab allocator.
//
// Unit 2 ships the global (lock-protected) path: kmem_cache_create,
// kmem_cache_alloc / _free, slab growth via pmm_alloc_page, list
// bookkeeping, subsystem counters. Per-CPU magazines are added in
// unit 3 on top of this; both the magazine and the global path end up
// calling the same slab_global_alloc / slab_global_free helpers.
//
// Invariants (also asserted at compile time in slab.h):
//   - slab_header_t is exactly 64 bytes (cache-line aligned).
//   - Each slab spans exactly one pmm page (PAGE_SIZE = 4096).
//   - object_size is in [SLAB_MIN_OBJECT_SIZE .. SLAB_MAX_OBJECT_SIZE].
//   - Free-list link is the first sizeof(void *) bytes of a free object.
//   - current_in_use == total_allocated - total_freed at all times.
//
// Memory model:
//   pmm_alloc_page returns a PHYSICAL address. We convert it to the
//   kernel virtual via HHDM (phys + g_hhdm_offset) and store/return
//   the virtual form. pmm_free_page wants a physical address.

#include "slab.h"

#include <stddef.h>
#include <stdint.h>

#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"  // g_hhdm_offset
#include "../../arch/x86_64/cpu/smp.h" // g_cpu_locals, g_cpu_count
#include "../log.h"
#include "../panic.h"

// --- Static kernel-BSS pool of cache structs and registry state ---
// The slab subsystem bootstraps itself: it needs space for its own
// kmem_cache_t objects BEFORE any cache is ready to allocate from.
// Fixed static pool keeps bootstrap trivial.

static kmem_cache_t  g_caches_pool[SLAB_MAX_CACHES];
static uint32_t      g_caches_used   = 0;
static spinlock_t    g_caches_registry_lock = SPINLOCK_INITIALIZER("slab_registry");
static kmem_cache_t *g_caches_first  = NULL;
static kmem_cache_t *g_caches_last   = NULL;
static bool          g_slab_inited   = false;

// --- Helpers ------------------------------------------------------------

static inline void *phys_to_virt(void *phys) {
    return (void *)((uintptr_t)phys + g_hhdm_offset);
}

static inline void *virt_to_phys(void *virt) {
    return (void *)((uintptr_t)virt - g_hhdm_offset);
}

// Compute the slab header containing an object pointer. `obj` must be a
// kernel virtual address inside an HHDM-mapped slab page. The page-aligned
// base of the slab page is the header.
static inline slab_header_t *slab_header_from_obj(void *obj) {
    return (slab_header_t *)((uintptr_t)obj & ~((uintptr_t)PAGE_SIZE - 1));
}

// Round size up to the next multiple of `align`.
static inline uint32_t round_up(uint32_t size, uint32_t align) {
    if (align <= 1) return size;
    return (size + align - 1) & ~(align - 1);
}

// Clip an arbitrary Phase 13 subsystem id into our [0..15] counter bucket.
// Overflow collapses to bucket 0 (SUBSYS_CORE) — intentional: lost precision
// on rarely-used user subsys ids is cheaper than a per-cache hash table.
static inline uint8_t subsys_bucket(uint8_t subsys) {
    return (subsys < SLAB_SUBSYS_BUCKETS) ? subsys : 0;
}

static inline size_t kstrlen(const char *s) {
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

// --- List manipulation (lock held by caller) ---

static void list_push_head(slab_header_t **head, slab_header_t *slab) {
    slab->prev = NULL;
    slab->next = *head;
    if (*head) (*head)->prev = slab;
    *head = slab;
}

static void list_unlink(slab_header_t **head, slab_header_t *slab) {
    if (slab->prev) slab->prev->next = slab->next;
    else            *head            = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    slab->prev = NULL;
    slab->next = NULL;
}

// --- Slab growth: carve a fresh pmm page into a new slab --------------

static slab_header_t *slab_grow_locked(kmem_cache_t *cache) {
    void *page_phys = pmm_alloc_page();
    if (!page_phys) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "slab: pmm exhausted while growing cache '%s'",
             cache->name);
        return NULL;
    }

    slab_header_t *slab = (slab_header_t *)phys_to_virt(page_phys);

    // Initialise header. Page layout:
    //   [0..63]         slab_header_t
    //   [64..N)         object 0   — first sizeof(void*) bytes = next-free-ptr
    //   [N..2N)         object 1
    //   ...
    //   [64+(k-1)*N..)  object k-1
    slab->magic       = SLAB_MAGIC;
    slab->free_count  = cache->objects_per_slab;
    slab->cache       = cache;
    slab->next        = NULL;
    slab->prev        = NULL;
    slab->_pad[0]     = 0;
    slab->_pad[1]     = 0;
    slab->_pad[2]     = 0;

    // Thread the free list through each object in REVERSE order so the
    // first object in memory is popped first (locality-friendly).
    void *prev = NULL;
    char *base = (char *)slab + SLAB_HEADER_SIZE;
    for (int i = (int)cache->objects_per_slab - 1; i >= 0; --i) {
        void *obj = base + (uint32_t)i * cache->object_size;
        *(void **)obj = prev;          // store next-free pointer
        if (cache->object_ctor) {
            // Constructor pattern (Bonwick): init each object once per
            // slab carve. Note: ctor runs BEFORE the free-list link is
            // observed by alloc, so ctor must not touch the first
            // sizeof(void*) bytes, OR we must run it later. We choose
            // the simpler option: Phase 14 caches all pass ctor=NULL.
            // Future phases with stateful ctors will need to rework
            // this — documented as a ctor limitation in slab.h.
            cache->object_ctor(obj);
        }
        prev = obj;
    }
    slab->free_head = prev;  // The first-in-memory object

    cache->total_pages++;
    return slab;
}

// --- Internal alloc / free (lock-protected) -------------------------

static void *slab_global_alloc_locked(kmem_cache_t *cache, uint8_t subsys) {
    slab_header_t *slab = cache->partial_slabs;
    if (!slab) {
        // Try to reuse an empty slab before growing.
        if (cache->empty_slabs) {
            slab = cache->empty_slabs;
            list_unlink(&cache->empty_slabs, slab);
            cache->empty_slab_count--;
            list_push_head(&cache->partial_slabs, slab);
        } else {
            slab = slab_grow_locked(cache);
            if (!slab) return NULL;
            list_push_head(&cache->partial_slabs, slab);
        }
    }

    // Pop one object.
    void *obj = slab->free_head;
    slab->free_head = *(void **)obj;   // advance free list
    slab->free_count--;

    if (slab->free_count == 0) {
        // Slab full → move from partial to full.
        list_unlink(&cache->partial_slabs, slab);
        list_push_head(&cache->full_slabs, slab);
    }

    cache->total_allocated++;
    cache->current_in_use++;
    cache->subsys_counters[subsys_bucket(subsys)]++;
    return obj;
}

static void slab_global_free_locked(kmem_cache_t *cache, void *obj, uint8_t subsys) {
    slab_header_t *slab = slab_header_from_obj(obj);

    if (slab->magic != SLAB_MAGIC) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "slab: reverse-lookup magic fail cache='%s' obj=%p magic=0x%x",
             cache->name, obj, slab->magic);
        kpanic("slab reverse-lookup magic fail");
    }
    if (slab->cache != cache) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "slab: free on wrong cache obj=%p from '%s' freed as '%s'",
             obj, slab->cache->name, cache->name);
        kpanic("slab free on wrong cache");
    }

    // Push onto free list.
    *(void **)obj = slab->free_head;
    slab->free_head = obj;
    slab->free_count++;

    if (slab->free_count == 1) {
        // Slab was full → move from full back to partial.
        list_unlink(&cache->full_slabs, slab);
        list_push_head(&cache->partial_slabs, slab);
    } else if (slab->free_count == cache->objects_per_slab) {
        // Slab now empty → move from partial to empty list.
        list_unlink(&cache->partial_slabs, slab);
        list_push_head(&cache->empty_slabs, slab);
        cache->empty_slab_count++;
    }

    cache->total_freed++;
    cache->current_in_use--;
    uint8_t s = subsys_bucket(subsys);
    if (cache->subsys_counters[s] > 0) {
        cache->subsys_counters[s]--;
    }
}

// --- Public API -------------------------------------------------------

void kmem_slab_init(void) {
    if (g_slab_inited) return;
    g_slab_inited = true;
    klog(KLOG_INFO, SUBSYS_MM, "slab: allocator ready (max_caches=%u)",
         (unsigned)SLAB_MAX_CACHES);
}

kmem_cache_t *kmem_cache_create(const char *name,
                                uint32_t object_size,
                                uint32_t align,
                                void (*ctor)(void *),
                                uint8_t default_subsys) {
    if (!g_slab_inited) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "slab: kmem_cache_create called before kmem_slab_init");
        return NULL;
    }
    if (object_size < SLAB_MIN_OBJECT_SIZE) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "slab: object_size %u too small for '%s' (min=%u)",
             (unsigned)object_size, name, (unsigned)SLAB_MIN_OBJECT_SIZE);
        return NULL;
    }
    if (object_size > SLAB_MAX_OBJECT_SIZE) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "slab: object_size %u too large for '%s' (max=%u)",
             (unsigned)object_size, name, (unsigned)SLAB_MAX_OBJECT_SIZE);
        return NULL;
    }
    if (align < 8) align = 8;  // Minimum alignment for free-list link.

    spinlock_acquire(&g_caches_registry_lock);
    if (g_caches_used >= SLAB_MAX_CACHES) {
        spinlock_release(&g_caches_registry_lock);
        klog(KLOG_ERROR, SUBSYS_MM,
             "slab: registry full (%u), cannot create '%s'",
             (unsigned)SLAB_MAX_CACHES, name);
        return NULL;
    }
    uint32_t idx = g_caches_used++;
    kmem_cache_t *cache = &g_caches_pool[idx];
    spinlock_release(&g_caches_registry_lock);

    // Initialise cache fields.
    size_t n = kstrlen(name);
    if (n >= SLAB_CACHE_NAME_LEN) n = SLAB_CACHE_NAME_LEN - 1;
    for (size_t i = 0; i < n; ++i) cache->name[i] = name[i];
    for (size_t i = n; i < SLAB_CACHE_NAME_LEN; ++i) cache->name[i] = '\0';

    cache->object_size       = round_up(object_size, align);
    cache->align             = align;
    cache->objects_per_slab  = (PAGE_SIZE - SLAB_HEADER_SIZE) / cache->object_size;
    cache->cache_index       = idx;
    cache->default_subsys    = default_subsys;
    cache->_pad0[0]          = 0;
    cache->_pad0[1]          = 0;
    cache->_pad0[2]          = 0;
    cache->object_ctor       = ctor;
    cache->partial_slabs     = NULL;
    cache->full_slabs        = NULL;
    cache->empty_slabs       = NULL;
    cache->empty_slab_count  = 0;
    cache->total_pages       = 0;
    cache->total_allocated   = 0;
    cache->total_freed       = 0;
    cache->current_in_use    = 0;
    cache->next_registry     = NULL;
    for (int i = 0; i < SLAB_SUBSYS_BUCKETS; ++i) {
        cache->subsys_counters[i] = 0;
    }
    spinlock_init(&cache->lock, cache->name);

    // Append to global registry.
    spinlock_acquire(&g_caches_registry_lock);
    if (g_caches_last) {
        g_caches_last->next_registry = cache;
    } else {
        g_caches_first = cache;
    }
    g_caches_last = cache;
    spinlock_release(&g_caches_registry_lock);

    klog(KLOG_INFO, SUBSYS_MM,
         "slab: cache '%s' registered obj=%uB align=%u per_slab=%u idx=%u",
         cache->name,
         (unsigned)cache->object_size,
         (unsigned)cache->align,
         (unsigned)cache->objects_per_slab,
         (unsigned)cache->cache_index);

    return cache;
}

// --- Magazine hot path (Unit 3) -------------------------------------
//
// Each CPU owns one magazine per registered cache (indexed by
// cache->cache_index). Magazines hold up to KMEM_MAGAZINE_CAPACITY
// (8) objects; alloc pops from the top, free pushes onto the top.
// On miss, we acquire the cache lock and refill/flush in batches to
// amortise the global-lock cost.
//
// Concurrency: percpu_preempt_disable / _enable bracket the whole
// magazine op. Since GrahaOS doesn't migrate tasks between CPUs
// (Phase 20 change), disabling interrupts is sufficient to protect
// the current CPU's magazine from ISR-driven reentry.

static int magazine_refill(kmem_cache_t *cache,
                           kmem_magazine_t *mag,
                           uint8_t subsys) {
    // Magazine is empty when we enter. Fill up to capacity.
    int got = 0;
    spinlock_acquire(&cache->lock);
    for (int i = 0; i < KMEM_MAGAZINE_CAPACITY; ++i) {
        void *obj = slab_global_alloc_locked(cache, subsys);
        if (!obj) break;
        mag->objects[got++] = obj;
    }
    spinlock_release(&cache->lock);
    mag->count = (uint8_t)got;
    return got;
}

static void magazine_flush(kmem_cache_t *cache,
                           kmem_magazine_t *mag,
                           uint8_t subsys) {
    // Magazine is full when we enter. Flush half back to global slabs
    // so a subsequent free has room without touching the global path.
    int flush_count = KMEM_MAGAZINE_CAPACITY / 2;
    spinlock_acquire(&cache->lock);
    for (int i = 0; i < flush_count && mag->count > 0; ++i) {
        void *obj = mag->objects[--mag->count];
        slab_global_free_locked(cache, obj, subsys);
    }
    spinlock_release(&cache->lock);
}

static inline void zero_object(const kmem_cache_t *cache, void *obj) {
    // Zero-initialise on every allocation when no ctor is attached.
    // With ctor, the object was initialised once at slab-carve time
    // and we trust the caller to re-initialise fields they mutate.
    if (cache->object_ctor) return;
    uint8_t *p = (uint8_t *)obj;
    for (uint32_t i = 0; i < cache->object_size; ++i) p[i] = 0;
}

void *kmem_cache_alloc_subsys(kmem_cache_t *cache, uint8_t subsys) {
    if (!cache) return NULL;

    percpu_preempt_disable();
    percpu_t *p = percpu_get();
    kmem_magazine_t *mag = &p->magazines[cache->cache_index];

    void *obj = NULL;
    if (mag->count > 0) {
        obj = mag->objects[--mag->count];
    } else {
        // Magazine miss: refill up to capacity, pop one.
        int got = magazine_refill(cache, mag, subsys);
        if (got > 0) {
            obj = mag->objects[--mag->count];
        }
    }
    percpu_preempt_enable();

    if (obj) zero_object(cache, obj);
    return obj;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    return kmem_cache_alloc_subsys(cache, cache ? cache->default_subsys : 0);
}

void kmem_cache_free_subsys(kmem_cache_t *cache, void *obj, uint8_t subsys) {
    if (!cache || !obj) return;

    // Validate the slab header before any magazine-side state change.
    // This keeps corrupted-pointer panics deterministic and ensures we
    // never stash a bogus pointer into a magazine.
    slab_header_t *slab = slab_header_from_obj(obj);
    if (slab->magic != SLAB_MAGIC || slab->cache != cache) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "slab: invalid free cache='%s' obj=%p magic=0x%x header_cache=%p",
             cache->name, obj, slab->magic, slab->cache);
        kpanic("slab invalid free (bad magic or cache mismatch)");
    }

    percpu_preempt_disable();
    percpu_t *p = percpu_get();
    kmem_magazine_t *mag = &p->magazines[cache->cache_index];

    if (mag->count >= KMEM_MAGAZINE_CAPACITY) {
        // Full magazine: flush half first so we always have room.
        magazine_flush(cache, mag, subsys);
    }
    mag->objects[mag->count++] = obj;
    percpu_preempt_enable();
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    kmem_cache_free_subsys(cache, obj, cache ? cache->default_subsys : 0);
}

// Walks all per-CPU magazines of `cache` and ensures none of them
// contains a pointer whose page falls inside [slab, slab + PAGE_SIZE).
// Returns true if the slab can be safely returned to pmm. Used only
// by kmem_cache_shrink (cold path).
//
// NB: we read another CPU's magazine without its preempt_disable. That
// CPU might be pushing or popping concurrently. For shrink this is safe
// in a conservative direction: we'd rather MISS a cached pointer and
// keep the slab (no UAF, just delayed reclaim) than free a page whose
// pointer is still live in a magazine. We snapshot count once and bound
// it defensively.
static bool slab_unreferenced_by_magazines(kmem_cache_t *cache,
                                           slab_header_t *slab) {
    uintptr_t slab_base = (uintptr_t)slab;
    uintptr_t slab_end  = slab_base + PAGE_SIZE;
    uint32_t  cpu_count = g_cpu_count;
    if (cpu_count == 0) cpu_count = 1;  // Pre-SMP boot path
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    for (uint32_t c = 0; c < cpu_count; ++c) {
        kmem_magazine_t *mag = &g_cpu_locals[c].magazines[cache->cache_index];
        uint8_t count = mag->count;
        if (count > KMEM_MAGAZINE_CAPACITY) count = KMEM_MAGAZINE_CAPACITY;
        for (uint8_t i = 0; i < count; ++i) {
            uintptr_t p = (uintptr_t)mag->objects[i];
            if (p >= slab_base && p < slab_end) return false;
        }
    }
    return true;
}

int kmem_cache_shrink(kmem_cache_t *cache) {
    if (!cache) return 0;

    spinlock_acquire(&cache->lock);
    int freed_pages = 0;
    int skipped    = 0;
    // Walk empty_slabs, keeping SLAB_EMPTY_RETAIN as cushion.
    slab_header_t *slab = cache->empty_slabs;
    while (cache->empty_slab_count > (uint32_t)SLAB_EMPTY_RETAIN && slab) {
        slab_header_t *next = slab->next;

        // The slab is on the empty list and holds no live objects — but
        // per-CPU magazines could still cache a pointer into this page.
        // Confirm cross-CPU before returning the page to pmm.
        if (!slab_unreferenced_by_magazines(cache, slab)) {
            skipped++;
            slab = next;
            continue;
        }

        list_unlink(&cache->empty_slabs, slab);
        cache->empty_slab_count--;
        cache->total_pages--;

        void *phys = virt_to_phys(slab);
        spinlock_release(&cache->lock);
        pmm_free_page(phys);
        spinlock_acquire(&cache->lock);
        freed_pages++;
        slab = next;
    }
    spinlock_release(&cache->lock);

    if (freed_pages > 0 || skipped > 0) {
        klog(KLOG_INFO, SUBSYS_MM,
             "slab: shrink '%s' freed %d pages (retained %u empty, %d held by magazines)",
             cache->name, freed_pages,
             (unsigned)SLAB_EMPTY_RETAIN, skipped);
    }
    return freed_pages;
}

kmem_cache_t *kmem_cache_first(void) {
    return g_caches_first;
}

kmem_cache_t *kmem_cache_next(kmem_cache_t *cache) {
    return cache ? cache->next_registry : NULL;
}

uint32_t kmem_cache_count(void) {
    return g_caches_used;
}
