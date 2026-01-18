// libc/include/stdlib.h
#pragma once

#include <stddef.h>

// Process control
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

// Memory allocation
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

// Program break
void *sbrk(ptrdiff_t increment);
int brk(void *addr);

// String conversion
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

// Pseudo-random number generation
int rand(void);
void srand(unsigned int seed);

// Absolute value
int abs(int n);
long labs(long n);
long long llabs(long long n);

// Division
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);
