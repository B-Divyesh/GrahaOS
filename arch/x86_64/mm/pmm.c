#include "pmm.h"
#include <stdbool.h>
#include <stddef.h> // For NULL

// Simple bitmap-based physical memory manager
static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t used_pages = 0;
static uint64_t usable_memory = 0;
static uint64_t last_used_index = 0;

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

// FIXED: Accept volatile pointer but use non-volatile local variable
void pmm_init(volatile struct limine_memmap_response *memmap_response) {
    // FIX: The local variable `entries` should not be volatile.
    // We are reading a value from a volatile location into a stable local variable.
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

    // Calculate total pages and bitmap size
    total_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;  // Round up

    // Find space for bitmap in usable memory
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
            entries[i]->length >= bitmap_size) {
            // Use higher half mapping (assuming Limine sets this up)
            bitmap = (uint8_t *)(entries[i]->base + 0xFFFF800000000000ULL);
            break;
        }
    }

    if (!bitmap) {
        // Fallback: use direct mapping if higher half isn't available
        for (uint64_t i = 0; i < entry_count; i++) {
            if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
                entries[i]->length >= bitmap_size) {
                bitmap = (uint8_t *)entries[i]->base;
                break;
            }
        }
    }

    // Initialize bitmap - set all pages as used initially
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
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

    // Mark bitmap pages as used
    uint64_t bitmap_phys = (uint64_t)bitmap;
    if (bitmap_phys >= 0xFFFF800000000000ULL) {
        bitmap_phys -= 0xFFFF800000000000ULL;
    }
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < bitmap_pages; i++) {
        bitmap_set_bit((bitmap_phys / PAGE_SIZE) + i);
        used_pages++;
    }
}

void *pmm_alloc_page(void) {
    // Start searching from last used index for better performance
    for (uint64_t i = last_used_index; i < total_pages; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            used_pages++;
            last_used_index = i + 1;
            return (void *)(i * PAGE_SIZE);
        }
    }

    // If nothing found, search from beginning
    for (uint64_t i = 0; i < last_used_index; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            used_pages++;
            last_used_index = i + 1;
            return (void *)(i * PAGE_SIZE);
        }
    }

    return NULL; // Out of memory
}

// Function to allocate multiple contiguous pages
void *pmm_alloc_pages(size_t num_pages) {
    if (num_pages == 0) return NULL;
    if (num_pages == 1) return pmm_alloc_page();

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
                used_pages++;
            }
            return (void *)(start_page * PAGE_SIZE);
        }
    }

    return NULL; // Not enough contiguous pages
}

void pmm_free_page(void *page) {
    uint64_t page_index = (uint64_t)page / PAGE_SIZE;

    if (page_index < total_pages && bitmap_test_bit(page_index)) {
        bitmap_clear_bit(page_index);
        used_pages--;

        // Update last_used_index for faster allocation
        if (page_index < last_used_index) {
            last_used_index = page_index;
        }
    }
}

uint64_t pmm_get_total_memory(void) {
    return usable_memory;
}

uint64_t pmm_get_free_memory(void) {
    return usable_memory - (used_pages * PAGE_SIZE);
}