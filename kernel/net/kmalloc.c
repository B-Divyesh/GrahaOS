// kernel/net/kmalloc.c
// Phase 9b: Arena-based kernel memory allocator for Mongoose
// Pre-allocates 2MB from PMM, implements free-list allocator within

#include "kmalloc.h"
#include <stdint.h>
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"

extern uint64_t g_hhdm_offset;

// Arena configuration
#define KMALLOC_ARENA_PAGES 512  // 2MB
#define KMALLOC_ARENA_SIZE  (KMALLOC_ARENA_PAGES * PAGE_SIZE)

// Block header for free-list allocator
struct block_header {
    size_t size;                // Usable size (excluding header)
    struct block_header *next;  // Next block in list
    int free;                   // 1 = free, 0 = allocated
};

#define HEADER_SIZE sizeof(struct block_header)

// Arena state
static uint8_t *arena_base = NULL;
static struct block_header *free_list = NULL;
static int kmalloc_initialized = 0;

void kmalloc_init(void) {
    if (kmalloc_initialized) return;

    // Allocate contiguous physical pages
    void *phys = pmm_alloc_pages(KMALLOC_ARENA_PAGES);
    if (!phys) return;

    // Convert to virtual address
    arena_base = (uint8_t *)((uint64_t)phys + g_hhdm_offset);

    // Initialize as single large free block
    free_list = (struct block_header *)arena_base;
    free_list->size = KMALLOC_ARENA_SIZE - HEADER_SIZE;
    free_list->next = NULL;
    free_list->free = 1;

    kmalloc_initialized = 1;
}

void *kmalloc(size_t size) {
    if (size == 0 || !kmalloc_initialized) return NULL;

    // Align to 16 bytes
    size = (size + 15) & ~15;

    // First-fit search
    struct block_header *curr = free_list;
    while (curr != NULL) {
        if (curr->free && curr->size >= size) {
            // Split if remainder is large enough
            if (curr->size >= size + HEADER_SIZE + 16) {
                struct block_header *new_block =
                    (struct block_header *)((uint8_t *)curr + HEADER_SIZE + size);
                new_block->size = curr->size - size - HEADER_SIZE;
                new_block->free = 1;
                new_block->next = curr->next;
                curr->size = size;
                curr->next = new_block;
            }
            curr->free = 0;
            return (void *)((uint8_t *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    return NULL;  // Arena exhausted
}

void kfree(void *ptr) {
    if (!ptr || !kmalloc_initialized) return;

    // Validate pointer is within arena
    if ((uint8_t *)ptr < arena_base + HEADER_SIZE ||
        (uint8_t *)ptr >= arena_base + KMALLOC_ARENA_SIZE) {
        return;
    }

    struct block_header *block =
        (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);
    block->free = 1;

    // Coalesce with next block if free
    if (block->next && block->next->free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    // Coalesce with previous block if free
    struct block_header *curr = free_list;
    while (curr && curr->next != block) {
        curr = curr->next;
    }
    if (curr && curr->free && curr->next == block) {
        curr->size += HEADER_SIZE + block->size;
        curr->next = block->next;
    }
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (total == 0) return NULL;
    void *ptr = kmalloc(total);
    if (ptr) {
        // Use extern memset from kernel/main.c
        extern void *memset(void *, int, size_t);
        memset(ptr, 0, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block_header *block =
        (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);
    size_t old_size = block->size;

    if (new_size <= old_size) return ptr;

    // Allocate new block, copy, free old
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    extern void *memcpy(void *, const void *, size_t);
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}
