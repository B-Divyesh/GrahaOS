// kernel/keyboard_task.c
// Safer keyboard polling task

#include <stdint.h>
#include "../arch/x86_64/cpu/ports.h"
#include "../arch/x86_64/drivers/keyboard/keyboard.h"

// Ensure this function is not optimized out and has proper alignment
__attribute__((used, noinline, aligned(16), section(".text")))
void keyboard_poll_task(void) {
    // Validate that we're running in kernel space
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    
    if (rsp < 0xFFFF800000000000) {
        // Stack pointer is invalid - halt
        asm volatile("cli; hlt");
        while(1);
    }
    
    // Long initial delay to ensure system is stable
    for (volatile int i = 0; i < 2000000; i++) {
        asm volatile("pause");
    }
    
    // Main polling loop with defensive programming
    while (1) {
        // Validate stack is still good
        asm volatile("mov %%rsp, %0" : "=r"(rsp));
        if (rsp < 0xFFFF800000000000) {
            // Stack corrupted - halt
            break;
        }
        
        // Check keyboard status port carefully
        uint8_t status;
        
        // Use inline assembly to ensure correct port access
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x64));
        
        // Check if data is available
        if (status & 0x01) {
            // Check for errors
            if (!(status & 0xC0)) {
                // Read scancode
                uint8_t scancode;
                asm volatile("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));
                
                // Only process valid scancodes
                if (scancode != 0xFF && scancode != 0x00 && scancode < 0x80) {
                    keyboard_handle_scancode(scancode);
                }
            } else {
                // Error - read and discard
                uint8_t dummy;
                asm volatile("inb %1, %0" : "=a"(dummy) : "Nd"((uint16_t)0x60));
            }
        }
        
        // Longer delay to reduce CPU usage
        for (volatile int i = 0; i < 10000; i++) {
            asm volatile("pause");
        }
        
        // Yield to scheduler
        asm volatile("hlt");
    }
    
    // Should never get here
    asm volatile("cli; hlt");
    while(1);
}

// Export function pointer - also defensive
__attribute__((used, noinline))
void (*get_keyboard_poll_task(void))(void) {
    // Return the address explicitly
    return &keyboard_poll_task;
}