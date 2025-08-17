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
#include "keyboard_task.h"
#include "../arch/x86_64/drivers/lapic/lapic.h"
#include "../arch/x86_64/drivers/ahci/ahci.h"
#include "fs/grahafs.h"

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
// External keyboard task function from keyboard_task.c
extern void keyboard_poll_task(void);
extern void (*get_keyboard_poll_task(void))(void);

// --- Test function for GrahaFS ---
static void test_grahafs(void) {
    // Initialize the GrahaFS driver
    grahafs_init();
    
    // Get the block device we registered earlier
    block_device_t* hdd = vfs_get_block_device(0);
    if (!hdd) {
        framebuffer_draw_string("GFS TEST: Could not get block device 0.", 10, 570, COLOR_RED, 0x00101828);
        return;
    }
    framebuffer_draw_string("GFS TEST: Got block device 0.", 10, 570, COLOR_GREEN, 0x00101828);

    // Mount the filesystem
    vfs_node_t* root = grahafs_mount(hdd);
    if (!root) {
        framebuffer_draw_string("GFS TEST: Mount failed!", 10, 590, COLOR_RED, 0x00101828);
        return;
    }
    framebuffer_draw_string("GFS TEST: Mount successful!", 10, 590, COLOR_GREEN, 0x00101828);
    
    // Debug: Show root inode info
    char info_msg[80] = "GFS TEST: Root node - inode: ";
    int pos = 30;
    uint32_t inode_num = root->inode;
    if (inode_num == 0) {
        info_msg[pos++] = '0';
    } else {
        char digits[10];
        int digit_count = 0;
        while (inode_num > 0) {
            digits[digit_count++] = '0' + (inode_num % 10);
            inode_num /= 10;
        }
        while (digit_count > 0) {
            info_msg[pos++] = digits[--digit_count];
        }
    }
    info_msg[pos++] = ',';
    info_msg[pos++] = ' ';
    info_msg[pos++] = 's';
    info_msg[pos++] = 'i';
    info_msg[pos++] = 'z';
    info_msg[pos++] = 'e';
    info_msg[pos++] = ':';
    info_msg[pos++] = ' ';
    uint64_t size = root->size;
    if (size == 0) {
        info_msg[pos++] = '0';
    } else {
        char size_digits[20];
        int size_count = 0;
        while (size > 0) {
            size_digits[size_count++] = '0' + (size % 10);
            size /= 10;
        }
        while (size_count > 0) {
            info_msg[pos++] = size_digits[--size_count];
        }
    }
    info_msg[pos] = '\0';
    framebuffer_draw_string(info_msg, 210, 610, COLOR_CYAN, 0x00101828);
    
    // Test directory listing
    framebuffer_draw_string("GFS TEST: Listing root directory...", 10, 630, COLOR_YELLOW, 0x00101828);
    
    int entries_found = 0;
    for (uint32_t i = 0; i < 10; i++) {  // Try up to 10 entries
        vfs_node_t* entry = root->readdir(root, i);
        if (!entry) {
            if (i == 0) {
                framebuffer_draw_string("  No entries found!", 10, 650, COLOR_RED, 0x00101828);
            }
            break;
        }
        
        entries_found++;
        
        char msg[80] = "  [";
        int j = 3;
        
        // Add index
        if (i < 10) {
            msg[j++] = '0' + i;
        } else {
            msg[j++] = '1';
            msg[j++] = '0' + (i - 10);
        }
        msg[j++] = ']';
        msg[j++] = ' ';
        
        // Add name
        const char* name = entry->name;
        while (*name && j < 30) {
            msg[j++] = *name++;
        }
        
        // Add type
        msg[j++] = ' ';
        msg[j++] = '(';
        if (entry->type == VFS_DIRECTORY) {
            msg[j++] = 'd';
            msg[j++] = 'i';
            msg[j++] = 'r';
        } else {
            msg[j++] = 'f';
            msg[j++] = 'i';
            msg[j++] = 'l';
            msg[j++] = 'e';
        }
        msg[j++] = ',';
        msg[j++] = ' ';
        msg[j++] = 'i';
        msg[j++] = 'n';
        msg[j++] = 'o';
        msg[j++] = 'd';
        msg[j++] = 'e';
        msg[j++] = ':';
        msg[j++] = ' ';
        
        // Add inode number
        uint32_t entry_inode = entry->inode;
        if (entry_inode == 0) {
            msg[j++] = '0';
        } else {
            char inode_digits[10];
            int inode_count = 0;
            while (entry_inode > 0) {
                inode_digits[inode_count++] = '0' + (entry_inode % 10);
                entry_inode /= 10;
            }
            while (inode_count > 0) {
                msg[j++] = inode_digits[--inode_count];
            }
        }
        msg[j++] = ')';
        msg[j] = '\0';
        
        framebuffer_draw_string(msg, 10, 670 + (i * 20), COLOR_CYAN, 0x00101828);
        
        vfs_destroy_node(entry);
    }
    
    if (entries_found > 0) {
        char summary[64] = "GFS TEST: Found ";
        int s = 16;
        if (entries_found < 10) {
            summary[s++] = '0' + entries_found;
        } else {
            summary[s++] = '1';
            summary[s++] = '0' + (entries_found - 10);
        }
        summary[s++] = ' ';
        summary[s++] = 'e';
        summary[s++] = 'n';
        summary[s++] = 't';
        summary[s++] = 'r';
        summary[s++] = 'i';
        summary[s++] = 'e';
        summary[s++] = 's';
        summary[s] = '\0';
        framebuffer_draw_string(summary, 10, 690 + (entries_found * 20) + 20, COLOR_GREEN, 0x00101828);
    }
    
    framebuffer_draw_string("GFS TEST: Complete!", 10, 710, COLOR_GREEN, 0x00101828);
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

    //adding AHCI
    ahci_init();
    y_pos += 20;

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

    // Initialize keyboard hardware BEFORE creating the polling task
    keyboard_init();
    framebuffer_draw_string("Keyboard hardware initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Create keyboard polling task with extensive validation - FIXED VERSION
    framebuffer_draw_string("Creating keyboard polling task...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;
    
    // CRITICAL: Use volatile to prevent optimization
    volatile void (*kbd_func)(void) = keyboard_poll_task;
    
    // Double-check the pointer
    if (!kbd_func) {
        framebuffer_draw_string("ERROR: keyboard_poll_task is NULL!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    
    // Validate address
    volatile uint64_t func_addr = (uint64_t)kbd_func;
    if (func_addr < 0xFFFFFFFF80000000) {
        framebuffer_draw_string("ERROR: Invalid keyboard task address!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    
    // Show the address for debugging
    char addr_msg[64] = "Task addr: 0x";
    for (int i = 0; i < 16; i++) {
        char hex = "0123456789ABCDEF"[(func_addr >> (60 - i * 4)) & 0xF];
        addr_msg[13 + i] = hex;
    }
    addr_msg[29] = '\0';
    framebuffer_draw_string(addr_msg, 50, y_pos, COLOR_CYAN, 0x00101828);
    y_pos += 20;
    
    // CRITICAL: Force the function pointer through memory to avoid register issues
    void (*task_func)(void) = (void (*)(void))func_addr;
    
    // Create the task with explicit type casting
    int kbd_task_id = sched_create_task(task_func);
    
    if (kbd_task_id < 0) {
        framebuffer_draw_string("ERROR: Failed to create keyboard task!", 50, y_pos, COLOR_RED, 0x00101828);
        y_pos += 20;
    } else {
        framebuffer_draw_string("Keyboard task created successfully", 50, y_pos, COLOR_GREEN, 0x00101828);
        y_pos += 20;
        
        char id_msg[32] = "Task ID: ";
        id_msg[9] = '0' + kbd_task_id;
        id_msg[10] = '\0';
        framebuffer_draw_string(id_msg, 50, y_pos, COLOR_CYAN, 0x00101828);
        y_pos += 20;
    }

    // Wait for all CPUs to stabilize
    framebuffer_draw_string("Waiting for all CPUs to stabilize...", 50, y_pos, COLOR_YELLOW, 0x00101828);
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    framebuffer_draw_string("System ready to start.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 30;

    // Brief pause to read messages
    for (volatile int i = 0; i < 500000; i++) {
        asm volatile("pause");
    }

    // Clear screen
    framebuffer_clear(0x00101828);
    
    // Clear screen
    framebuffer_clear(0x00101828);
    
    // --- FINAL SYSTEM ACTIVATION ---
    
    framebuffer_draw_string("System initialization complete", 10, 10, COLOR_GREEN, 0x00101828);
    
    // CRITICAL: Ensure all CPUs are ready before starting ANY timers
    framebuffer_draw_string("Synchronizing all CPUs...", 10, 30, COLOR_YELLOW, 0x00101828);
    
    // Wait for all APs to be ready
    while (aps_started < g_cpu_count - 1) {
        asm volatile("pause");
    }
    
    framebuffer_draw_string("All CPUs synchronized", 10, 30, COLOR_GREEN, 0x00101828);
    
    // Long delay to ensure everything is stable
    for (volatile int i = 0; i < 2000000; i++) {
        asm volatile("pause");
    }
    
    // Enable interrupts FIRST (but no timers running yet)
    framebuffer_draw_string("Enabling interrupts...", 10, 50, COLOR_YELLOW, 0x00101828);
    asm volatile("sti");
    
    // Wait to ensure no pending issues
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    
    // NOW start the timer on BSP only
    framebuffer_draw_string("Starting scheduler timer on BSP...", 10, 70, COLOR_YELLOW, 0x00101828);
    
    // Disable interrupts briefly while starting timer
    asm volatile("cli");
    lapic_timer_init(100, 32);
    asm volatile("sti");
    
    if (!lapic_timer_is_running()) {
        framebuffer_draw_string("ERROR: Timer failed to start!", 10, 90, COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("System running!", 10, 90, COLOR_GREEN, 0x00101828);
    }
    
    // NEW: Run our filesystem test
    test_grahafs();
    // OPTIONAL: Start timers on APs later (commented out for now)
    // This would require IPI (Inter-Processor Interrupts) to signal APs
    // For now, only BSP handles scheduling
    
    // Main idle loop
    uint64_t loop_count = 0;
    while (1) {
        // Periodically check system health
        if ((loop_count++ & 0xFFFFF) == 0) {
            // Check stack
            uint64_t rsp;
            asm volatile("mov %%rsp, %0" : "=r"(rsp));
            if (rsp < 0xFFFF800000000000) {
                framebuffer_draw_string("FATAL: Stack corrupted!", 10, 200, COLOR_RED, 0x00101828);
                asm volatile("cli; hlt");
            }
        }
        
        asm volatile("hlt");
    }

}