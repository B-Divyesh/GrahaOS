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
#include "../drivers/serial/serial.h"
#include "../../../kernel/log.h"
#include "../../../kernel/percpu.h"

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
spinlock_t ap_startup_lock = SPINLOCK_INITIALIZER("ap_startup");
volatile uint32_t aps_started = 0;

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

// Phase 20: BSP signals "APs may start their scheduler timers" via this
// flag after grahafs_v2_mount completes in kmain. Until then, APs idle
// without a timer. This preserves Phase 19's invariant that the FS is
// mounted before any scheduling occurs.
volatile bool g_ap_scheduler_go = false;

// C entry point for APs
#if LIMINE_API_REVISION >= 1
void ap_main(struct limine_mp_info *info) {
#else
void ap_main(struct limine_smp_info *info) {
#endif
    // Keep interrupts disabled during initialization
    asm volatile("cli");

    // Validate info
    if (!info || info->processor_id >= MAX_CPUS) {
        asm volatile("hlt");
        while(1);
    }

    // Set up per-CPU data
    g_cpu_locals[info->processor_id].cpu_id = info->processor_id;
    g_cpu_locals[info->processor_id].lapic_id = info->lapic_id;
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[info->processor_id]);

    // Phase 14: initialise the Phase 14 extension fields (magazines,
    // preempt counters, self-pointer) for this AP. Done via direct
    // array indexing, not GS-relative, so it doesn't care if GSBASE
    // gets clobbered by the upcoming gdt_init.
    // Phase 20 side-effect: percpu_init now runs runq_init on this AP's
    // per-CPU runqueue (sched_rq_head/tail placeholders replaced by the
    // inline runq_t).
    percpu_init(info->processor_id);

    // Initialize core structures. gdt_init_for_cpu performs
    // `mov gs, ax` with the kernel data selector — in long mode this
    // clobbers the hidden GS.base that we just wrote via MSR_GS_BASE.
    // Re-issue the wrmsr after GDT reload so per_cpu() reads land in
    // this CPU's block.
    gdt_init_for_cpu(info->processor_id);
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[info->processor_id]);
    idt_init();
    lapic_init();
    syscall_init();

    // Mark as active
    uint32_t cpu_id = info->processor_id;
    spinlock_acquire(&ap_startup_lock);
    g_cpu_info[cpu_id].active = true;
    aps_started++;

    // Print message
    char msg[64] = "AP: CPU ";
    msg[8] = '0' + cpu_id;
    msg[9] = ' ';
    msg[10] = 'r';
    msg[11] = 'e';
    msg[12] = 'a';
    msg[13] = 'd';
    msg[14] = 'y';
    msg[15] = '\0';
    framebuffer_draw_string(msg, 50, 420 + (cpu_id * 20), COLOR_CYAN, 0x00101828);
    spinlock_release(&ap_startup_lock);

    // Phase 20: wait for BSP to finish mounting FS and signal go-ahead. Then
    // start this AP's LAPIC timer at 100 Hz (same vector 32 as BSP), enable
    // interrupts, and drop into an idle loop. Every tick lands in the
    // common schedule() which reads this CPU's local runq, work-steals if
    // empty, falls back to the idle hlt.
    while (!g_ap_scheduler_go) {
        asm volatile("pause");
    }
    klog(KLOG_INFO, SUBSYS_CORE,
         "[AP] CPU %u: g_ap_scheduler_go observed, arming LAPIC timer", cpu_id);

    lapic_timer_init(100, 32);
    klog(KLOG_INFO, SUBSYS_CORE,
         "[AP] CPU %u: timer armed, enabling interrupts", cpu_id);
    asm volatile("sti");
    klog(KLOG_INFO, SUBSYS_CORE,
         "[AP] CPU %u: interrupts enabled, entering hlt loop", cpu_id);

    // AP idle loop: hlt until next interrupt. The timer IRQ will call
    // schedule() which may pick up a work-stolen task and jump to it; when
    // that task blocks or time slices out, schedule() returns here and we
    // hlt again. If the runq was empty and steal also failed, schedule()
    // dispatches this CPU's notional idle task — which is, conceptually,
    // this very loop.
    while (1) {
        asm volatile("hlt");
    }
}

