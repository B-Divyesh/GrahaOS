// libc/src/malloc.c
// Phase 7c: Dynamic memory allocation with free-list algorithm

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Memory alignment (16 bytes for x86_64)
#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Minimum allocation size (including header)
#define MIN_ALLOC_SIZE 32

// Magic number for heap corruption detection
#define BLOCK_MAGIC 0xDEADBEEF

// Block header structure
typedef struct block_header {
    size_t size;              // Size of the data region (not including header)
    bool is_free;             // Free flag
    uint32_t magic;           // Magic number for corruption detection
    struct block_header *next; // Next block in free list
    struct block_header *prev; // Previous block in free list
} block_header_t;

// Head of the free list
static block_header_t *free_list_head = NULL;

// Heap boundaries for safety checks
static void *heap_start = NULL;
static void *heap_end = NULL;

// Heap statistics
static size_t total_allocated = 0;
static size_t total_freed = 0;

// Forward declarations
static block_header_t *find_free_block(size_t size);
static block_header_t *request_space(size_t size);
static void split_block(block_header_t *block, size_t size);
static void coalesce_blocks(block_header_t *block);
static void add_to_free_list(block_header_t *block);
static void remove_from_free_list(block_header_t *block);

/**
 * @brief Find a suitable free block using first-fit strategy
 */
