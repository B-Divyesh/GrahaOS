#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "limine.h"
#include "initrd.h"
#include "elf.h"
#include "fs/vfs.h"
#include "gcp.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/syscall/syscall.h"
#include "../arch/x86_64/cpu/interrupts.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/drivers/lapic_timer/lapic_timer.h"
#include "../arch/x86_64/cpu/ports.h"
#include "../arch/x86_64/drivers/keyboard/keyboard.h"

// --- Limine Requests ---
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL
};

#if LIMINE_API_REVISION >= 1
__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = { 
    .id = LIMINE_MP_REQUEST, 
    .revision = 0, 
    .response = NULL 
};
#else
__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = { 
    .id = LIMINE_SMP_REQUEST, 
    .revision = 0, 
    .response = NULL 
};
#endif

// --- Standard C Library Functions ---
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

// --- Utility Functions ---
static void hcf(void) {
    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

// --- Keyboard Polling Task ---
static void keyboard_poll_task(void) {
    while (1) {
        // Check if keyboard has data available
        if (inb(0x64) & 0x01) {
            uint8_t scancode = inb(0x60);
            keyboard_handle_scancode(scancode);
        }
        // Yield to other tasks
        asm volatile("hlt");
    }
}

// --- Main Kernel Entry Point ---
void kmain(void) {
    // Verify Limine base revision support
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }
    
    // Verify all required Limine responses are available
    if (framebuffer_request.response == NULL || 
        memmap_request.response == NULL ||
        executable_address_request.response == NULL || 
        hhdm_request.response == NULL ||
        module_request.response == NULL ||
    #if LIMINE_API_REVISION >= 1
        mp_request.response == NULL) {
    #else
        smp_request.response == NULL) {
    #endif
        hcf();
    }

    // Initialize framebuffer for early output
    if (!framebuffer_init(&framebuffer_request)) {
        hcf();
    }

    framebuffer_clear(0x00101828);

    // Display boot banner
    framebuffer_draw_rect(50, 50, 600, 140, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 596, 136, 0x00004488);
    framebuffer_draw_rect(54, 54, 592, 132, 0x000066AA);
    framebuffer_draw_string("GrahaOS - Phase 7a: SMP Support", 70, 70, COLOR_WHITE, 0x000066AA);
    framebuffer_draw_string("Symmetric Multiprocessing Enabled!", 70, 90, COLOR_LIGHT_GRAY, 0x000066AA);
    framebuffer_draw_rect_outline(40, 40, 620, 160, COLOR_WHITE);
    framebuffer_draw_rect_outline(42, 42, 616, 156, COLOR_LIGHT_GRAY);

    int y_pos = 220;

    // Extract kernel and memory layout information
    uint64_t kernel_phys_base = executable_address_request.response->physical_base;
    uint64_t kernel_virt_base = executable_address_request.response->virtual_base;
    uint64_t hhdm_offset = hhdm_request.response->offset;

    // --- CRITICAL: CORRECT INITIALIZATION ORDER ---
    
    // 1. Initialize Physical Memory Manager
    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 2. Initialize Virtual Memory Manager and enable paging
    vmm_init(memmap_request.response, framebuffer_request.response,
             kernel_phys_base, kernel_virt_base, hhdm_offset);
    framebuffer_init(&framebuffer_request);  // Reinitialize after paging
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 3. CRITICAL: Initialize SMP FIRST (sets up GDT/TSS for BSP and starts APs)
    // This also initializes LAPIC and LAPIC timer for all CPUs
    #if LIMINE_API_REVISION >= 1
        smp_init(&mp_request);
    #else
        smp_init(&smp_request);
    #endif
    framebuffer_draw_string("SMP Initialized - All CPUs online!", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 4. NOW we can initialize IDT (after GDT is set up)
    idt_init();
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 5. Initialize scheduler (after per-CPU structures exist)
    sched_init();
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 6. Initialize syscall interface (after per-CPU structures exist)
    syscall_init();
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 7. Initialize Virtual File System
    vfs_init();
    framebuffer_draw_string("VFS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    // --- USER SPACE INITIALIZATION ---
    framebuffer_draw_string("=== Loading Interactive Shell ===", 50, y_pos, COLOR_WHITE, 0x00101828);
    y_pos += 30;

    // Initialize Initial RAM Disk
    initrd_init(&module_request);
    framebuffer_draw_string("Initrd initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Locate shell binary in initrd
    size_t gash_size;
    void *gash_data = initrd_lookup("bin/gash", &gash_size);
    if (!gash_data) {
        framebuffer_draw_string("FATAL: Could not find bin/gash in initrd!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("Found bin/gash in initrd.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Load shell ELF binary
    uint64_t entry_point, cr3;
    if (!elf_load(gash_data, &entry_point, &cr3)) {
        framebuffer_draw_string("FATAL: Failed to load shell ELF file!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("Shell loaded successfully into memory.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Create shell process
    int process_id = sched_create_user_process(entry_point, cr3);
    if (process_id < 0) {
        framebuffer_draw_string("FATAL: Failed to create shell process!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("Shell process created.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Create keyboard polling task
    int kbd_task = sched_create_task(keyboard_poll_task);
    if (kbd_task >= 0) {
        framebuffer_draw_string("Keyboard polling task created.", 50, y_pos, COLOR_GREEN, 0x00101828);
    } else {
        framebuffer_draw_string("WARNING: Failed to create keyboard task!", 50, y_pos, COLOR_YELLOW, 0x00101828);
    }
    y_pos += 30;

    // Brief pause to allow user to read initialization messages
    for (volatile int i = 0; i < 500000; i++) {
        asm volatile("pause");
    }

    // Clear screen before transferring control to shell
    framebuffer_clear(0x00101828);
    
    // --- FINAL SYSTEM ACTIVATION ---
    
    // Note: LAPIC timer is already initialized by BSP in smp_init()
    // and by each AP in ap_main()
    framebuffer_draw_string("LAPIC Timers active on all CPUs", 10, 10, COLOR_GREEN, 0x00101828);
    
    // Enable hardware interrupts
    asm volatile("sti");
    framebuffer_draw_string("Interrupts enabled - System ready!", 10, 30, COLOR_GREEN, 0x00101828);
    
    // Enter idle loop - scheduler will handle process switching
    while (1) {
        asm volatile("hlt");  // Halt until interrupt
    }
}