#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/syscall/syscall.h"
#include "../arch/x86_64/drivers/timer/pit.h" // New include
#include "../arch/x86_64/cpu/interrupts.h" // For irq_init

// --- Centralized Limine Requests ---
// This is the SINGLE source of truth for all requests.

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

// The Limine requests start and end markers
__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// Request framebuffer
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

// Request memory map for physical memory manager
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

// Request executable address information (for kernel base addresses)
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};

// Request higher half direct map - Provides virtual memory layout info
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }
    return 0;
}

// Simple integer to string conversion for displaying memory stats
static void uint_to_string(uint64_t value, char *buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    char temp[21]; // Enough for 64-bit number
    int i = 0;

    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }

    // Reverse the string
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}

// Convert hex value to string (for addresses)
static void hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17]; // 16 hex digits + null terminator
    int i = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0) {
        temp[i++] = hex_chars[value & 0xF];
        value >>= 4;
    }

    // Reverse the string
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}

// A wrapper function to make a syscall from C
static inline uint64_t do_syscall(uint64_t syscall_num) {
    uint64_t ret;
    asm volatile (
        "mov %1, %%rax\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(syscall_num)
        : "rax", "rcx", "r11" // Clobbered registers
    );
    return ret;
}

// Halt and catch fire function
static void hcf(void) {
    asm ("cli");  // Disable interrupts first
    for (;;) {
        asm ("hlt");
    }
}

// A simple second task that will run concurrently.
void other_task_main(void) {
    uint32_t color = COLOR_YELLOW;
    while (1) {
        // --- FIX: Use the correct 4-argument function call ---
        // First, draw a black space to clear the old character.
        framebuffer_draw_char(' ', 750, 10, COLOR_BLACK);
        // Then, draw the new character.
        framebuffer_draw_char('@', 750, 10, color);

        // Simple delay loop
        for (volatile int i = 0; i < 10000000; i++);
        color = (color == COLOR_YELLOW) ? COLOR_CYAN : COLOR_YELLOW;
    }
}

void kmain(void) {
    // Check if the bootloader supports our base revision
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // Check for critical responses BEFORE using them.
    if (framebuffer_request.response == NULL ||
        memmap_request.response == NULL ||
        executable_address_request.response == NULL ||
        hhdm_request.response == NULL) {
        hcf();
    }

    // Initialize the framebuffer driver
    if (!framebuffer_init(&framebuffer_request)) {
        hcf();
    }

    // Clear the screen to a dark blue-gray
    framebuffer_clear(0x00101828);

    // Draw a welcome banner - Updated for Phase 4b
    framebuffer_draw_rect(50, 50, 600, 140, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 596, 136, 0x00004488); // Inner darker blue
    framebuffer_draw_rect(54, 54, 592, 132, 0x000066AA); // Lighter inner

    // Draw the main title - Updated for Phase 4b
    framebuffer_draw_string("GrahaOS - Phase 4b", 70, 70, COLOR_WHITE, 0x000066AA);
    framebuffer_draw_string("Multitasking & First Context Switch", 70, 90, COLOR_LIGHT_GRAY, 0x000066AA);

    // Draw decorative elements
    framebuffer_draw_rect_outline(40, 40, 620, 160, COLOR_WHITE);
    framebuffer_draw_rect_outline(42, 42, 616, 156, COLOR_LIGHT_GRAY);

    int y_pos = 220;

    // Initialize GDT
    gdt_init();
    framebuffer_draw_string("GDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Initialize IDT
    idt_init();
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Initialize Physical Memory Manager
    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Display memory statistics (before VMM initialization)
    uint64_t total_mb = pmm_get_total_memory() / (1024 * 1024);
    uint64_t free_mb = pmm_get_free_memory() / (1024 * 1024);

    char total_str[32];
    char free_str[32];
    uint_to_string(total_mb, total_str);
    uint_to_string(free_mb, free_str);

    framebuffer_draw_string("Total Memory: ", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(total_str, 170, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(" MB", 250, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    framebuffer_draw_string("Free Memory:  ", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(free_str, 170, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(" MB", 250, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 40;

    // --- Initialize Virtual Memory Manager ---
    framebuffer_draw_string("VMM: Initializing and enabling paging...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    // Get kernel addresses using the proper Limine requests
    uint64_t kernel_phys_base = executable_address_request.response->physical_base;
    uint64_t kernel_virt_base = executable_address_request.response->virtual_base;
    uint64_t hhdm_offset = hhdm_request.response->offset;

    // Display the addresses we're using
    char virt_str[32], phys_str[32], hhdm_str[32];
    hex_to_string(kernel_virt_base, virt_str);
    hex_to_string(kernel_phys_base, phys_str);
    hex_to_string(hhdm_offset, hhdm_str);

    framebuffer_draw_string("Kernel Virtual Base: 0x", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(virt_str, 220, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    framebuffer_draw_string("Kernel Physical Base: 0x", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(phys_str, 230, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    framebuffer_draw_string("Higher-half direct map: 0x", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(hhdm_str, 250, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 60;

    // --- CORRECTED VMM INITIALIZATION ---
    // Pass the hhdm_offset from Limine to the VMM.
    vmm_init(
        memmap_request.response,
        framebuffer_request.response,
        kernel_phys_base,
        kernel_virt_base,
        hhdm_offset // <-- The crucial new parameter
    );

    // CRITICAL: Re-initialize framebuffer with new virtual address.
    framebuffer_init(&framebuffer_request);

    // If we get here, it means paging was enabled successfully and the kernel
    // continued executing from its virtual address without crashing.
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // --- NEW: Initialize Phase 4b components ---

    // Initialize the scheduler
    sched_init();
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Create our second task.
    if (sched_create_task(other_task_main) != -1) {
        framebuffer_draw_string("Second task created.", 50, y_pos, COLOR_GREEN, 0x00101828);
    } else {
        framebuffer_draw_string("Failed to create second task.", 50, y_pos, COLOR_RED, 0x00101828);
    }
    y_pos += 20;

    // Initialize the syscall interface
    syscall_init();
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // --- NEW: Initialize Timer and IRQs ---
    pit_init(100); // Initialize PIT to 100 Hz
    framebuffer_draw_string("PIT Initialized to 100 Hz.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    irq_init(); // Remap PIC and enable interrupts
    // Add a small delay to ensure PIT is working
    for (volatile int i = 0; i < 1000000; i++);
    framebuffer_draw_string("IRQs Initialized and Enabled.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    framebuffer_draw_string("Phase 4b Complete: Multitasking Active!", 50, y_pos, COLOR_WHITE, 0x00101828);
    framebuffer_draw_string("Watch for blinking characters!", 50, y_pos + 20, COLOR_LIGHT_GRAY, 0x00101828);

    // The main kernel task now just prints a character in a loop.
    uint32_t color = COLOR_WHITE;
    while (1) {
        // --- FIX: Use the correct 4-argument function call ---
        // First, draw a black space to clear the old character.
        framebuffer_draw_char(' ', 10, 10, COLOR_BLACK);
        // Then, draw the new character.
        framebuffer_draw_char('#', 10, 10, color);

        for (volatile int i = 0; i < 10000000; i++);
        color = (color == COLOR_WHITE) ? COLOR_LIGHT_GRAY : COLOR_WHITE;
    }
}
