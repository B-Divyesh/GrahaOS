// kernel/net/kmalloc.h
// Phase 9b: Kernel memory allocator for Mongoose
#pragma once
#include <stddef.h>

void  kmalloc_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *ptr, size_t new_size);
