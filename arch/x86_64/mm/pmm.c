#include "pmm.h"
#include <stdbool.h>
#include <stddef.h>
#include "../../../kernel/sync/spinlock.h"

// Bitmap-based physical memory manager
static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t used_pages = 0;
static uint64_t usable_memory = 0;
static uint64_t last_used_index = 0;

// Phase 17: per-page refcount array. One byte per 4 KiB physical frame.
// Indexed by page number (pa / 4096). refcounts[i] == 0 means "not a live
// page"; live pages are 1..255. Any attempt to increment beyond 255 is
// saturating (not an error). Allocated from the same usable region as the
// bitmap during pmm_init. Sized exactly to total_pages bytes.
static uint8_t *pp_refcounts = NULL;

// PMM spinlock with static initialization
spinlock_t pmm_lock = SPINLOCK_INITIALIZER("pmm");

// Bitmap manipulation functions
static void bitmap_set_bit(uint64_t page) {
    uint64_t byte = page / 8;
    uint8_t bit = page % 8;
    bitmap[byte] |= (1 << bit);
}

static void bitmap_clear_bit(uint64_t page) {
    uint64_t byte = page / 8;
    uint8_t bit = page % 8;
    bitmap[byte] &= ~(1 << bit);
}

static bool bitmap_test_bit(uint64_t page) {
    uint64_t byte = page / 8;
    uint8_t bit = page % 8;
    return (bitmap[byte] & (1 << bit)) != 0;
}

void pmm_init(volatile struct limine_memmap_response *memmap_response) {
    // Initialize the lock
    spinlock_init(&pmm_lock, "pmm");
    
    // No need to lock during init as we're single-threaded here
    struct limine_memmap_entry **entries = memmap_response->entries;
    uint64_t entry_count = memmap_response->entry_count;
    uint64_t highest_addr = 0;

    // Find highest address and calculate usable memory
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE) {
            usable_memory += entries[i]->length;
            uint64_t top = entries[i]->base + entries[i]->length;
            if (top > highest_addr) {
                highest_addr = top;
            }
        }
    }

    // Calculate total pages and bitmap size. Phase 17 extends the PMM
    // bookkeeping with a parallel refcount array (1 byte per page). Lay out
    // [bitmap][refcounts] in the same usable chunk so one find-space loop
    // suffices; both sized to total_pages' worth of storage.
    total_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;
    uint64_t refcount_size = total_pages;  // 1 byte per page
    uint64_t meta_size = bitmap_size + refcount_size;

    // Find space for both in one usable chunk.
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
            entries[i]->length >= meta_size) {
            bitmap = (uint8_t *)(entries[i]->base + 0xFFFF800000000000ULL);
            break;
        }
    }

    if (!bitmap) {
        // Fallback: use direct mapping
        for (uint64_t i = 0; i < entry_count; i++) {
            if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
                entries[i]->length >= meta_size) {
                bitmap = (uint8_t *)entries[i]->base;
                break;
            }
        }
    }

    pp_refcounts = bitmap + bitmap_size;

    // Initialize bitmap - set all pages as used initially
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }
    // Initialize refcounts to 0 (no live pages yet).
    for (uint64_t i = 0; i < refcount_size; i++) {
        pp_refcounts[i] = 0;
    }

    // Mark usable pages as free
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entries[i]->base / PAGE_SIZE;
            uint64_t page_count = entries[i]->length / PAGE_SIZE;

            for (uint64_t j = 0; j < page_count; j++) {
                bitmap_clear_bit(start_page + j);
            }
        }
    }

    // Mark bitmap + refcount-array pages as used (combined meta_size).
    uint64_t bitmap_phys = (uint64_t)bitmap;
    if (bitmap_phys >= 0xFFFF800000000000ULL) {
        bitmap_phys -= 0xFFFF800000000000ULL;
    }
    uint64_t meta_pages = (meta_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < meta_pages; i++) {
        bitmap_set_bit((bitmap_phys / PAGE_SIZE) + i);
        used_pages++;
    }
}

