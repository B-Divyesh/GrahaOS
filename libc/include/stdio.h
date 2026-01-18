// libc/include/stdio.h
#pragma once

#include <stddef.h>
#include <stdarg.h>

// Standard I/O streams (not yet implemented)
#define stdin  ((FILE *)0)
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)

// FILE structure placeholder
typedef struct {
    int fd;
    int flags;
} FILE;

// End-of-file indicator
#define EOF (-1)

// Character I/O
int putchar(int c);
int puts(const char *s);
int getchar(void);

// Formatted output
int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *str, const char *format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *str, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *format, __builtin_va_list ap);
int vsprintf(char *str, const char *format, __builtin_va_list ap);
int vsnprintf(char *str, size_t size, const char *format, __builtin_va_list ap);

// File operations (placeholders for future implementation)
FILE *fopen(const char *pathname, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
