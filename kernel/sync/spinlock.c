// kernel/sync/spinlock.c
#include "spinlock.h"
#include "../../drivers/video/framebuffer.h"
#include <stdarg.h>

// Forward declaration to avoid circular dependency
void framebuffer_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

// Simple string formatting for panic messages
static void panic_print(const char *msg) {
    // Use framebuffer directly in panic mode
    // This assumes framebuffer is already initialized
    framebuffer_draw_string(msg, 10, 10, COLOR_WHITE, COLOR_RED);
}

void kernel_panic(const char *fmt, ...) {
    // Disable interrupts
    asm volatile("cli");
    
    // Clear screen with red
    framebuffer_clear(COLOR_RED);
    
    // For now, just print the format string
    // A full printf implementation would go here
    panic_print("KERNEL PANIC: ");
    panic_print(fmt);
    
    // Halt forever
    for (;;) {
        asm volatile("hlt");
    }
}

void spinlock_init(spinlock_t *lock, const char *name) {
    if (!lock) {
        kernel_panic("spinlock_init: NULL lock pointer");
    }
    
    lock->owner = (uint64_t)-1;
    lock->count = 0;
    lock->locked = false;
    lock->name = name ? name : "unnamed";
    lock->last_file = NULL;
    lock->last_line = 0;
}

void _spinlock_acquire(spinlock_t *lock, const char *file, int line) {
    if (!lock) {
        kernel_panic("spinlock_acquire: NULL lock pointer");
    }
    
    uint64_t cpu_id = get_cpu_id();
    
    // Check if we already own this lock (recursive acquisition)
    if (lock->locked && lock->owner == cpu_id) {
        lock->count++;
        return;
    }
    
    // Try to acquire the lock
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // CPU pause instruction to reduce power consumption
        asm volatile("pause");
    }
    
    // We got the lock
    lock->owner = cpu_id;
    lock->count = 1;
    
    #ifdef DEBUG_LOCKS
    lock->last_file = file;
    lock->last_line = line;
    #endif
}

void _spinlock_release(spinlock_t *lock, const char *file, int line) {
    if (!lock) {
        kernel_panic("spinlock_release: NULL lock pointer");
    }
    
    uint64_t cpu_id = get_cpu_id();
    
    // Verify we own the lock
    if (!lock->locked || lock->owner != cpu_id) {
        kernel_panic("spinlock_release: Trying to release unowned lock");
    }
    
    // Handle recursive release
    if (--lock->count > 0) {
        return;
    }
    
    // Release the lock
    lock->owner = (uint64_t)-1;
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

bool spinlock_held(spinlock_t *lock) {
    if (!lock) return false;
    return lock->locked && lock->owner == get_cpu_id();
}