void *pmm_alloc_page(void) {
    spinlock_acquire(&pmm_lock);

    // Start searching from last used index
    for (uint64_t i = last_used_index; i < total_pages; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            pp_refcounts[i] = 1;
            used_pages++;
            last_used_index = i + 1;
            spinlock_release(&pmm_lock);
            return (void *)(i * PAGE_SIZE);
        }
    }

    // If nothing found, search from beginning
    for (uint64_t i = 0; i < last_used_index; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            pp_refcounts[i] = 1;
            used_pages++;
            last_used_index = i + 1;
            spinlock_release(&pmm_lock);
            return (void *)(i * PAGE_SIZE);
        }
    }

    spinlock_release(&pmm_lock);
    return NULL; // Out of memory
}

void *pmm_alloc_pages(size_t num_pages) {
    if (num_pages == 0) return NULL;
    if (num_pages == 1) return pmm_alloc_page();

    spinlock_acquire(&pmm_lock);
    
    uint64_t consecutive_pages = 0;
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test_bit(i)) {
            consecutive_pages++;
        } else {
            consecutive_pages = 0;
        }

        if (consecutive_pages == num_pages) {
            uint64_t start_page = i - num_pages + 1;
            for (uint64_t j = 0; j < num_pages; j++) {
                bitmap_set_bit(start_page + j);
                pp_refcounts[start_page + j] = 1;
            }
            used_pages += num_pages;
            spinlock_release(&pmm_lock);
            return (void *)(start_page * PAGE_SIZE);
        }
    }

    spinlock_release(&pmm_lock);
    return NULL; // Not enough contiguous pages
}

void pmm_free_page(void *page) {
    if (!page) return;

    uint64_t page_index = (uint64_t)page / PAGE_SIZE;

    spinlock_acquire(&pmm_lock);

    if (page_index < total_pages && bitmap_test_bit(page_index)) {
        // Phase 17: decrement refcount; actually free only at zero.
        if (pp_refcounts[page_index] > 0) {
            pp_refcounts[page_index]--;
        }
        if (pp_refcounts[page_index] == 0) {
            bitmap_clear_bit(page_index);
            used_pages--;

            if (page_index < last_used_index) {
                last_used_index = page_index;
            }
        }
    }

    spinlock_release(&pmm_lock);
}

void pmm_free_pages(void *pages, size_t num_pages) {
    if (!pages || num_pages == 0) return;

    uint64_t start_page_index = (uint64_t)pages / PAGE_SIZE;

    spinlock_acquire(&pmm_lock);

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_index = start_page_index + i;
        if (page_index < total_pages && bitmap_test_bit(page_index)) {
            if (pp_refcounts[page_index] > 0) pp_refcounts[page_index]--;
            if (pp_refcounts[page_index] == 0) {
                bitmap_clear_bit(page_index);
                used_pages--;
            }
        }
    }

    if (start_page_index < last_used_index) {
        last_used_index = start_page_index;
    }

    spinlock_release(&pmm_lock);
}

uint64_t pmm_get_total_memory(void) {
    spinlock_acquire(&pmm_lock);
    uint64_t total = usable_memory;
    spinlock_release(&pmm_lock);
    return total;
}

uint64_t pmm_get_free_memory(void) {
    spinlock_acquire(&pmm_lock);
    uint64_t free_mem = usable_memory - (used_pages * PAGE_SIZE);
    spinlock_release(&pmm_lock);
    return free_mem;
}

// ---------------------------------------------------------------------------
// Phase 17: per-page refcount API.
// ---------------------------------------------------------------------------

void pmm_page_ref(void *page) {
    if (!page) return;
    uint64_t idx = (uint64_t)page / PAGE_SIZE;
    spinlock_acquire(&pmm_lock);
    if (idx < total_pages && bitmap_test_bit(idx)) {
        // Saturate at 255. Sharing a page 256-ways pins it forever, which is
        // acceptable for Phase 17 scale (fewer than 256 processes).
        if (pp_refcounts[idx] < 255) pp_refcounts[idx]++;
    }
    spinlock_release(&pmm_lock);
}

void pmm_page_unref(void *page) {
    pmm_free_page(page);  // same semantics: decrement + free-at-zero
}

uint8_t pmm_page_get_refcount(void *page) {
    if (!page) return 0;
    uint64_t idx = (uint64_t)page / PAGE_SIZE;
    if (idx >= total_pages) return 0;
    spinlock_acquire(&pmm_lock);
    uint8_t r = pp_refcounts[idx];
    spinlock_release(&pmm_lock);
    return r;
}