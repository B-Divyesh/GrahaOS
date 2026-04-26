// kernel/string.h — freestanding kernel libc shim.
// Phase 22 Stage F: replaces kernel/net/string.h (deleted with Mongoose).
// The implementations live in kernel/main.c (memcpy/memset/strlen/strncmp/strcmp)
// and kernel/vsnprintf.c (snprintf family).  Add new declarations here when a
// new kernel call site appears — never re-implement these in module-local
// statics, since duplicate definitions will collide at link time.
#pragma once
#include <stddef.h>

extern void   *memcpy (void *dest, const void *src, size_t n);
extern void   *memset (void *s, int c, size_t n);
extern void   *memmove(void *dest, const void *src, size_t n);
extern int     memcmp (const void *s1, const void *s2, size_t n);
extern size_t  strlen (const char *s);
extern int     strcmp (const char *a, const char *b);
extern int     strncmp(const char *a, const char *b, size_t n);
