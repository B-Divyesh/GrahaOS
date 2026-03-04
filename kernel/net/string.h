// kernel/net/string.h — stub for freestanding environment
// Mongoose's chacha20-poly1305 code includes <string.h>
// All string/memory functions are already declared in mongoose_config.h
#pragma once
#include <stddef.h>
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern int memcmp(const void *s1, const void *s2, size_t n);
extern size_t strlen(const char *str);
