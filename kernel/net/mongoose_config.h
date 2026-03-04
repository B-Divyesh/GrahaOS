// kernel/net/mongoose_config.h
// Phase 9b: GrahaOS custom arch configuration for Mongoose
// This file is included by mongoose.h when MG_ARCH == MG_ARCH_CUSTOM (0)
#pragma once

// GCC freestanding headers (available without libc)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

// Types not available in freestanding
typedef int64_t time_t;

// errno stub (no thread-local storage, just a global)
extern int mg_errno;
#define errno mg_errno

// Kernel globals (defined in kernel/main.c)
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern int memcmp(const void *s1, const void *s2, size_t n);
extern size_t strlen(const char *str);

// Kernel string library (klib.c)
#include "klib.h"

// Kernel malloc (kmalloc.c)
#include "kmalloc.h"

// Stubs for libc functions Mongoose references
static inline void srand(unsigned int seed) { (void)seed; }
static inline int rand(void) { return 42; }
static inline void putchar(int c) { (void)c; }
static inline int sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt; return 0;
}

// Note: TLS crypto code (chacha20-poly1305) includes <assert.h> and <string.h>
// Stub headers are provided in kernel/net/ (on the include path via -I./kernel/net)

// Architecture
#define MG_ARCH MG_ARCH_CUSTOM

// Enable built-in TCP/IP stack (no OS sockets)
#define MG_ENABLE_TCPIP 1
#define MG_ENABLE_SOCKET 0

// Logging via serial
#define MG_ENABLE_LOG 1
#define MG_ENABLE_CUSTOM_LOG 1

// Custom implementations
#define MG_ENABLE_CUSTOM_MILLIS 1
#define MG_ENABLE_CUSTOM_RANDOM 1

// TLS 1.3 via Mongoose's built-in implementation (no external crypto library)
#define MG_TLS MG_TLS_BUILTIN

// Disable features not needed / that require filesystem
#define MG_ENABLE_FATFS 0
#define MG_ENABLE_PACKED_FS 0
#define MG_ENABLE_POSIX_FS 0
#define MG_ENABLE_DIRLIST 0
#define MG_ENABLE_SSI 0
#define MG_ENABLE_ASSERT 0
#define MG_ENABLE_PROFILE 0
#define MG_ENABLE_EPOLL 0
#define MG_ENABLE_POLL 0
#define MG_ENABLE_IPV6 0
#define MG_ENABLE_MD5 0
#define MG_ENABLE_WINSOCK 0

// Don't auto-init the driver (we do it manually)
#define MG_ENABLE_TCPIP_DRIVER_INIT 0

// IO buffer size matches E1000 buffer size
#define MG_IO_SIZE 2048

// Map standard allocators to kernel allocators
#define malloc(s) kmalloc(s)
#define free(p) kfree(p)
#define calloc(n, s) kcalloc(n, s)
#define realloc(p, s) krealloc(p, s)

// Provide mg_log_prefix and mg_log implementations (in net.c)
void serial_write(const char *str);
