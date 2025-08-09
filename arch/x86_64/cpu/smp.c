// arch/x86_64/cpu/smp.c
#include "smp.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../drivers/lapic/lapic.h"
#include "../drivers/lapic_timer/lapic_timer.h"
#include "../../../drivers/video/framebuffer.h"
#include "../../../kernel/sync/spinlock.h"
#include "gdt.h"
#include "idt.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "interrupts.h"

// External assembly function from ap_start.S
#if LIMINE_API_REVISION >= 1
extern void ap_trampoline(struct limine_mp_info *info);
#else
extern void ap_trampoline(struct limine_smp_info *info);
#endif

// Global CPU information
cpu_info_t g_cpu_info[MAX_CPUS];
uint32_t g_cpu_count = 0;
uint32_t g_bsp_lapic_id = 0;
uint64_t g_kernel_pml4 = 0;

// Global per-CPU data structures
cpu_local_t g_cpu_locals[MAX_CPUS];

// MSR for GS base
#define MSR_GS_BASE 0xC0000101

// Spinlock for synchronized AP startup
static spinlock_t ap_startup_lock = SPINLOCK_INITIALIZER("ap_startup");
static volatile uint32_t aps_started = 0;

// Helper to write MSR
static void write_msr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

// Get current CPU ID using GS segment
uint32_t smp_get_current_cpu_id(void) {
    // Check if GS is set up
    uint32_t gs_low, gs_high;
    asm volatile(
        "rdmsr"
        : "=a"(gs_low), "=d"(gs_high)
        : "c"(MSR_GS_BASE)
    );
    
    uint64_t gs_base = ((uint64_t)gs_high << 32) | gs_low;
    
    if (gs_base == 0) {
        return 0; // Default to BSP if GS not set up
    }
    
    uint32_t id;
    asm volatile("movl %%gs:0, %0" : "=r"(id));
    return id;
}

// Helper to convert LAPIC ID to linear CPU ID
static uint32_t lapic_to_cpu_id(uint32_t lapic_id) {
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpu_info[i].lapic_id == lapic_id) {
            return i;
        }
    }
    return 0; // Should not happen, default to BSP
}

uint32_t smp_get_current_cpu(void) {
    // Try GS-based method first (most reliable)
    if (g_cpu_locals[0].cpu_id != (uint32_t)-1) {
        return smp_get_current_cpu_id();
    }
    // Fall back to LAPIC if GS not set up yet
    if (!lapic_is_enabled()) {
        return 0; // BSP before LAPIC init
    }
    return lapic_to_cpu_id(lapic_get_id());
}

cpu_info_t* smp_get_cpu_info(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) {
        return NULL;
    }
    return &g_cpu_info[cpu_id];
}

// C entry point for APs
#if LIMINE_API_REVISION >= 1
void ap_main(struct limine_mp_info *info) {
#else
void ap_main(struct limine_smp_info *info) {
#endif
    // Disable interrupts during initialization
    asm volatile("cli");
    
    // Set up per-CPU data FIRST, before any other initialization
    g_cpu_locals[info->processor_id].cpu_id = info->processor_id;
    g_cpu_locals[info->processor_id].lapic_id = info->lapic_id;
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[info->processor_id]);
    
    // Now initialize per-core structures
    gdt_init_for_cpu(info->processor_id);  // Load per-CPU GDT and TSS
    idt_init();       // Load IDT for this core
    lapic_init();     // Initialize this core's LAPIC
    syscall_init();   // Initialize syscall MSRs for this core
    
    // Initialize LAPIC timer for this AP (100Hz, vector 32)
    lapic_timer_init(100, 32);
    
    // Get our CPU ID (now using GS)
    uint32_t cpu_id = smp_get_current_cpu_id();
    
    // Mark ourselves as active
    spinlock_acquire(&ap_startup_lock);
    g_cpu_info[cpu_id].active = true;
    aps_started++;
    
    // Print startup message
    char msg[64] = "AP: CPU X (LAPIC ID XX) + Timer online!";
    if (cpu_id < 10) {
        msg[8] = '0' + cpu_id;
    } else {
        msg[8] = 'A' + (cpu_id - 10); // For CPU IDs > 9, use letters
    }
    
    // Convert LAPIC ID to string
    uint32_t lapic_id = info->lapic_id;
    if (lapic_id < 10) {
        msg[21] = '0' + lapic_id;
        msg[22] = ')';
    } else if (lapic_id < 100) {
        msg[21] = '0' + (lapic_id / 10);
        msg[22] = '0' + (lapic_id % 10);
    } else {
        // Handle 3-digit LAPIC IDs
        msg[20] = '0' + (lapic_id / 100);
        msg[21] = '0' + ((lapic_id / 10) % 10);
        msg[22] = '0' + (lapic_id % 10);
    }
    
    framebuffer_draw_string(msg, 50, 420 + (cpu_id * 20), COLOR_CYAN, 0x00101828);
    spinlock_release(&ap_startup_lock);
    
    // Enable interrupts and enter idle loop
    asm volatile("sti");
    
    // Enter scheduler idle loop
    while (1) {
        asm volatile("hlt");
        // Scheduler will take over when timer interrupts fire
    }
}

