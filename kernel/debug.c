// kernel/debug.c
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/drivers/lapic/lapic.h"
#include "../arch/x86_64/drivers/lapic_timer/lapic_timer.h"
#include "sync/spinlock.h"

// External references to locks from various modules
extern spinlock_t sched_lock;  // From sched.c
extern spinlock_t fb_lock;     // From framebuffer.c
extern spinlock_t pmm_lock;    // From pmm.c
extern spinlock_t vfs_lock;    // From vfs.c
extern spinlock_t ap_startup_lock; // From smp.c

// External references from scheduler
extern uint32_t schedule_count;
extern uint32_t context_switches;
extern int current_task_index;
extern int next_task_id;

// Implementation of debug accessor functions
bool debug_is_sched_lock_held(void) {
    return spinlock_held(&sched_lock);
}

bool debug_is_fb_lock_held(void) {
    return spinlock_held(&fb_lock);
}

bool debug_is_pmm_lock_held(void) {
    return spinlock_held(&pmm_lock);
}

bool debug_is_vfs_lock_held(void) {
    return spinlock_held(&vfs_lock);
}

void debug_print_lock_states(void) {
    framebuffer_draw_string("=== Lock States ===", 400, 100, COLOR_YELLOW, 0x00101828);
    
    uint32_t cpu = smp_get_current_cpu();
    char msg[80];
    
    // Current CPU
    msg[0] = 'C'; msg[1] = 'P'; msg[2] = 'U'; msg[3] = ':'; msg[4] = ' ';
    msg[5] = '0' + cpu;
    msg[6] = '\0';
    framebuffer_draw_string(msg, 400, 120, COLOR_WHITE, 0x00101828);
    
    // Check each lock
    if (sched_lock.locked) {
        framebuffer_draw_string("sched_lock: LOCKED", 400, 140, 
            sched_lock.owner == cpu ? COLOR_YELLOW : COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("sched_lock: free", 400, 140, COLOR_GREEN, 0x00101828);
    }
    
    if (fb_lock.locked) {
        framebuffer_draw_string("fb_lock: LOCKED", 400, 160,
            fb_lock.owner == cpu ? COLOR_YELLOW : COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("fb_lock: free", 400, 160, COLOR_GREEN, 0x00101828);
    }
    
    if (pmm_lock.locked) {
        framebuffer_draw_string("pmm_lock: LOCKED", 400, 180,
            pmm_lock.owner == cpu ? COLOR_YELLOW : COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("pmm_lock: free", 400, 180, COLOR_GREEN, 0x00101828);
    }
    
    if (vfs_lock.locked) {
        framebuffer_draw_string("vfs_lock: LOCKED", 400, 200,
            vfs_lock.owner == cpu ? COLOR_YELLOW : COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("vfs_lock: free", 400, 200, COLOR_GREEN, 0x00101828);
    }
    
    // Scheduler stats
    msg[0] = 'S'; msg[1] = 'c'; msg[2] = 'h'; msg[3] = 'e'; msg[4] = 'd'; msg[5] = ':'; msg[6] = ' ';
    int n = schedule_count;
    int i = 7;
    if (n == 0) {
        msg[i++] = '0';
    } else {
        char digits[10];
        int d = 0;
        while (n > 0) {
            digits[d++] = '0' + (n % 10);
            n /= 10;
        }
        while (d > 0) {
            msg[i++] = digits[--d];
        }
    }
    msg[i] = '\0';
    framebuffer_draw_string(msg, 400, 220, COLOR_WHITE, 0x00101828);
}

// System check function
void debug_check_system(void) {
    framebuffer_draw_string("=== System Check ===", 400, 50, COLOR_CYAN, 0x00101828);
    
    // Check CPU info
    char msg[80];
    msg[0] = 'C'; msg[1] = 'P'; msg[2] = 'U'; msg[3] = 's'; msg[4] = ':'; msg[5] = ' ';
    msg[6] = '0' + g_cpu_count;
    msg[7] = '\0';
    framebuffer_draw_string(msg, 400, 70, COLOR_WHITE, 0x00101828);
    
    // Check LAPIC
    if (lapic_is_enabled()) {
        framebuffer_draw_string("LAPIC: OK", 400, 90, COLOR_GREEN, 0x00101828);
    } else {
        framebuffer_draw_string("LAPIC: FAIL", 400, 90, COLOR_RED, 0x00101828);
    }
    
    // Check timer
    if (lapic_timer_is_running()) {
        framebuffer_draw_string("Timer: OK", 400, 110, COLOR_GREEN, 0x00101828);
    } else {
        framebuffer_draw_string("Timer: FAIL", 400, 110, COLOR_RED, 0x00101828);
    }
    
    // Print lock states
    debug_print_lock_states();
}

// Get return address from stack
static inline uint64_t get_return_address(int level) {
    uint64_t* rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    
    // Walk the stack frames
    for (int i = 0; i < level && rbp; i++) {
        rbp = (uint64_t*)*rbp;
        if ((uint64_t)rbp < 0xFFFF800000000000) {
            return 0;  // Invalid frame pointer
        }
    }
    
    if (rbp && (uint64_t)rbp >= 0xFFFF800000000000) {
        return rbp[1];  // Return address is one word above frame pointer
    }
    return 0;
}

// Simple stack trace
void debug_stack_trace(int max_frames) {
    framebuffer_draw_string("Stack trace:", 600, 100, COLOR_YELLOW, COLOR_BLACK);
    
    for (int i = 0; i < max_frames; i++) {
        uint64_t addr = get_return_address(i);
        if (addr == 0) break;
        
        // Display address
        char msg[32] = "  0x";
        for (int j = 0; j < 16; j++) {
            char hex = "0123456789ABCDEF"[(addr >> (60 - j * 4)) & 0xF];
            msg[4 + j] = hex;
        }
        msg[20] = '\0';
        
        framebuffer_draw_string(msg, 600, 120 + (i * 20), COLOR_CYAN, COLOR_BLACK);
    }
}

// Call this when spinlock error occurs
void debug_spinlock_error(const char *lock_name, uint64_t owner, uint64_t current_cpu) {
    // Save and disable interrupts
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));
    
    // Show error details
    framebuffer_draw_string("=== SPINLOCK ERROR ===", 500, 400, COLOR_RED, COLOR_BLACK);
    
    char msg[64];
    msg[0] = 'L'; msg[1] = 'o'; msg[2] = 'c'; msg[3] = 'k'; msg[4] = ':'; msg[5] = ' ';
    int i = 6;
    while (*lock_name && i < 30) msg[i++] = *lock_name++;
    msg[i] = '\0';
    framebuffer_draw_string(msg, 500, 420, COLOR_WHITE, COLOR_BLACK);
    
    msg[0] = 'O'; msg[1] = 'w'; msg[2] = 'n'; msg[3] = 'e'; msg[4] = 'r'; msg[5] = ':'; msg[6] = ' ';
    msg[7] = '0' + owner;
    msg[8] = '\0';
    framebuffer_draw_string(msg, 500, 440, COLOR_WHITE, COLOR_BLACK);
    
    msg[0] = 'C'; msg[1] = 'P'; msg[2] = 'U'; msg[3] = ':'; msg[4] = ' ';
    msg[5] = '0' + current_cpu;
    msg[6] = '\0';
    framebuffer_draw_string(msg, 500, 460, COLOR_WHITE, COLOR_BLACK);
    
    // Show stack trace
    debug_stack_trace(5);
    
    // Restore interrupts if they were enabled
    if (flags & 0x200) {
        asm volatile("sti");
    }
}