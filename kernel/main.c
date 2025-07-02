#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "limine.h"
#include "initrd.h"
#include "elf.h"
#include "fs/vfs.h"  // <-- ADDED
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/syscall/syscall.h"
#include "../arch/x86_64/drivers/timer/pit.h"
#include "../arch/x86_64/cpu/interrupts.h"

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

// Limine module request for the initrd
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
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
        hhdm_request.response == NULL ||
        module_request.response == NULL) {
        hcf();
    }

    // Initialize the framebuffer driver
    if (!framebuffer_init(&framebuffer_request)) {
        hcf();
    }

    // Clear the screen to a dark blue-gray
    framebuffer_clear(0x00101828);

    // UPDATED: Banner for Phase 6a
    framebuffer_draw_rect(50, 50, 600, 140, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 596, 136, 0x00004488); // Inner darker blue
    framebuffer_draw_rect(54, 54, 592, 132, 0x000066AA); // Lighter inner

    // Draw the main title - Updated for Phase 6a
    framebuffer_draw_string("GrahaOS - Phase 6a: Filesystem Syscalls", 70, 70, COLOR_WHITE, 0x000066AA);
    framebuffer_draw_string("Exposing initrd to user-space programs", 70, 90, COLOR_LIGHT_GRAY, 0x000066AA);

    // Draw decorative elements
    framebuffer_draw_rect_outline(40, 40, 620, 160, COLOR_WHITE);
    framebuffer_draw_rect_outline(42, 42, 616, 156, COLOR_LIGHT_GRAY);

    int y_pos = 220;

    // --- INITIALIZATION SEQUENCE ---
    
    // 1. Get critical memory information from Limine FIRST.
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
    y_pos += 40;

    // 2. Initialize Physical Memory Manager (PMM).
    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Display memory statistics (after PMM initialization)
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

    // 3. Initialize Virtual Memory Manager (VMM).
    framebuffer_draw_string("VMM: Initializing and enabling paging...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    vmm_init(memmap_request.response, framebuffer_request.response, kernel_phys_base, kernel_virt_base, hhdm_offset);
    framebuffer_init(&framebuffer_request); // Re-init with virtual addresses
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    // 4. Initialize GDT and TSS.
    gdt_init();
    framebuffer_draw_string("GDT & TSS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 5. Initialize IDT, Syscalls, and Scheduler.
    idt_init();
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    sched_init();
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    syscall_init();
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // --- NEW: Initialize VFS ---
    vfs_init();
    framebuffer_draw_string("VFS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    // --- PHASE 6a: ELF Loading and Execution ---
    framebuffer_draw_string("=== Phase 6a: Loading Test Program ===", 50, y_pos, COLOR_WHITE, 0x00101828);
    y_pos += 30;

    // Initialize the initrd subsystem
    initrd_init(&module_request);
    framebuffer_draw_string("Initrd initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Look for the grahai executable in the initrd
    size_t grahai_size;
    void *grahai_data = initrd_lookup("bin/grahai", &grahai_size);
    if (!grahai_data) {
        framebuffer_draw_string("FATAL: Could not find bin/grahai in initrd!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("Found bin/grahai in initrd.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Display file size
    char size_str[32];
    uint_to_string(grahai_size, size_str);
    framebuffer_draw_string("File size: ", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(size_str, 130, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(" bytes", 200, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    // Load the ELF file into memory and get entry point + CR3
    uint64_t entry_point, cr3;
    if (!elf_load(grahai_data, &entry_point, &cr3)) {
        framebuffer_draw_string("FATAL: Failed to load ELF file!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    
    framebuffer_draw_string("ELF loaded successfully into memory.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // Display the loaded ELF entry point and CR3
    char entry_str[17], cr3_str[17];
    hex_to_string(entry_point, entry_str);
    hex_to_string(cr3, cr3_str);
    framebuffer_draw_string("Entry Point: 0x", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(entry_str, 170, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;

    framebuffer_draw_string("Process CR3: 0x", 50, y_pos, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(cr3_str, 170, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 30;

    // Create a user process with the loaded ELF
    int process_id = sched_create_user_process(entry_point, cr3);
    if (process_id == -1) {
        framebuffer_draw_string("FATAL: Failed to create user process!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }

    char pid_str[16];
    uint_to_string(process_id, pid_str);
    framebuffer_draw_string("User process created with ID: ", 50, y_pos, COLOR_GREEN, 0x00101828);
    framebuffer_draw_string(pid_str, 280, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 30;

    framebuffer_draw_string("=== Starting Process Execution ===", 50, y_pos, COLOR_WHITE, 0x00101828);
    y_pos += 20;

    // Start the timer and scheduler to begin execution
    pit_init(100); // 100 Hz timer
    framebuffer_draw_string("PIT Initialized to 100 Hz.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    irq_init(); // Remap PIC and enable interrupts
    framebuffer_draw_string("IRQs Initialized and Enabled.", 50, y_pos, COLOR_GREEN, 0x00101828);
    
    // Add a visual separator in the right half to indicate where user output appears.
    framebuffer_draw_rect(400, 220, 2, framebuffer_get_height() - 240, COLOR_GRAHA_BLUE);
    framebuffer_draw_string("User Output ->", 410, 220, COLOR_YELLOW, 0x00101828);

    framebuffer_draw_string("Phase 6a: Filesystem syscalls ready!", 410, 200, COLOR_YELLOW, 0x00101828);
    framebuffer_draw_string("User program will test file I/O...", 410, 180, COLOR_WHITE, 0x00101828);

    framebuffer_draw_string("Kernel initialization complete.", 410, 260, COLOR_GREEN, 0x00101828);
    framebuffer_draw_string("Waiting for timer interrupts to start scheduling...", 410, 280, COLOR_YELLOW, 0x00101828);
    
    // Simple approach: Just wait for timer interrupts to trigger the scheduler
    while (1) {
        asm ("hlt");
    }
}