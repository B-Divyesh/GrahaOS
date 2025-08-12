// kernel/sync/spinlock.c - Enhanced with debugging
#include "spinlock.h"
#include "../../drivers/video/framebuffer.h"
#include "../../arch/x86_64/cpu/smp.h"
#include <stdarg.h>

// Forward declaration
void framebuffer_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

// Debug counter for spinlock issues
static volatile uint32_t spinlock_errors = 0;

// Simple string formatting for panic messages
static void panic_print(const char *msg) {
    framebuffer_draw_string(msg, 10, 10, COLOR_WHITE, COLOR_RED);
}

void kernel_panic(const char *fmt, ...) {
    // Disable interrupts
    asm volatile("cli");
    
    // Clear screen with red
    framebuffer_clear(COLOR_RED);
    
    panic_print("KERNEL PANIC: ");
    panic_print(fmt);
    
    // Show which CPU panicked
    uint64_t cpu_id = get_cpu_id();
    char cpu_msg[32] = "CPU: ";
    cpu_msg[5] = '0' + (cpu_id & 0xF);
    cpu_msg[6] = '\0';
    framebuffer_draw_string(cpu_msg, 10, 30, COLOR_WHITE, COLOR_RED);
    
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
    lock->interrupt_state = 0;
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
    
    // Save interrupt state and disable interrupts
    uint64_t flags;
    asm volatile(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
    );
    
    // Try to acquire the lock with timeout
    int attempts = 10000000;  // Large timeout
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        if (--attempts <= 0) {
            // Timeout - possible deadlock
            char msg[128] = "DEADLOCK: Lock ";
            int i = 15;
            const char *n = lock->name;
            while (*n && i < 100) msg[i++] = *n++;
            msg[i++] = ' '; msg[i++] = 'C'; msg[i++] = 'P'; msg[i++] = 'U'; msg[i++] = ':';
            msg[i++] = '0' + cpu_id;
            msg[i] = '\0';
            
            // Show error but don't panic
            framebuffer_draw_string(msg, 10, 500 + (spinlock_errors * 20), COLOR_RED, COLOR_BLACK);
            spinlock_errors++;
            
            // Restore interrupts and fail
            if (flags & 0x200) {
                asm volatile("sti");
            }
            return;  // Failed to acquire
        }
        
        // Brief pause while spinning
        asm volatile("pause");
    }
    
    // We got the lock
    lock->interrupt_state = flags;
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
    
    // CRITICAL: More detailed check for ownership
    if (!lock->locked) {
        // Lock not held at all!
        char msg[128] = "SPINLOCK ERROR: Releasing unheld lock: ";
        int i = 39;
        const char *n = lock->name;
        while (*n && i < 100) msg[i++] = *n++;
        msg[i] = '\0';
        
        framebuffer_draw_string(msg, 10, 520 + (spinlock_errors * 20), COLOR_RED, COLOR_BLACK);
        spinlock_errors++;
        return;  // Don't crash, just log
    }
    
    if (lock->owner != cpu_id) {
        // Different CPU owns it!
        char msg[128] = "SPINLOCK ERROR: CPU ";
        msg[20] = '0' + cpu_id;
        msg[21] = ' '; msg[22] = 'r'; msg[23] = 'e'; msg[24] = 'l'; 
        msg[25] = ' '; 
        int i = 26;
        const char *n = lock->name;
        while (*n && i < 60) msg[i++] = *n++;
        msg[i++] = ' '; msg[i++] = 'o'; msg[i++] = 'w'; msg[i++] = 'n'; msg[i++] = ':';
        msg[i++] = '0' + lock->owner;
        msg[i] = '\0';
        
        framebuffer_draw_string(msg, 10, 540 + (spinlock_errors * 20), COLOR_RED, COLOR_BLACK);
        spinlock_errors++;
        
        // DON'T panic in production - just log and continue
        #ifdef DEBUG_LOCKS
        kernel_panic("spinlock_release: Trying to release unowned lock");
        #else
        return;  // Don't release it
        #endif
    }
    
    // Handle recursive release
    if (--lock->count > 0) {
        return;
    }
    
    // Get saved interrupt state
    uint64_t flags = lock->interrupt_state;
    
    // Release the lock
    lock->owner = (uint64_t)-1;
    lock->interrupt_state = 0;
    
    // CRITICAL: Memory barrier before releasing
    asm volatile("mfence" ::: "memory");
    
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    
    // Restore interrupt state
    if (flags & 0x200) {
        asm volatile("sti");
    }
}

bool spinlock_held(spinlock_t *lock) {
    if (!lock) return false;
    return lock->locked && lock->owner == get_cpu_id();
}

// Implementation of get_cpu_id
uint64_t get_cpu_id(void) {
    return (uint64_t)smp_get_current_cpu();
}

// Debug function to check all locks
void spinlock_check_all(void) {
    extern spinlock_t sched_lock;
    extern spinlock_t fb_lock;
    extern spinlock_t pmm_lock;
    extern spinlock_t vfs_lock;
    
    uint64_t cpu_id = get_cpu_id();
    
    char msg[64] = "Lock check CPU ";
    msg[15] = '0' + cpu_id;
    msg[16] = ':';
    msg[17] = '\0';
    framebuffer_draw_string(msg, 600, 10, COLOR_YELLOW, COLOR_BLACK);
    
    if (sched_lock.locked) {
        msg[0] = 's'; msg[1] = 'c'; msg[2] = 'h'; msg[3] = ':';
        msg[4] = '0' + sched_lock.owner;
        msg[5] = '\0';
        framebuffer_draw_string(msg, 600, 30, COLOR_CYAN, COLOR_BLACK);
    }
}