// kernel/mm/kheap.c — Phase 14: power-of-two kernel heap.
//
// Layered on the slab. The 10 buckets are ordinary kmem_cache_t's;
// kmalloc finds the right bucket, allocates from its slab, writes a
// 16-byte header, and returns body = raw + 16. kfree reads the header
// and routes the raw allocation back to the matching cache (or to
// pmm_free_pages for the >8 KiB spill path).
//
// Why not route SUBSYS_MM for all internal allocations? Because the
// header carries the CALLER's subsys tag, the leak scanner can tell
// "net leaks 600 B/s" apart from "mm leaks 600 B/s" without the caller
// having to touch any extra scaffolding.

#include "kheap.h"
#include "slab.h"

#include <stdint.h>
#include <stddef.h>

#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"   // g_hhdm_offset
#include "../log.h"
#include "../panic.h"

// --- Bucket sizes ---
// Eight slab-backed buckets 16..2048. Requests beyond 2032-byte body
// (i.e. bucket need > 2048) route to the pmm-spill path.
const uint32_t kheap_bucket_sizes[KHEAP_BUCKET_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static kmem_cache_t *g_buckets[KHEAP_BUCKET_COUNT];
static bool          g_kheap_inited = false;

// Track spill-bucket stats separately (not a real slab cache).
static uint64_t g_spill_in_use  = 0;   // Number of live spill allocations
static uint64_t g_spill_bytes   = 0;   // Sum of allocated pmm pages * PAGE_SIZE
static uint32_t g_spill_pages   = 0;   // Total pmm pages held by spill
static uint32_t g_spill_subsys[KHEAP_STATS_SUBSYS_BUCKETS];  // Per-subsys live count

// Forward decls for the spill path.
static void *kheap_spill_alloc(size_t size, uint8_t subsys);
static void  kheap_spill_free(void *body, kheap_header_t *hdr);

static inline void *phys_to_virt(void *phys) {
    return (void *)((uintptr_t)phys + g_hhdm_offset);
}

static inline void *virt_to_phys(void *virt) {
    return (void *)((uintptr_t)virt - g_hhdm_offset);
}

static inline uint8_t subsys_bucket(uint8_t subsys) {
    return (subsys < KHEAP_STATS_SUBSYS_BUCKETS) ? subsys : 0;
}

// --- Initialisation ---

void kheap_init(void) {
    if (g_kheap_inited) return;
    // Names: "kheap_16", "kheap_32", ..., "kheap_2048".
    // Eight buckets, default_subsys = SUBSYS_MM (1). Caller-specific
    // subsys is propagated via kmem_cache_alloc_subsys.
    static const char *bucket_names[KHEAP_BUCKET_COUNT] = {
        "kheap_16",   "kheap_32",   "kheap_64",   "kheap_128",
        "kheap_256",  "kheap_512",  "kheap_1024", "kheap_2048",
    };
    for (int i = 0; i < KHEAP_BUCKET_COUNT; ++i) {
        g_buckets[i] = kmem_cache_create(bucket_names[i],
                                         kheap_bucket_sizes[i],
                                         /*align=*/16,
                                         /*ctor=*/NULL,
                                         /*default_subsys=*/SUBSYS_MM);
        if (!g_buckets[i]) {
            klog(KLOG_FATAL, SUBSYS_MM,
                 "kheap: failed to create bucket '%s'", bucket_names[i]);
            kpanic("kheap_init: bucket cache create failed");
        }
    }
    for (int i = 0; i < KHEAP_STATS_SUBSYS_BUCKETS; ++i) {
        g_spill_subsys[i] = 0;
    }
    g_kheap_inited = true;
    klog(KLOG_INFO, SUBSYS_MM,
         "kheap: %u buckets (16..2048) ready; spill via pmm_alloc_pages for >2032-byte body",
         (unsigned)KHEAP_BUCKET_COUNT);
}

// --- Bucket selection ---

uint8_t kheap_bucket_for_size(size_t size) {
    // Smallest bucket that fits (size + header).
    size_t need = size + KHEAP_HEADER_SIZE;
    for (uint8_t i = 0; i < KHEAP_BUCKET_COUNT; ++i) {
        if (kheap_bucket_sizes[i] >= need) return i;
    }
    return KHEAP_SPILL_BUCKET;
}

// --- kmalloc / kfree ---

void *kmalloc(size_t size, uint8_t subsys) {
    if (!g_kheap_inited) {
        klog(KLOG_FATAL, SUBSYS_MM, "kmalloc called before kheap_init");
        return NULL;
    }
    // 0-byte request: return a valid 16 B allocation. Safer than NULL
    // for callers that pass size from user-controlled values; it also
    // makes kmalloc(0)+kfree idempotent.
    if (size == 0) size = 1;
    // Bound requested size so our u16 size field in the header doesn't
    // truncate a large request silently. The spill path handles anything
    // up to a few MiB; beyond that, reject.
    if (size > (16u * 1024u * 1024u)) {
        klog(KLOG_WARN, SUBSYS_MM,
             "kmalloc: oversized request %lu bytes, returning NULL",
             (unsigned long)size);
        return NULL;
    }

    uint8_t bucket = kheap_bucket_for_size(size);
    if (bucket == KHEAP_SPILL_BUCKET) {
        return kheap_spill_alloc(size, subsys);
    }

    void *raw = kmem_cache_alloc_subsys(g_buckets[bucket], subsys);
    if (!raw) return NULL;

    kheap_header_t *hdr = (kheap_header_t *)raw;
    hdr->magic        = KHEAP_MAGIC;
    hdr->bucket_index = bucket;
    hdr->subsys       = subsys;
    hdr->size         = (uint16_t)(size > UINT16_MAX ? UINT16_MAX : size);
    for (int i = 0; i < 8; ++i) hdr->_pad[i] = 0;

    return (char *)raw + KHEAP_HEADER_SIZE;
}

void *kcalloc(size_t n, size_t size, uint8_t subsys) {
    if (n && size > SIZE_MAX / n) {
        klog(KLOG_WARN, SUBSYS_MM,
             "kcalloc: overflow n=%lu size=%lu",
             (unsigned long)n, (unsigned long)size);
        return NULL;
    }
    size_t total = n * size;
    void *p = kmalloc(total, subsys);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; ++i) b[i] = 0;
    }
    return p;
}

