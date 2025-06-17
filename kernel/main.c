#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"     // New include
#include "../arch/x86_64/cpu/syscall/syscall.h" // New include

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

    // Draw a welcome banner - Updated for Phase 4a
    framebuffer_draw_rect(50, 50, 600, 140, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 596, 136, 0x00004488); // Inner darker blue
    framebuffer_draw_rect(54, 54, 592, 132, 0x000066AA); // Lighter inner

    // Draw the main title - Updated for Phase 4a
    framebuffer_draw_string("GrahaOS - Phase 4a", 70, 70, COLOR_WHITE, 0x000066AA);
    framebuffer_draw_string("System Calls & Task Structures", 70, 90, COLOR_LIGHT_GRAY, 0x000066AA);

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

    // --- NEW: Initialize Phase 4a components ---

    // Initialize the scheduler
    sched_init();
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Initialize the syscall interface
    syscall_init();
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    framebuffer_draw_string("Kernel running in virtual mode", 50, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    // Show post-VMM memory statistics
    uint64_t free_after_vmm_mb = pmm_get_free_memory() / (1024 * 1024);
    char free_after_str[32];
    uint_to_string(free_after_vmm_mb, free_after_str);

    framebuffer_draw_string("Free Memory (post-VMM): ", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(free_after_str, 220, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(" MB", 300, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 40;

    // Test the interrupt system again, this time while paging is active.
    framebuffer_draw_string("Triggering interrupt 0x20 (with paging)...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    // Enable interrupts and trigger a test interrupt
    asm volatile ("sti");           // Enable interrupts
    asm volatile ("int $0x20");     // Trigger interrupt 32 (0x20)

    // The interrupt handler should print its message, and execution will continue here.
    // If we reach this point, the interrupt system is working correctly with paging.
    framebuffer_draw_string("Interrupt system working with paging!", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    // --- NEW: Test the syscall system ---
    framebuffer_draw_string("Testing syscall 0 (SYS_TEST)...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    uint64_t syscall_ret = do_syscall(SYS_TEST);

    // This message will appear if the syscall returns correctly
    // The handler itself will also print a message
    framebuffer_draw_string("Syscall returned successfully.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Print the return value
    char ret_str[32];
    uint_to_string(syscall_ret, ret_str);
    framebuffer_draw_string("Return value: ", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(ret_str, 170, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 40;

    // Final success message
    framebuffer_draw_string("Phase 4a Complete: Syscalls Active!", 50, y_pos, COLOR_WHITE, 0x00101828);
    framebuffer_draw_string("Task structures and syscall interface ready.", 50, y_pos + 20, COLOR_LIGHT_GRAY, 0x00101828);

    // We're done, just hang...
    hcf();
}
