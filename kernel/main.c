#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "limine.h"
#include "initrd.h"
#include "elf.h"
#include "fs/vfs.h"
#include "fs/pipe.h"
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
#include "cap/can.h"
#include "cap/object.h"
#include "cmdline.h"
#include "autorun.h"
#include "log.h"
#include "percpu.h"
#include "mm/slab.h"
#include "mm/kheap.h"
#include "rtc.h"
#include "audit.h"
#include "ipc/manifest.h"
#include "mm/vmo.h"
#include "ipc/channel.h"
#include "io/stream.h"

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

// Phase 12: kernel command-line request. Parsed after serial_init so
// autorun=, quiet=, test_timeout_seconds= are available to boot logic.
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
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

    // Enable SSE/SSE2 on BSP (APs do this in ap_start.S)
    // Required for mongoose.c compiled with SSE2 (TLS crypto, float code)
    {
        uint64_t cr0, cr4;
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1UL << 2);  // Clear CR0.EM (no FPU emulation)
        cr0 |=  (1UL << 1);  // Set CR0.MP (monitor coprocessor)
        asm volatile("mov %0, %%cr0" :: "r"(cr0));
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1UL << 9);   // Set CR4.OSFXSR (enable SSE)
        cr4 |= (1UL << 10);  // Set CR4.OSXMMEXCPT (SSE exceptions)
        asm volatile("mov %0, %%cr4" :: "r"(cr4));
    }

    // Initialize serial port FIRST for logging
    serial_init();
    klog(KLOG_INFO, SUBSYS_CORE, "\n=== GrahaOS Boot Log ===\nSerial port initialized");

    // Phase 12: parse the kernel command line. Populates g_cmdline_flags
    // so every subsequent boot step can see autorun/quiet/timeout values.
    {
        const char *raw = NULL;
        if (cmdline_request.response && cmdline_request.response->cmdline) {
            raw = cmdline_request.response->cmdline;
        }
        cmdline_parse(raw);
    }

    // Phase 13 fault injection: optionally emit klog calls BEFORE
    // klog_init so the early-drop counter has work to do. Each call
    // bumps g_early_drops; klog_init then surfaces the count via a
    // retrospective KLOG_WARN entry.
    if (g_cmdline_flags.inject_klog_preinit) {
        for (uint32_t i = 0; i < g_cmdline_flags.inject_klog_preinit; i++) {
            klog(KLOG_INFO, SUBSYS_CORE, "preinit test message %u", i);
        }
    }

    // Phase 13: bring up the klog ring. From this point on every
    // subsystem should log via klog() rather than serial_write().
    // The mirror-to-serial flag is still on by default so early boot
    // output keeps flowing through COM1; cmdline `klog_mirror=0`
    // overrides for boot-time perf measurement (spec exit criterion).
    klog_init();
    if (g_cmdline_flags.klog_mirror == 0) {
        klog_disable_mirror();
        klog(KLOG_INFO, SUBSYS_CORE, "klog: mirror disabled via cmdline");
    }

    // Phase 13 fault injection: deliberately wrap the ring. Writing
    // > 16384 entries forces head to wrap; downstream readers must
    // see a contiguous tail and notice seq gaps where entries fell
    // off the front.
    if (g_cmdline_flags.inject_ring_wrap) {
        uint32_t target = g_cmdline_flags.inject_ring_wrap;
        for (uint32_t i = 0; i < target; i++) {
            klog(KLOG_INFO, SUBSYS_TEST, "ring-wrap probe %u", i);
        }
    }
    klog(KLOG_INFO, SUBSYS_CORE, "GrahaOS boot — build=%s",
#ifdef GRAHAOS_BUILD_SHA
         GRAHAOS_BUILD_SHA
#else
         "unknown"
#endif
    );

    // Phase 14: hardware capability registration moved AFTER kheap_init
    // (further down in this function) because can_entry_t objects now
    // live in can_entry_cache, which needs the slab allocator ready.
    // The old comment said "before driver inits" — that's still honored
    // because driver inits happen well after Phase 14 allocator setup.

    // Initialize framebuffer for early output
    if (!framebuffer_init(&framebuffer_request)) {
        klog(KLOG_ERROR, SUBSYS_CORE, "ERROR: Framebuffer init failed!");
        hcf();
    }

    framebuffer_clear(0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "Framebuffer initialized");

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
    klog(KLOG_INFO, SUBSYS_CORE, "Initializing PMM...");
    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "PMM initialized successfully");
    y_pos += 20;

    // 2. Initialize Virtual Memory Manager and enable paging
    klog(KLOG_INFO, SUBSYS_CORE, "Initializing VMM...");
    vmm_init(memmap_request.response, framebuffer_request.response,
             kernel_phys_base, kernel_virt_base, hhdm_offset);
    framebuffer_init(&framebuffer_request);  // Reinitialize after paging
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "VMM initialized, paging active");
    y_pos += 20;
    
    // 3. CRITICAL: Initialize SMP FIRST (sets up GDT/TSS for BSP and starts APs)
    // This also initializes LAPIC and LAPIC timer for all CPUs
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize SMP...");
    #if LIMINE_API_REVISION >= 1
        klog(KLOG_INFO, SUBSYS_CORE, "Calling smp_init (mp_request)...");
        smp_init(&mp_request);
    #else
        klog(KLOG_INFO, SUBSYS_CORE, "Calling smp_init (smp_request)...");
        smp_init(&smp_request);
    #endif
    klog(KLOG_INFO, SUBSYS_CORE, "SMP initialized successfully\nDrawing framebuffer message...");
    framebuffer_draw_string("SMP Initialized - All CPUs online!", 50, y_pos, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "Framebuffer drawn OK");
    y_pos += 20;

    // 3b. Phase 14: per-CPU data + slab + kheap.
    // percpu_init(0) populates the BSP's Phase 14 extension fields
    // (magazines, preempt counters, self-pointer). APs run their own
    // percpu_init in ap_main after they wrmsr GSBASE.
    // kmem_slab_init + kheap_init must come before any kmem_cache_create
    // or kmalloc call — which means before anything that migrates to
    // task_cache/can_entry_cache (sched_init, cap_register beyond the
    // hardware caps already registered above against the static
    // capability[] array; those caps' storage will migrate in a later
    // Phase 14 unit).
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 14: percpu_init(BSP)...");
    percpu_init(0);
    // Phase 15b: read CMOS RTC once, publish g_boot_wall_seconds. Runs
    // before audit_init so every future audit_write carries a correct
    // wall_clock_seconds. No dependency on slab/heap.
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 15b: rtc_init...");
    rtc_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 14: kmem_slab_init...");
    kmem_slab_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 14: kheap_init...");
    kheap_init();
    framebuffer_draw_string("Phase 14 Allocators Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 3b-2. Phase 15a: initialize the cap_object registry. Must run after
    // kheap_init (cap_object_cache is a slab cache) and before the first
    // cap_register call (cap_register now also creates a paired cap_object_t).
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 15a: cap_object_init...");
    cap_object_init();
    framebuffer_draw_string("Phase 15a Cap Objects Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 15b: initialize the audit queue BEFORE the first cap_register
    // so every bootstrap cap's registration emits an audit entry. The
    // flusher isn't running yet; entries queue in memory until attach_fs +
    // the audit flusher kernel thread come up later.
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 15b: audit_init...");
    audit_init();
    framebuffer_draw_string("Phase 15b Audit Queue Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 17: manifest type-hash registry. Populates FNV-1a hashes for the
    // 6 well-known GCP manifest types; consumed by chan_create to accept
    // channels pinned to those types.
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 17: manifest_init...");
    manifest_init();
    framebuffer_draw_string("Phase 17 Manifest Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 17: VMO subsystem. Registers vmo_t slab cache and installs the
    // page-fault hook for COW dispatch.
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 17: vmo_init...");
    vmo_init();
    framebuffer_draw_string("Phase 17 VMOs Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 17: channel subsystem. Registers channel_t + chan_endpoint_t
    // slab caches. Must run after manifest_init (channels consume type hashes).
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 17: channel_subsystem_init...");
    channel_subsystem_init();
    framebuffer_draw_string("Phase 17 Channels Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 18: stream_subsystem_init must wait until after sched_init
    // because it spawns a kernel worker thread via sched_create_task. Its
    // call site is moved just after sched_init below.

    // 3c. Register hardware base capabilities (always ON, no deps).
    // Delayed from pre-PMM to here because Phase 14 made can_entry_t
    // live in can_entry_cache (slab-backed); slab needs pmm + kheap.
    // Still well before driver inits (AHCI, E1000, etc.) which depend
    // on these hw caps.
    klog(KLOG_INFO, SUBSYS_CORE, "Registering hardware capabilities...");
    cap_register("cpu", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("memory", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("interrupt_controller", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("pci_bus", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    cap_register("framebuffer_hw", CAP_HARDWARE, 0, -1, NULL, 0, NULL, NULL, NULL, 0, NULL);
    klog(KLOG_INFO, SUBSYS_CORE, "Hardware capabilities registered");

    // Phase 14: now that slab + hw caps are ready, register the deferred
    // driver caps from serial and framebuffer (their *_init functions ran
    // before the slab was available).
    serial_register_cap();
    framebuffer_register_cap();

    // 4. NOW we can initialize IDT (after GDT is set up)
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize IDT...");
    idt_init();
    klog(KLOG_INFO, SUBSYS_CORE, "IDT initialized successfully");
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 5. Initialize scheduler (after per-CPU structures exist)
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize scheduler...");
    sched_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Scheduler initialized successfully");
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);

    // Phase 15b: start the audit flusher kernel thread. It pulls from the
    // in-memory queue and writes to /var/audit/YYYY-MM-DD.log once
    // audit_attach_fs runs (post-grahafs_mount).
    int audit_flusher_pid = sched_create_task(audit_flusher_task_entry);
    if (audit_flusher_pid >= 0) {
        g_audit_state.flusher_task_id = audit_flusher_pid;
        klog(KLOG_INFO, SUBSYS_AUDIT,
             "audit: flusher task started as pid=%d", audit_flusher_pid);
    } else {
        klog(KLOG_WARN, SUBSYS_AUDIT,
             "audit: flusher task creation failed (rc=%d); remaining in klog-only mode",
             audit_flusher_pid);
    }

    // Phase 18: stream subsystem. Depends on VMO + channel subsystems AND
    // sched_init (worker thread). Registers slab caches and spawns the
    // kernel worker task.
    klog(KLOG_INFO, SUBSYS_CORE, "Phase 18: stream_subsystem_init...");
    stream_subsystem_init();
    framebuffer_draw_string("Phase 18 Streams Ready.", 50, y_pos, COLOR_GREEN, 0x00101828);

    y_pos += 20;
    
    // 6. Initialize syscall interface (after per-CPU structures exist)
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize syscalls...");
    syscall_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Syscalls initialized successfully");
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // 7. Initialize Virtual File System
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize VFS...");
    vfs_init();
    klog(KLOG_INFO, SUBSYS_CORE, "VFS initialized successfully");

    // Phase 10b: Initialize pipe subsystem
    pipe_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Pipe subsystem initialized");

    framebuffer_draw_string("VFS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 40;

    //adding AHCI
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize AHCI...");
    ahci_init();
    klog(KLOG_INFO, SUBSYS_CORE, "AHCI initialized successfully");
    y_pos += 20;

    // Initialize E1000 NIC driver
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize E1000 NIC...");
    e1000_init();
    klog(KLOG_INFO, SUBSYS_CORE, "E1000 initialization complete");
    y_pos += 20;

    // Initialize Mongoose TCP/IP stack
    klog(KLOG_INFO, SUBSYS_CORE, "About to initialize network stack...");
    net_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Network stack initialization complete");
    y_pos += 20;

    // Initialize filesystem AFTER a small delay to let AHCI stabilize
    klog(KLOG_INFO, SUBSYS_CORE, "Waiting for AHCI to stabilize...");
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    klog(KLOG_INFO, SUBSYS_CORE, "AHCI stabilized");

    framebuffer_draw_string("Mounting GrahaFS filesystem...", 10, y_pos, COLOR_YELLOW, 0x00101828);
    y_pos += 20;

    // Initialize GrahaFS driver
    klog(KLOG_INFO, SUBSYS_CORE, "Initializing GrahaFS driver...");
    grahafs_init();
    klog(KLOG_INFO, SUBSYS_CORE, "GrahaFS driver initialized");

    // Get first block device (disk 0)
    klog(KLOG_INFO, SUBSYS_CORE, "About to call vfs_get_block_device(0)...");
    block_device_t* hdd = vfs_get_block_device(0);
    klog(KLOG_INFO, SUBSYS_CORE, "vfs_get_block_device(0) returned: 0x%lx", (unsigned long)((uint64_t)hdd));

    if (hdd) {
        klog(KLOG_INFO, SUBSYS_CORE, "Block device found, drawing framebuffer msg...");
        framebuffer_draw_string("Found block device 0", 10, y_pos, COLOR_GREEN, 0x00101828);
        klog(KLOG_INFO, SUBSYS_CORE, "Framebuffer msg drawn");
        y_pos += 20;

        klog(KLOG_INFO, SUBSYS_CORE, "About to call grahafs_mount...");
        vfs_node_t* root = grahafs_mount(hdd);
        klog(KLOG_INFO, SUBSYS_CORE, "grahafs_mount returned: 0x%lx", (unsigned long)((uint64_t)root));

        if (root) {
            klog(KLOG_INFO, SUBSYS_CORE, "Mount successful, drawing success msg...");
            framebuffer_draw_string("GrahaFS mounted successfully on /", 10, y_pos, COLOR_GREEN, 0x00101828);
            klog(KLOG_INFO, SUBSYS_CORE, "Success msg drawn");
            y_pos += 20;

            // Phase 15b: the FS is now live; attach the audit subsystem so
            // future entries go to /var/audit and the early-boot entries in
            // the in-memory queue get flushed to disk.
            audit_attach_fs();
        } else {
            klog(KLOG_ERROR, SUBSYS_CORE, "Mount failed, drawing error msgs...");
            framebuffer_draw_string("Failed to mount GrahaFS!", 10, y_pos, COLOR_RED, 0x00101828);
            framebuffer_draw_string("Disk may need formatting with mkfs.gfs", 10, y_pos + 20, COLOR_YELLOW, 0x00101828);
            klog(KLOG_ERROR, SUBSYS_CORE, "Error msgs drawn");
            y_pos += 40;
        }
    } else {
        klog(KLOG_ERROR, SUBSYS_CORE, "No block device found, drawing error msg...");
        framebuffer_draw_string("No block device found!", 10, y_pos, COLOR_RED, 0x00101828);
        klog(KLOG_ERROR, SUBSYS_CORE, "Error msg drawn");
        y_pos += 20;
    }

    // --- USER SPACE INITIALIZATION ---
    klog(KLOG_INFO, SUBSYS_CORE, "=== USER SPACE INITIALIZATION ===");
    framebuffer_draw_string("=== Loading Interactive Shell ===", 50, y_pos, COLOR_WHITE, 0x00101828);
    y_pos += 30;

    // Initialize Initial RAM Disk
    klog(KLOG_INFO, SUBSYS_CORE, "Initializing initrd...");
    initrd_init(&module_request);
    klog(KLOG_INFO, SUBSYS_CORE, "Initrd initialized");
    framebuffer_draw_string("Initrd initialized.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Phase 12: init-process selection. autorun_decide() returns either
    // "bin/gash" (default) or "bin/<name>" if the kernel command line
    // supplied autorun=<name>.
    const char *init_path = autorun_decide();
    klog(KLOG_INFO, SUBSYS_CORE, "Looking up %s in initrd...", init_path);
    size_t gash_size;
    void *gash_data = initrd_lookup(init_path, &gash_size);
    if (!gash_data) {
        klog(KLOG_FATAL, SUBSYS_CORE, "FATAL: Could not find init binary in initrd: %s", init_path);
        klog(KLOG_INFO, SUBSYS_CORE, "");
        framebuffer_draw_string("FATAL: Could not find init binary in initrd!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    klog(KLOG_INFO, SUBSYS_CORE, "Found init binary, size=%lu", (unsigned long)(gash_size));
    framebuffer_draw_string("Found init binary in initrd.", 50, y_pos, COLOR_GREEN, 0x00101828);
    y_pos += 20;

    // Load shell ELF binary
    klog(KLOG_INFO, SUBSYS_CORE, "About to call elf_load for init...");
    uint64_t entry_point, cr3;
    if (!elf_load(gash_data, &entry_point, &cr3)) {
        klog(KLOG_FATAL, SUBSYS_CORE, "FATAL: elf_load failed!");
        framebuffer_draw_string("FATAL: Failed to load shell ELF file!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    klog(KLOG_INFO, SUBSYS_CORE, "elf_load succeeded! entry_point=0x%lx cr3=0x%lx", (unsigned long)(entry_point), (unsigned long)(cr3));

    klog(KLOG_INFO, SUBSYS_CORE, "Drawing 'Shell loaded' message...");
    framebuffer_draw_string("Shell loaded successfully into memory.", 50, y_pos, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "Message drawn");
    y_pos += 20;

    // Create shell process
    klog(KLOG_INFO, SUBSYS_CORE, "Creating shell process...");
    int process_id = sched_create_user_process(entry_point, cr3);
    if (process_id < 0) {
        klog(KLOG_ERROR, SUBSYS_CORE, "ERROR: Failed to create shell process!");
        framebuffer_draw_string("FATAL: Failed to create shell process!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    klog(KLOG_INFO, SUBSYS_CORE, "Shell process created, ID=%lu", (unsigned long)(process_id));

    // Phase 12: remember the init PID so SYS_EXIT can trigger shutdown
    // when autorun is active.
    autorun_register_init_pid(process_id);

    // Set process name for the shell. Use the last '/'-separated
    // component of init_path so `autorun=ktest` shows up as "ktest".
    {
        task_t *shell_task = sched_get_task(process_id);
        if (shell_task) {
            const char *base = init_path;
            for (const char *p = init_path; *p; p++) {
                if (*p == '/') base = p + 1;
            }
            int j = 0;
            while (base[j] && j < 31) { shell_task->name[j] = base[j]; j++; }
            shell_task->name[j] = '\0';
        }
    }

    klog(KLOG_INFO, SUBSYS_CORE, "Drawing 'Shell process created' message...");
    framebuffer_draw_string("Shell process created.", 50, y_pos, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "Message drawn");
    y_pos += 20;

    // Initialize keyboard hardware BEFORE creating the polling task
    klog(KLOG_INFO, SUBSYS_CORE, "Initializing keyboard...");
    keyboard_init();
    klog(KLOG_INFO, SUBSYS_CORE, "Keyboard initialized");
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
    klog(KLOG_INFO, SUBSYS_CORE, "Creating keyboard task...");
    int kbd_task_id = sched_create_task(task_func);
    klog(KLOG_INFO, SUBSYS_CORE, "sched_create_task returned: %lu", (unsigned long)(kbd_task_id));

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
    klog(KLOG_INFO, SUBSYS_CORE, "Creating mongoose poll task...");
    int mg_task_id = sched_create_task(mongoose_poll_task);
    klog(KLOG_INFO, SUBSYS_CORE, "sched_create_task (mongoose) returned: %lu", (unsigned long)(mg_task_id));
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

    // Create background indexer task (Phase 11b)
    klog(KLOG_INFO, SUBSYS_CORE, "Creating indexer task...");
    int idx_task_id = sched_create_task(grahafs_indexer_task);
    klog(KLOG_INFO, SUBSYS_CORE, "sched_create_task (indexer) returned: %lu", (unsigned long)(idx_task_id));
    {
        task_t *idx_task = sched_get_task(idx_task_id);
        if (idx_task) {
            const char *n = "indexer";
            int j = 0;
            while (n[j] && j < 31) { idx_task->name[j] = n[j]; j++; }
            idx_task->name[j] = '\0';
        }
    }
    if (idx_task_id >= 0) {
        framebuffer_draw_string("Indexer task created successfully", 50, y_pos, COLOR_GREEN, 0x00101828);
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
    klog(KLOG_INFO, SUBSYS_CORE, "About to enable interrupts...");
    framebuffer_draw_string("Enabling interrupts...", 10, 50, COLOR_YELLOW, 0x00101828);
    asm volatile("sti");
    klog(KLOG_INFO, SUBSYS_CORE, "Interrupts enabled");

    // Wait to ensure no pending issues
    klog(KLOG_INFO, SUBSYS_CORE, "Waiting after STI...");
    for (volatile int i = 0; i < 1000000; i++) {
        asm volatile("pause");
    }
    klog(KLOG_INFO, SUBSYS_CORE, "Wait complete");

    // NOW start the timer on BSP only
    klog(KLOG_INFO, SUBSYS_CORE, "About to start scheduler timer...");
    framebuffer_draw_string("Starting scheduler timer on BSP...", 10, 70, COLOR_YELLOW, 0x00101828);
    
    // Disable interrupts briefly while starting timer
    asm volatile("cli");
    klog(KLOG_INFO, SUBSYS_CORE, "Calling lapic_timer_init...");
    lapic_timer_init(100, 32);
    klog(KLOG_INFO, SUBSYS_CORE, "lapic_timer_init returned");
    asm volatile("sti");
    klog(KLOG_INFO, SUBSYS_CORE, "Re-enabled interrupts after timer init");

    if (!lapic_timer_is_running()) {
        klog(KLOG_ERROR, SUBSYS_CORE, "ERROR: Timer failed to start!");
        framebuffer_draw_string("ERROR: Timer failed to start!", 10, 90, COLOR_RED, 0x00101828);
    } else {
        klog(KLOG_INFO, SUBSYS_CORE, "Timer is running, system initialized!");
        framebuffer_draw_string("System running!", 10, 90, COLOR_GREEN, 0x00101828);
    }
    
   
    // OPTIONAL: Start timers on APs later (commented out for now)
    // This would require IPI (Inter-Processor Interrupts) to signal APs
    // For now, only BSP handles scheduling

    // Register service and application capabilities
    klog(KLOG_INFO, SUBSYS_CORE, "Registering service/application capabilities...");
    {
        const char *sched_deps[] = {"timer", "memory"};
        cap_register("scheduler", CAP_SERVICE, 0, -1, sched_deps, 2,
                     NULL, NULL, NULL, 0, NULL);

        const char *shell_deps[] = {"display", "keyboard_input", "filesystem"};
        cap_register("shell", CAP_APPLICATION, 0, -1, shell_deps, 3,
                     NULL, NULL, NULL, 0, NULL);
    }

    // Activate all registered capabilities. Phase 16 wires real callbacks into
    // keyboard/fb/e1000/ahci: these do MMIO + PIC writes, which interleaved
    // with klog-to-serial pushes the loop past the first timer tick. Disable
    // interrupts for the duration so we activate every cap atomically before
    // scheduling kicks in. The caps themselves don't rely on interrupts.
    klog(KLOG_INFO, SUBSYS_CORE, "Activating all capabilities...");
    asm volatile("cli");
    for (int i = 0; i < cap_get_count(); i++) {
        cap_activate(i);
    }
    asm volatile("sti");
    klog(KLOG_INFO, SUBSYS_CORE, "All capabilities activated");

    klog(KLOG_INFO, SUBSYS_CORE, "\n=== ENTERING IDLE LOOP ===\nSystem fully initialized, waiting for timer interrupts...\n");

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