void *krealloc(void *p, size_t new_size, uint8_t subsys) {
    if (!p) return kmalloc(new_size, subsys);
    if (new_size == 0) { kfree(p); return NULL; }

    kheap_header_t *hdr = (kheap_header_t *)((char *)p - KHEAP_HEADER_SIZE);
    if (hdr->magic != KHEAP_MAGIC) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "krealloc: magic violation at %p (got 0x%x)", p, hdr->magic);
        kpanic("kheap magic violation in krealloc");
    }
    size_t old_size = hdr->size;

    void *np = kmalloc(new_size, subsys);
    if (!np) return NULL;

    size_t copy = (old_size < new_size) ? old_size : new_size;
    uint8_t *dst = (uint8_t *)np;
    uint8_t *src = (uint8_t *)p;
    for (size_t i = 0; i < copy; ++i) dst[i] = src[i];

    kfree(p);
    return np;
}

void kfree(void *p) {
    if (!p) return;

    kheap_header_t *hdr = (kheap_header_t *)((char *)p - KHEAP_HEADER_SIZE);
    if (hdr->magic != KHEAP_MAGIC) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "kfree: magic violation at %p (got 0x%x, expected 0x%x)",
             p, hdr->magic, KHEAP_MAGIC);
        kpanic("kheap magic violation");
    }

    uint8_t bucket = hdr->bucket_index;
    uint8_t subsys = hdr->subsys;

    // Poison the magic before returning the block. If the same body is
    // freed again, the next kfree sees magic=0 and panics deterministically.
    hdr->magic = 0;

    if (bucket == KHEAP_SPILL_BUCKET) {
        kheap_spill_free(p, hdr);
        return;
    }
    if (bucket >= KHEAP_BUCKET_COUNT) {
        klog(KLOG_FATAL, SUBSYS_MM,
             "kfree: bucket_index %u out of range at %p", bucket, p);
        kpanic("kheap corrupted bucket_index");
    }
    void *raw = (void *)hdr;
    kmem_cache_free_subsys(g_buckets[bucket], raw, subsys);
}

// --- Spill path (>8 KiB) ---

