// kernel/net/klib.h
// Phase 9b: Kernel string/format library for Mongoose
// Provides C standard library functions needed by Mongoose in kernel space
#pragma once
#include <stddef.h>
#include <stdarg.h>

// String functions (not provided globally by kernel)
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strncat(char *dest, const char *src, size_t n);
size_t strnlen(const char *s, size_t maxlen);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
void  *memchr(const void *s, int c, size_t n);
char  *strdup(const char *s);

// Format functions
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);

// Conversion functions
long          strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int           atoi(const char *nptr);

// Needed by Mongoose for abort()
void abort(void);
