// kernel/sync/spinlock.h
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


// Forward declaration to avoid circular dependency
uint32_t lapic_get_id(void);

// Recursive spinlock structure
typedef struct spinlock {
    volatile uint64_t owner;      // CPU core ID that owns the lock
    volatile uint32_t count;      // Recursion count
    volatile bool locked;         // Lock state
    const char *name;            // Lock name for debugging
    const char *last_file;       // File where lock was last acquired
    int last_line;               // Line where lock was last acquired
} spinlock_t;

// Static initializer for spinlocks
#define SPINLOCK_INITIALIZER(lockname) { \
    .owner = (uint64_t)-1, \
    .count = 0, \
    .locked = false, \
    .name = lockname, \
    .last_file = NULL, \
    .last_line = 0 \
}

// Debug macros
#ifdef DEBUG_LOCKS
#define spinlock_acquire(lock) _spinlock_acquire(lock, __FILE__, __LINE__)
#define spinlock_release(lock) _spinlock_release(lock, __FILE__, __LINE__)
#else
#define spinlock_acquire(lock) _spinlock_acquire(lock, NULL, 0)
#define spinlock_release(lock) _spinlock_release(lock, NULL, 0)
#endif

// Function prototypes
void spinlock_init(spinlock_t *lock, const char *name);
void _spinlock_acquire(spinlock_t *lock, const char *file, int line);
void _spinlock_release(spinlock_t *lock, const char *file, int line);
bool spinlock_held(spinlock_t *lock);

// Get current CPU ID - implementation moved to spinlock.c to avoid circular dependency
uint64_t get_cpu_id(void);

// Panic function declaration
void kernel_panic(const char *fmt, ...);