static void *kheap_spill_alloc(size_t size, uint8_t subsys) {
    // Round (size + header) up to PAGE_SIZE.
    size_t total = size + KHEAP_HEADER_SIZE;
    size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    void *phys = pmm_alloc_pages(pages);
    if (!phys) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "kheap_spill: pmm exhausted for %lu-byte alloc (%lu pages)",
             (unsigned long)size, (unsigned long)pages);
        return NULL;
    }

    kheap_header_t *hdr = (kheap_header_t *)phys_to_virt(phys);
    hdr->magic        = KHEAP_MAGIC;
    hdr->bucket_index = KHEAP_SPILL_BUCKET;
    hdr->subsys       = subsys;
    hdr->size         = (uint16_t)(size > UINT16_MAX ? UINT16_MAX : size);
    // Stash the page count in the reserved pad so kfree knows how many
    // pages to return to pmm. We have 8 bytes; u32 is plenty.
    *(uint32_t *)&hdr->_pad[0] = (uint32_t)pages;
    for (int i = 4; i < 8; ++i) hdr->_pad[i] = 0;

    g_spill_in_use++;
    g_spill_pages += (uint32_t)pages;
    g_spill_bytes += pages * PAGE_SIZE;
    g_spill_subsys[subsys_bucket(subsys)]++;

    return (char *)hdr + KHEAP_HEADER_SIZE;
}

static void kheap_spill_free(void *body, kheap_header_t *hdr) {
    uint32_t pages = *(uint32_t *)&hdr->_pad[0];
    uint8_t  sbkt  = subsys_bucket(hdr->subsys);
    (void)body;

    // hdr is already the page-aligned virtual base; compute phys.
    void *phys = virt_to_phys(hdr);
    pmm_free_pages(phys, pages);

    if (g_spill_in_use > 0) g_spill_in_use--;
    if (g_spill_pages >= pages) g_spill_pages -= pages;
    else g_spill_pages = 0;
    if (g_spill_bytes >= pages * PAGE_SIZE) g_spill_bytes -= pages * PAGE_SIZE;
    else g_spill_bytes = 0;
    if (g_spill_subsys[sbkt] > 0) g_spill_subsys[sbkt]--;
}

// --- Stats snapshot ---

static void fill_entry_from_cache(kheap_stats_entry_t *e,
                                  const kmem_cache_t *c) {
    for (int i = 0; i < 32; ++i) e->name[i] = c->name[i];
    e->object_size = c->object_size;
    e->_pad0       = 0;
    e->in_use      = c->current_in_use;
    // "free" = total capacity in partial+empty+full slabs minus in_use,
    // which for a slab allocator is the number of currently-free slots
    // across all partial+empty slabs.
    uint64_t cap = (uint64_t)c->total_pages * c->objects_per_slab;
    e->free        = (cap >= c->current_in_use) ? (cap - c->current_in_use) : 0;
    e->pages       = c->total_pages;
    e->_pad1       = 0;
    for (int i = 0; i < KHEAP_STATS_SUBSYS_BUCKETS; ++i) {
        e->subsys_counters[i] = c->subsys_counters[i];
    }
}

uint32_t kheap_stats_snapshot(kheap_stats_entry_t *out, uint32_t max) {
    if (!out || max == 0) return 0;
    uint32_t written = 0;

    // Iterate the global cache registry. kheap buckets were created
    // first, so they appear first. Typed caches (task_cache,
    // can_entry_cache, …) follow in creation order.
    for (kmem_cache_t *c = kmem_cache_first();
         c && written < max;
         c = kmem_cache_next(c)) {
        fill_entry_from_cache(&out[written++], c);
    }

    // Append a synthetic entry for the spill path if there's room.
    if (written < max) {
        kheap_stats_entry_t *e = &out[written++];
        const char spill_name[] = "kheap_spill";
        int i;
        for (i = 0; spill_name[i] && i < 31; ++i) e->name[i] = spill_name[i];
        for (; i < 32; ++i) e->name[i] = '\0';
        e->object_size = 0;   // variable
        e->_pad0       = 0;
        e->in_use      = g_spill_in_use;
        e->free        = 0;
        e->pages       = g_spill_pages;
        e->_pad1       = 0;
        for (int j = 0; j < KHEAP_STATS_SUBSYS_BUCKETS; ++j) {
            e->subsys_counters[j] = g_spill_subsys[j];
        }
    }
    return written;
}
