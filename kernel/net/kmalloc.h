// kernel/net/kmalloc.h
// Phase 9b: Arena allocator for Mongoose. Renamed net_* in Phase 14 to
// avoid colliding with kernel/mm/kheap.h's Phase 14 kmalloc family.
// Slated for removal in Phase 22 alongside the Mongoose teardown.
#pragma once
#include <stddef.h>

void  net_kmalloc_init(void);
void *net_kmalloc(size_t size);
void  net_kfree(void *ptr);
void *net_kcalloc(size_t nmemb, size_t size);
void *net_krealloc(void *ptr, size_t new_size);
