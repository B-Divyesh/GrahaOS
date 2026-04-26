// user/libtls-mg/vendor/mongoose_config.h
//
// Phase 22 closeout (G1): userspace TLS-only Mongoose configuration.
//
// Stage D's original config (kernel-side) has been retired with the
// kernel/net/* delete.  This file replaces it for the userspace
// libtls-mg.a build.  We compile mongoose.c whole but turn OFF every
// subsystem except `MG_TLS == MG_TLS_BUILTIN`.  Mongoose's #ifdef
// gates trim out TCP/IP, sockets, FS, IPv6, mDNS, MQTT, etc., leaving
// just the TLS handshake + record layer + crypto primitives.
//
// All libc + memory functions resolve to GrahaOS libc (linked via
// $(LIBC) in user/Makefile).  Mongoose's malloc/free map directly to
// libc malloc/free (libc has a freelist allocator since Phase 14).

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

// time_t — Mongoose passes timestamps around but we never look at them
// in TLS-only mode.
typedef int64_t time_t;

// errno: Mongoose tracks errors via a global int.  Provide it; nobody
// reads it in our code path.
extern int mg_errno;
#define errno mg_errno

// libc surface.  GrahaOS libc provides all of these.
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern int   memcmp(const void *s1, const void *s2, size_t n);
extern void *memchr(const void *s, int c, size_t n);
extern size_t strlen(const char *str);
extern int   strcmp(const char *s1, const char *s2);
extern int   strncmp(const char *s1, const char *s2, size_t n);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern char *strstr(const char *haystack, const char *needle);
extern char *strcpy(char *dest, const char *src);
extern char *strncpy(char *dest, const char *src, size_t n);
extern char *strcat(char *dest, const char *src);
extern char *strncat(char *dest, const char *src, size_t n);
extern size_t strnlen(const char *s, size_t maxlen);
extern size_t strspn(const char *s, const char *accept);
extern size_t strcspn(const char *s, const char *reject);
extern void *malloc(size_t n);
extern void  free(void *p);
extern void *calloc(size_t n, size_t s);
extern void *realloc(void *p, size_t s);
extern int   atoi(const char *nptr);
extern long  strtol(const char *nptr, char **endptr, int base);
extern unsigned long strtoul(const char *nptr, char **endptr, int base);
extern int   snprintf(char *str, size_t size, const char *format, ...);
extern int   vsnprintf(char *str, size_t size, const char *format, va_list ap);
extern void  abort(void);

// Stubs for libc functions Mongoose references but TLS-only doesn't
// actually need to do anything useful for.
static inline void srand(unsigned int seed) { (void)seed; }
static inline int  rand(void) { return 42; }
static inline void putchar(int c) { (void)c; }
static inline int  sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt; return 0;
}

// Architecture: tells Mongoose to use this config file's defines.
#define MG_ARCH MG_ARCH_CUSTOM

// ----- Subsystem on/off -----
// TLS path is the ONLY thing we want compiled in.
// MG_TLS_BUILTIN = 3 (defined in mongoose.h after this file is included).
// We must use the literal 3 here because mongoose_config.h is included
// BEFORE mongoose.h's MG_TLS_* macros are defined.
#define MG_TLS               3   /* MG_TLS_BUILTIN */

// Disable everything else.  Each gate trims a chunk of mongoose.c.
#define MG_ENABLE_TCPIP      0
#define MG_ENABLE_SOCKET     0
#define MG_ENABLE_LOG        0
#define MG_ENABLE_CUSTOM_LOG 1
#define MG_ENABLE_CUSTOM_MILLIS 1
#define MG_ENABLE_CUSTOM_RANDOM 1
#define MG_ENABLE_FATFS      0
#define MG_ENABLE_PACKED_FS  0
#define MG_ENABLE_POSIX_FS   0
#define MG_ENABLE_DIRLIST    0
#define MG_ENABLE_SSI        0
#define MG_ENABLE_ASSERT     0
#define MG_ENABLE_PROFILE    0
#define MG_ENABLE_EPOLL      0
#define MG_ENABLE_POLL       0
#define MG_ENABLE_IPV6       0
#define MG_ENABLE_MD5        0
#define MG_ENABLE_WINSOCK    0
#define MG_ENABLE_LWIP       0
#define MG_ENABLE_FREERTOS_TCP 0
#define MG_ENABLE_RL         0
#define MG_TLS_BUILTIN_RNG_SEED  0

// IO buffer.
#define MG_IO_SIZE 2048

// Hint for mongoose.c to skip its built-in driver init.
#define MG_ENABLE_TCPIP_DRIVER_INIT 0
