// arch/x86_64/mm/pmm.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../../../kernel/limine.h"

#include "../../../kernel/sync/spinlock.h"

#define PAGE_SIZE 4096

/**
 * @brief Initialize the Physical Memory Manager
 * @param memmap_response Pointer to Limine memory map response
 */
void pmm_init(volatile struct limine_memmap_response *memmap_response);

/**
 * @brief Allocate a single physical page
 * @return Physical address of allocated page, or NULL if out of memory
 */
void *pmm_alloc_page(void);

/**
 * @brief Allocate multiple contiguous physical pages
 * @param num_pages Number of pages to allocate
 * @return Physical address of first allocated page, or NULL if out of memory
 */
void *pmm_alloc_pages(size_t num_pages);

/**
 * @brief Free a previously allocated physical page
 * @param page Physical address of page to free
 */
void pmm_free_page(void *page);

/**
 * @brief Free multiple contiguous physical pages
 * @param pages Physical address of first page
 * @param num_pages Number of pages to free
 */
void pmm_free_pages(void *pages, size_t num_pages);

/**
 * @brief Get total amount of usable memory in bytes
 * @return Total usable memory
 */
uint64_t pmm_get_total_memory(void);

/**
 * @brief Get amount of free memory in bytes
 * @return Free memory
 */
uint64_t pmm_get_free_memory(void);

// ---------------------------------------------------------------------------
// Phase 17: per-page reference counting (required by VMO COW).
//
// Every page starts with refcount=1 when pmm_alloc_page returns it. Callers
// that share a page between multiple owners (e.g., COW VMO parent & child)
// call pmm_page_ref() once per additional owner. pmm_free_page() now
// decrements; the page is returned to the bitmap only when refcount==0.
//
// Counts saturate at 255. Freeing a page at refcount==0 is a no-op (the
// existing bitmap check protects against double-free).
// ---------------------------------------------------------------------------
void pmm_page_ref(void *page);
void pmm_page_unref(void *page);   // alias for pmm_free_page
uint8_t pmm_page_get_refcount(void *page);