#if LIMINE_API_REVISION >= 1
void smp_init(volatile struct limine_mp_request *mp_request) {
    if (!mp_request || !mp_request->response) {
        framebuffer_draw_string("No MP support from bootloader!", 50, 400, COLOR_RED, 0x00101828);
        return;
    }
    
    struct limine_mp_response *mp_resp = mp_request->response;
    
    // Initialize CPU count and BSP ID
    g_cpu_count = (uint32_t)mp_resp->cpu_count;
    g_bsp_lapic_id = mp_resp->bsp_lapic_id;
    
    if (g_cpu_count > MAX_CPUS) {
        g_cpu_count = MAX_CPUS;
    }
    
    // Initialize all cpu_locals to invalid state
    for (int i = 0; i < MAX_CPUS; i++) {
        g_cpu_locals[i].cpu_id = (uint32_t)-1;
        g_cpu_locals[i].lapic_id = (uint32_t)-1;
    }
    
    // Store kernel PML4 for APs
    g_kernel_pml4 = vmm_get_pml4_phys(vmm_get_kernel_space());
    
    // Initialize BSP's per-CPU data FIRST
    g_cpu_locals[0].cpu_id = 0;
    g_cpu_locals[0].lapic_id = g_bsp_lapic_id;
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[0]);
    
    // Initialize BSP's GDT and TSS (per-CPU version)
    gdt_init_for_cpu(0);
    
    // Initialize BSP's LAPIC
    lapic_init();
    // Initialize LAPIC timer for BSP (100Hz, vector 32)
    lapic_timer_init(100, 32);
    framebuffer_draw_string("BSP LAPIC Timer initialized", 50, 390, COLOR_GREEN, 0x00101828);
    // Build CPU info table
    for (uint64_t i = 0; i < mp_resp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_mp_info *cpu = mp_resp->cpus[i];
        g_cpu_info[i].lapic_id = cpu->lapic_id;
        g_cpu_info[i].cpu_id = (uint32_t)i;
        g_cpu_info[i].active = (cpu->lapic_id == g_bsp_lapic_id);
        g_cpu_info[i].kernel_stack = 0;
    }
    
    // Print BSP info
    char bsp_msg[64] = "BSP: CPU 0 (LAPIC ID XX) online!";
    uint32_t bsp_id = g_bsp_lapic_id;
    if (bsp_id < 10) {
        bsp_msg[22] = '0' + bsp_id;
        bsp_msg[23] = ')';
        bsp_msg[24] = ' ';
    } else if (bsp_id < 100) {
        bsp_msg[22] = '0' + (bsp_id / 10);
        bsp_msg[23] = '0' + (bsp_id % 10);
        bsp_msg[24] = ')';
    } else {
        // Handle 3-digit LAPIC IDs
        bsp_msg[21] = '0' + (bsp_id / 100);
        bsp_msg[22] = '0' + ((bsp_id / 10) % 10);
        bsp_msg[23] = '0' + (bsp_id % 10);
        bsp_msg[24] = ')';
    }
    framebuffer_draw_string(bsp_msg, 50, 400, COLOR_GREEN, 0x00101828);
    
    // Start all APs
    uint32_t aps_to_start = 0;
    for (uint64_t i = 0; i < mp_resp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_mp_info *cpu = mp_resp->cpus[i];
        
        // Skip the BSP
        if (cpu->lapic_id == g_bsp_lapic_id) {
            continue;
        }
        
        // Allocate kernel stack for AP
        size_t stack_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
        void *ap_stack_phys = pmm_alloc_pages(stack_pages);
        if (!ap_stack_phys) {
            framebuffer_draw_string("Failed to allocate AP stack!", 50, 440 + (i * 20), COLOR_RED, 0x00101828);
            continue;
        }
        
        uint64_t ap_stack_top = (uint64_t)ap_stack_phys + KERNEL_STACK_SIZE + g_hhdm_offset;
        g_cpu_info[i].kernel_stack = ap_stack_top;
        
        // Set AP startup parameters
        cpu->goto_address = ap_trampoline;
        cpu->extra_argument = ap_stack_top;
        
        aps_to_start++;
    }
    
    // Wait for all APs to start (with timeout)
    uint64_t timeout = 1000000000; // Arbitrary large number
    while (aps_started < aps_to_start && timeout > 0) {
        asm volatile("pause");
        timeout--;
    }
    
    // Disable the legacy PIC now that all cores are using LAPIC
    pic_disable();
    framebuffer_draw_string("Legacy PIC disabled.", 50, 480, COLOR_YELLOW, 0x00101828);
    
    // Report final status
    char status_msg[64] = "MP: X of X CPUs online";
    status_msg[4] = '0' + (aps_started + 1); // +1 for BSP
    status_msg[9] = '0' + g_cpu_count;
    framebuffer_draw_string(status_msg, 50, 500, COLOR_WHITE, 0x00101828);
}