static block_header_t *find_free_block(size_t size) {
    block_header_t *current = free_list_head;

    while (current != NULL) {
        // Verify magic number
        if (current->magic != BLOCK_MAGIC) {
            // Heap corruption detected
            return NULL;
        }

        // Check if block is free and large enough
        if (current->is_free && current->size >= size) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

/**
 * @brief Request more memory from the kernel using sbrk
 */
static block_header_t *request_space(size_t size) {
    // Calculate total size needed (header + data)
    size_t total_size = ALIGN(sizeof(block_header_t) + size);

    // Request memory from kernel
    void *ptr = sbrk(total_size);
    if (ptr == (void *)-1) {
        // sbrk failed
        return NULL;
    }

    // Update heap boundaries
    if (heap_start == NULL) {
        heap_start = ptr;
    }
    heap_end = (void *)((uint8_t *)ptr + total_size);

    // Initialize block header
    block_header_t *block = (block_header_t *)ptr;
    block->size = size;
    block->is_free = false;
    block->magic = BLOCK_MAGIC;
    block->next = NULL;
    block->prev = NULL;

    total_allocated += size;

    return block;
}

/**
 * @brief Split a block if it's larger than needed
 */
static void split_block(block_header_t *block, size_t size) {
    // Only split if remaining space is large enough for a new block
    size_t remaining = block->size - size;
    size_t min_split_size = ALIGN(sizeof(block_header_t) + MIN_ALLOC_SIZE);

    if (remaining >= min_split_size) {
        // Create new block from the remaining space
        block_header_t *new_block = (block_header_t *)((uint8_t *)(block + 1) + size);

        // CRITICAL: Validate new block is within heap bounds
        if ((void *)new_block < heap_start || (void *)new_block >= heap_end) {
            // New block would be outside heap, don't split
            return;
        }

        if ((void *)((uint8_t *)new_block + sizeof(block_header_t)) > heap_end) {
            // Not enough space for new block header, don't split
            return;
        }

        new_block->size = remaining - sizeof(block_header_t);
        new_block->is_free = true;
        new_block->magic = BLOCK_MAGIC;
        new_block->next = NULL;
        new_block->prev = NULL;

        // Update original block size
        block->size = size;

        // Add new block to free list
        add_to_free_list(new_block);
    }
}

/**
 * @brief Coalesce adjacent free blocks
 */
static void coalesce_blocks(block_header_t *block) {
    if (!block || !block->is_free) {
        return;
    }

    // Calculate where next block would be
    block_header_t *next_block = (block_header_t *)((uint8_t *)(block + 1) + block->size);

    // CRITICAL FIX: Check if next_block is within heap bounds before dereferencing!
    // This prevents page faults when the block is at the end of the heap.
    if ((void *)next_block >= heap_end || (void *)next_block < heap_start) {
        // Next block is outside heap boundaries, cannot coalesce
        return;
    }

    // Check if next block has enough space for a valid header
    if ((void *)((uint8_t *)next_block + sizeof(block_header_t)) > heap_end) {
        // Not enough space for a valid block header
        return;
    }

    // Now it's safe to check the magic number and is_free flag
    if (next_block->magic == BLOCK_MAGIC && next_block->is_free) {
        // Remove next block from free list
        remove_from_free_list(next_block);

        // Merge blocks
        block->size += sizeof(block_header_t) + next_block->size;
    }
}

/**
 * @brief Add block to free list
 */
static void add_to_free_list(block_header_t *block) {
    if (!block) return;

    // CRITICAL: Validate block is within heap bounds
    if (heap_start != NULL) {
        if ((void *)block < heap_start || (void *)block >= heap_end) {
            // Block outside heap, don't add to free list
            return;
        }
    }

    // CRITICAL: Validate magic number before adding to list
    if (block->magic != BLOCK_MAGIC) {
        // Corrupted block, don't add to free list
        return;
    }

    block->is_free = true;

    // Insert at head of free list
    block->next = free_list_head;
    block->prev = NULL;

    if (free_list_head) {
        free_list_head->prev = block;
    }

    free_list_head = block;
}

/**
 * @brief Remove block from free list
 */
static void remove_from_free_list(block_header_t *block) {
    if (!block) return;

    if (block->prev) {
        block->prev->next = block->next;
    } else {
        // This is the head
        free_list_head = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
    block->is_free = false;
}

/**
 * @brief Allocate memory
 */
void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    // Align size
    size = ALIGN(size);

    // Try to find a free block
    block_header_t *block = find_free_block(size);

    if (block) {
        // Found a free block
        remove_from_free_list(block);

        // Split if block is too large
        split_block(block, size);

        // Return pointer to data region
        return (void *)(block + 1);
    }

    // No suitable free block, request more space
    block = request_space(size);
    if (!block) {
        return NULL;
    }

    // Return pointer to data region
    return (void *)(block + 1);
}

/**
 * @brief Free allocated memory
 */
void free(void *ptr) {
    if (!ptr) {
        return;
    }

    // CRITICAL: Validate pointer is in heap bounds BEFORE dereferencing
    if (heap_start != NULL) {  // Only check if heap has been initialized
        if (ptr < heap_start || ptr >= heap_end) {
            // Pointer outside heap boundaries, invalid
            return;
        }
    }

    // Get block header
    block_header_t *block = (block_header_t *)ptr - 1;

    // CRITICAL: Validate block header is also in heap bounds
    if (heap_start != NULL) {
        if ((void *)block < heap_start || (void *)block >= heap_end) {
            // Block header outside heap, invalid
            return;
        }
    }

    // Verify magic number
    if (block->magic != BLOCK_MAGIC) {
        // Heap corruption or invalid pointer
        return;
    }

    // Already free?
    if (block->is_free) {
        // Double free detected
        return;
    }

    total_freed += block->size;

    // Add to free list
    add_to_free_list(block);

    // Coalesce with adjacent blocks
    coalesce_blocks(block);
}

/**
 * @brief Allocate and zero memory
 */
void *calloc(size_t nmemb, size_t size) {
    // Check for overflow
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }

    size_t total_size = nmemb * size;
    void *ptr = malloc(total_size);

    if (ptr) {
        memset(ptr, 0, total_size);
    }

    return ptr;
}

/**
 * @brief Reallocate memory
 */
void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // CRITICAL: Validate pointer is within heap bounds
    if (heap_start != NULL) {
        if (ptr < heap_start || ptr >= heap_end) {
            // Invalid pointer
            return NULL;
        }
    }

    // Get current block
    block_header_t *block = (block_header_t *)ptr - 1;

    // CRITICAL: Validate block header is within heap bounds
    if (heap_start != NULL) {
        if ((void *)block < heap_start || (void *)block >= heap_end) {
            // Invalid block header
            return NULL;
        }
    }

    // Verify magic number
    if (block->magic != BLOCK_MAGIC) {
        return NULL;
    }

    // If new size fits in current block, just return it
    if (size <= block->size) {
        return ptr;
    }

    // Allocate new block
    void *new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    // Copy data
    memcpy(new_ptr, ptr, block->size);

    // Free old block
    free(ptr);

    return new_ptr;
}