#if LIMINE_API_REVISION >= 1
void smp_init(volatile struct limine_mp_request *mp_request) {
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Entered smp_init");

    if (!mp_request || !mp_request->response) {
        klog(KLOG_ERROR, SUBSYS_CORE, "[SMP] ERROR: No MP support from bootloader!");
        framebuffer_draw_string("No MP support from bootloader!", 50, 400, COLOR_RED, 0x00101828);
        return;
    }

    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] MP request validated");
    struct limine_mp_response *mp_resp = mp_request->response;

    // Initialize CPU count and BSP ID
    g_cpu_count = (uint32_t)mp_resp->cpu_count;
    g_bsp_lapic_id = mp_resp->bsp_lapic_id;

    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] CPU count: %lu", (unsigned long)(g_cpu_count));
    
    if (g_cpu_count > MAX_CPUS) {
        g_cpu_count = MAX_CPUS;
    }

    // Initialize all cpu_locals to invalid state
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Initializing cpu_locals...");
    for (int i = 0; i < MAX_CPUS; i++) {
        g_cpu_locals[i].cpu_id = (uint32_t)-1;
        g_cpu_locals[i].lapic_id = (uint32_t)-1;
    }

    // Store kernel PML4 for APs
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Getting kernel PML4...");
    g_kernel_pml4 = vmm_get_pml4_phys(vmm_get_kernel_space());
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] PML4=0x%lx", (unsigned long)(g_kernel_pml4));

    // Initialize BSP's per-CPU data FIRST
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Init BSP cpu_local...");
    g_cpu_locals[0].cpu_id = 0;
    g_cpu_locals[0].lapic_id = g_bsp_lapic_id;
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[0]);
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] BSP GS_BASE set");

    // Initialize BSP's GDT and TSS (per-CPU version). gdt_init_for_cpu
    // reloads the data segment registers, and in long mode `mov gs, ax`
    // clobbers the hidden GS.base we set via wrmsr above. Re-issue the
    // wrmsr after the GDT reload so Phase 14 per_cpu() reads land in
    // this CPU's block.
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Calling gdt_init_for_cpu...");
    gdt_init_for_cpu(0);
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[0]);
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] GDT init done");

    // Initialize BSP's LAPIC
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] Calling lapic_init...");
    lapic_init();
    klog(KLOG_INFO, SUBSYS_CORE, "[SMP] LAPIC init done");



    // Verify LAPIC is working
    // Verify LAPIC is working
    if (!lapic_is_enabled()) {
        framebuffer_draw_string("ERROR: BSP LAPIC failed!", 50, 380, COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("BSP LAPIC initialized", 50, 380, COLOR_GREEN, 0x00101828);
    }
    
    // DON'T start timer here - let main.c do it after everything is ready
    // lapic_timer_init(100, 32);  // COMMENTED OUT
    
    framebuffer_draw_string("BSP ready (timer delayed)", 50, 395, COLOR_YELLOW, 0x00101828);
    

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
    framebuffer_draw_string(bsp_msg, 200, 400, COLOR_GREEN, 0x00101828);
    
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

    // Initialize BSP's GDT and TSS (per-CPU version). Re-write GSBASE
    // after, since gdt_load clobbers the hidden GS.base via `mov gs, ax`.
    gdt_init_for_cpu(0);
    write_msr(MSR_GS_BASE, (uint64_t)&g_cpu_locals[0]);

    // Initialize BSP's LAPIC
    lapic_init();
    
    // Verify LAPIC is working
    if (!lapic_is_enabled()) {
        framebuffer_draw_string("ERROR: BSP LAPIC failed!", 50, 380, COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("BSP LAPIC initialized", 50, 380, COLOR_GREEN, 0x00101828);
    }
    
    // DON'T start timer here - let main.c do it after everything is ready
    // lapic_timer_init(100, 32);  // COMMENTED OUT
    
    framebuffer_draw_string("BSP ready (timer delayed)", 50, 395, COLOR_YELLOW, 0x00101828);
    
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