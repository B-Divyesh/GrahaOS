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
#include "../arch/x86_64/drivers/e1000/e1000.h"
#include "net/net.h"
#include "net/net_task.h"
#include "fs/grahafs.h"
#include "../arch/x86_64/drivers/serial/serial.h"
#include "capability.h"

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

size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// --- Keyboard Polling Task ---
// External keyboard task function from keyboard_task.c
extern void keyboard_poll_task(void);
extern void (*get_keyboard_poll_task(void))(void);

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

    // Initialize serial port FIRST for logging
    serial_init();
    serial_write("\n=== GrahaOS Boot Log ===\n");
    serial_write("Serial port initialized\n");

    // Register hardware base capabilities (always ON, no deps)
    // Must happen after serial_init (for logging) but before driver inits
    serial_write("Registering hardware capabilities...\n");
    cap_register("cpu", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("memory", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("interrupt_controller", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("pci_bus", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("framebuffer_hw", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    serial_write("Hardware capabilities registered\n");

    // Initialize framebuffer for early output
    if (!framebuffer_init(&framebuffer_request)) {
        serial_write("ERROR: Framebuffer init failed!\n");
        hcf();
    }

    framebuffer_clear(0x00101828);
    serial_write("Framebuffer initialized\n");

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
    serial_write("Initializing PMM...\n");
    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    serial_write("PMM initialized successfully\n");
    y_pos += 20;

    // 2. Initialize Virtual Memory Manager and enable paging
    serial_write("Initializing VMM...\n");
    vmm_init(memmap_request.response, framebuffer_request.response,
             kernel_phys_base, kernel_virt_base, hhdm_offset);
    framebuffer_init(&framebuffer_request);  // Reinitialize after paging
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828);
    serial_write("VMM initialized, paging active\n");
    y_pos += 20;
    
    // 3. CRITICAL: Initialize SMP FIRST (sets up GDT/TSS for BSP and starts APs)
    // This also initializes LAPIC and LAPIC timer for all CPUs
    serial_write("About to initialize SMP...\n");
    #if LIMINE_API_REVISION >= 1
        serial_write("Calling smp_init (mp_request)...\n");
        smp_init(&mp_request);
    #else
        serial_write("Calling smp_init (smp_request)...\n");
        smp_init(&smp_request);
    #endif
    serial_write("SMP initialized successfully\n");
    serial_write("Drawing framebuffer message...\n");
    framebuffer_draw_string("SMP Initialized - All CPUs online!", 50, y_pos, COLOR_GREEN, 0x00101828);
    serial_write("Framebuffer drawn OK\n");
    y_pos += 20;

    // 4. NOW we can initialize IDT (after GDT is set up)
    serial_write("About to initialize IDT...\n");
    idt_init();
    serial_write("IDT initialized successfully\n");
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 5. Initialize scheduler (after per-CPU structures exist)
    serial_write("About to initialize scheduler...\n");
    sched_init();
    serial_write("Scheduler initialized successfully\n");
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;
    
    // 6. Initialize syscall interface (after per-CPU structures exist)
    serial_write("About to initialize syscalls...\n");
    syscall_init();
    serial_write("Syscalls initialized successfully\n");
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 7. Initialize Virtual File System
    serial_write("About to initialize VFS...\n");
    vfs_init();
    serial_write("VFS initialized successfully\n");
    framebuffer_draw_string("VFS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    //adding AHCI
    serial_write("About to initialize AHCI...\n");
    ahci_init();
    serial_write("AHCI initialized successfully\n");
    y_pos += 20;

    // Initialize E1000 NIC driver
    serial_write("About to initialize E1000 NIC...\n");
    e1000_init();
    serial_write("E1000 initialization complete\n");
    y_pos += 20;

    // Initialize Mongoose TCP/IP stack
    serial_write("About to initialize network stack...\n");
    net_init();
    serial_write("Network stack initialization complete\n");
    y_pos += 20;

    // Initialize filesystem AFTER a small delay to let AHCI stabilize
    serial_write("Waiting for AHCI to stabilize...\n");
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    serial_write("AHCI stabilized\n");

    framebuffer_draw_string("Mounting GrahaFS filesystem...", 10, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    // Initialize GrahaFS driver
    serial_write("Initializing GrahaFS driver...\n");
    grahafs_init();
    serial_write("GrahaFS driver initialized\n");

    // Get first block device (disk 0)
    serial_write("About to call vfs_get_block_device(0)...\n");
    block_device_t* hdd = vfs_get_block_device(0);
    serial_write("vfs_get_block_device(0) returned: ");
    serial_write_hex((uint64_t)hdd);
    serial_write("\n");

    if (hdd) {
        serial_write("Block device found, drawing framebuffer msg...\n");
        framebuffer_draw_string("Found block device 0", 10, y_pos, COLOR_GREEN, 0x00101828);
        serial_write("Framebuffer msg drawn\n");
        y_pos += 20;

        serial_write("About to call grahafs_mount...\n");
        vfs_node_t* root = grahafs_mount(hdd);
        serial_write("grahafs_mount returned: ");
        serial_write_hex((uint64_t)root);
        serial_write("\n");

        if (root) {
            serial_write("Mount successful, drawing success msg...\n");
            framebuffer_draw_string("GrahaFS mounted successfully on /", 10, y_pos, COLOR_GREEN, 0x00101828);
            serial_write("Success msg drawn\n");
            y_pos += 20;
        } else {
            serial_write("Mount failed, drawing error msgs...\n");
            framebuffer_draw_string("Failed to mount GrahaFS!", 10, y_pos, COLOR_RED, 0x00101828);
            framebuffer_draw_string("Disk may need formatting with mkfs.gfs", 10, y_pos + 20, COLOR_YELLOW, 0x00101828);
            serial_write("Error msgs drawn\n");
            y_pos += 40;
        }
    } else {
        serial_write("No block device found, drawing error msg...\n");
        framebuffer_draw_string("No block device found!", 10, y_pos, COLOR_RED, 0x00101828);
        serial_write("Error msg drawn\n");
        y_pos += 20;
    }

    // --- USER SPACE INITIALIZATION ---
    serial_write("=== USER SPACE INITIALIZATION ===\n");
    framebuffer_draw_string("=== Loading Interactive Shell ===", 50, y_pos, COLOR_WHITE, 0x00101828);
    y_pos += 30;

    // Initialize Initial RAM Disk
    serial_write("Initializing initrd...\n");
    initrd_init(&module_request);
    serial_write("Initrd initialized\n");
    framebuffer_draw_string("Initrd initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Locate shell binary in initrd
    serial_write("Looking up bin/gash in initrd...\n");
    size_t gash_size;
    void *gash_data = initrd_lookup("bin/gash", &gash_size);
    if (!gash_data) {
        serial_write("FATAL: Could not find bin/gash in initrd!\n");
        framebuffer_draw_string("FATAL: Could not find bin/gash in initrd!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    serial_write("Found bin/gash, size=");
    serial_write_dec(gash_size);
    serial_write("\n");
    framebuffer_draw_string("Found bin/gash (shell) in initrd.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Load shell ELF binary
    serial_write("About to call elf_load for gash...\n");
    uint64_t entry_point, cr3;
    if (!elf_load(gash_data, &entry_point, &cr3)) {
        serial_write("FATAL: elf_load failed!\n");
        framebuffer_draw_string("FATAL: Failed to load shell ELF file!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    serial_write("elf_load succeeded! entry_point=");
    serial_write_hex(entry_point);
    serial_write(" cr3=");
    serial_write_hex(cr3);
    serial_write("\n");

    serial_write("Drawing 'Shell loaded' message...\n");
    framebuffer_draw_string("Shell loaded successfully into memory.", 50, y_pos, COLOR_GREEN, 0x00101828);
    serial_write("Message drawn\n");
    y_pos += 20;

    // Create shell process
    serial_write("Creating shell process...\n");
    int process_id = sched_create_user_process(entry_point, cr3);
    if (process_id < 0) {
        serial_write("ERROR: Failed to create shell process!\n");
        framebuffer_draw_string("FATAL: Failed to create shell process!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    serial_write("Shell process created, ID=");
    serial_write_dec(process_id);
    serial_write("\n");

    // Set process name for the shell
    {
        task_t *shell_task = sched_get_task(process_id);
        if (shell_task) {
            const char *n = "gash";
            int j = 0;
            while (n[j] && j < 31) { shell_task->name[j] = n[j]; j++; }
            shell_task->name[j] = '\0';
        }
    }

    serial_write("Drawing 'Shell process created' message...\n");
    framebuffer_draw_string("Shell process created.", 50, y_pos, COLOR_GREEN, 0x00101828);
    serial_write("Message drawn\n");
    y_pos += 20;

    // Initialize keyboard hardware BEFORE creating the polling task
    serial_write("Initializing keyboard...\n");
    keyboard_init();
    serial_write("Keyboard initialized\n");
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
    serial_write("Creating keyboard task...\n");
    int kbd_task_id = sched_create_task(task_func);
    serial_write("sched_create_task returned: ");
    serial_write_dec(kbd_task_id);
    serial_write("\n");

    // Set task name for keyboard polling
    {
        task_t *kbd_task = sched_get_task(kbd_task_id);
        if (kbd_task) {
            const char *n = "kbd_poll";
            int j = 0;
            while (n[j] && j < 31) { kbd_task->name[j] = n[j]; j++; }
            kbd_task->name[j] = '\0';
        }
    }

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

    // Create Mongoose polling task
    serial_write("Creating mongoose poll task...\n");
    int mg_task_id = sched_create_task(mongoose_poll_task);
    serial_write("sched_create_task (mongoose) returned: ");
    serial_write_dec(mg_task_id);
    serial_write("\n");
    {
        task_t *mg_task = sched_get_task(mg_task_id);
        if (mg_task) {
            const char *n = "mongoose";
            int j = 0;
            while (n[j] && j < 31) { mg_task->name[j] = n[j]; j++; }
            mg_task->name[j] = '\0';
        }
    }
    if (mg_task_id >= 0) {
        framebuffer_draw_string("Mongoose task created successfully", 50, y_pos, COLOR_GREEN, 0x00101828);
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
    serial_write("About to enable interrupts...\n");
    framebuffer_draw_string("Enabling interrupts...", 10, 50, COLOR_YELLOW, 0x00101828);
    asm volatile("sti");
    serial_write("Interrupts enabled\n");

    // Wait to ensure no pending issues
    serial_write("Waiting after STI...\n");
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    serial_write("Wait complete\n");

    // NOW start the timer on BSP only
    serial_write("About to start scheduler timer...\n");
    framebuffer_draw_string("Starting scheduler timer on BSP...", 10, 70, COLOR_YELLOW, 0x00101828);
    
    // Disable interrupts briefly while starting timer
    asm volatile("cli");
    serial_write("Calling lapic_timer_init...\n");
    lapic_timer_init(100, 32);
    serial_write("lapic_timer_init returned\n");
    asm volatile("sti");
    serial_write("Re-enabled interrupts after timer init\n");

    if (!lapic_timer_is_running()) {
        serial_write("ERROR: Timer failed to start!\n");
        framebuffer_draw_string("ERROR: Timer failed to start!", 10, 90, COLOR_RED, 0x00101828);
    } else {
        serial_write("Timer is running, system initialized!\n");
        framebuffer_draw_string("System running!", 10, 90, COLOR_GREEN, 0x00101828);
    }
    
   
    // OPTIONAL: Start timers on APs later (commented out for now)
    // This would require IPI (Inter-Processor Interrupts) to signal APs
    // For now, only BSP handles scheduling

    // Register service and application capabilities
    serial_write("Registering service/application capabilities...\n");
    {
        const char *sched_deps[] = {"timer", "memory"};
        cap_register("scheduler", CAP_SERVICE, 0, -1, sched_deps, 2,
                     NULL, NULL, NULL, 0, NULL);

        const char *shell_deps[] = {"display", "keyboard_input", "filesystem"};
        cap_register("shell", CAP_APPLICATION, 0, -1, shell_deps, 3,
                     NULL, NULL, NULL, 0, NULL);
    }

    // Activate all registered capabilities
    serial_write("Activating all capabilities...\n");
    for (int i = 0; i < cap_get_count(); i++) {
        cap_activate(i);
    }
    serial_write("All capabilities activated\n");

    serial_write("\n=== ENTERING IDLE LOOP ===\n");
    serial_write("System fully initialized, waiting for timer interrupts...\n\n");

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