#else
// Legacy SMP implementation for LIMINE_API_REVISION < 1
void smp_init(volatile struct limine_smp_request *smp_request) {
    if (!smp_request || !smp_request->response) {
        framebuffer_draw_string("No SMP support from bootloader!", 50, 400, COLOR_RED, 0x00101828);
        return;
    }
    
    struct limine_smp_response *smp_resp = smp_request->response;
    
    // Initialize CPU count and BSP ID
    g_cpu_count = (uint32_t)smp_resp->cpu_count;
    g_bsp_lapic_id = smp_resp->bsp_lapic_id;
    
    if (g_cpu_count > MAX_CPUS) {
        g_cpu_count = MAX_CPUS;
    }
    
    // Initialize all cpu_locals to invalid state
    for (int i = 0; i < MAX_CPUS; i++) {
        g_cpu_locals[i].cpu_id = (uint32_t)-1;
        g_cpu_locals[i].lapic_id = (uint32_t)-1;
    }
    
    // Store kernel PML4 for APs
    g_kernel_pml4 = vmm_get_pml4_phys(vmm_get_kernel_space());
    
    // Initialize BSP's per-CPU data FIRST
    g_cpu_locals[0].cpu_id = 0;
    g_cpu_locals[0].lapic_id = g_bsp_lapic_id;
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[0]);
    
    // Initialize BSP's GDT and TSS (per-CPU version)
    gdt_init_for_cpu(0);
    
    // Initialize BSP's LAPIC
    lapic_init();
    // Initialize LAPIC timer for BSP (100Hz, vector 32)
    lapic_timer_init(100, 32);
    framebuffer_draw_string("BSP LAPIC Timer initialized", 50, 390, COLOR_GREEN, 0x00101828);
    // Build CPU info table
    for (uint64_t i = 0; i < smp_resp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_smp_info *cpu = smp_resp->cpus[i];
        g_cpu_info[i].lapic_id = cpu->lapic_id;
        g_cpu_info[i].cpu_id = (uint32_t)i;
        g_cpu_info[i].active = (cpu->lapic_id == g_bsp_lapic_id);
        g_cpu_info[i].kernel_stack = 0;
    }
    
    // Print BSP info
    char bsp_msg[64] = "BSP: CPU 0 (LAPIC ID XX) online!";
    uint32_t bsp_id = g_bsp_lapic_id;
    if (bsp_id < 10) {
        bsp_msg[22] = '0' + bsp_id;
        bsp_msg[23] = ')';
        bsp_msg[24] = ' ';
    } else if (bsp_id < 100) {
        bsp_msg[22] = '0' + (bsp_id / 10);
        bsp_msg[23] = '0' + (bsp_id % 10);
        bsp_msg[24] = ')';
    } else {
        // Handle 3-digit LAPIC IDs
        bsp_msg[21] = '0' + (bsp_id / 100);
        bsp_msg[22] = '0' + ((bsp_id / 10) % 10);
        bsp_msg[23] = '0' + (bsp_id % 10);
        bsp_msg[24] = ')';
    }
    framebuffer_draw_string(bsp_msg, 50, 400, COLOR_GREEN, 0x00101828);
    
    // Start all APs
    uint32_t aps_to_start = 0;
    for (uint64_t i = 0; i < smp_resp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_smp_info *cpu = smp_resp->cpus[i];
        
        // Skip the BSP
        if (cpu->lapic_id == g_bsp_lapic_id) {
            continue;
        }
        
        // Allocate kernel stack for AP
        size_t stack_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
        void *ap_stack_phys = pmm_alloc_pages(stack_pages);
        if (!ap_stack_phys) {
            framebuffer_draw_string("Failed to allocate AP stack!", 50, 440 + (i * 20), COLOR_RED, 0x00101828);
            continue;
        }
        
        uint64_t ap_stack_top = (uint64_t)ap_stack_phys + KERNEL_STACK_SIZE + g_hhdm_offset;
        g_cpu_info[i].kernel_stack = ap_stack_top;
        
        // Set AP startup parameters
        cpu->goto_address = ap_trampoline;
        cpu->extra_argument = ap_stack_top;
        
        aps_to_start++;
    }
    
    // Wait for all APs to start (with timeout)
    uint64_t timeout = 1000000000; // Arbitrary large number
    while (aps_started < aps_to_start && timeout > 0) {
        asm volatile("pause");
        timeout--;
    }
    
    // Disable the legacy PIC now that all cores are using LAPIC
    pic_disable();
    framebuffer_draw_string("Legacy PIC disabled.", 50, 480, COLOR_YELLOW, 0x00101828);
    
    // Report final status
    char status_msg[64] = "SMP: X of X CPUs online";
    status_msg[5] = '0' + (aps_started + 1); // +1 for BSP
    status_msg[10] = '0' + g_cpu_count;
    framebuffer_draw_string(status_msg, 50, 500, COLOR_WHITE, 0x00101828);
}
#endif