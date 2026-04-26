//arch/x86_64/cpu/sched/sched.c
#include "sched.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../gdt.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/sync/spinlock.h"
#include "../../drivers/serial/serial.h"
#include "../../../../kernel/elf.h"
#include "../../../../kernel/initrd.h"
#include "../../../../kernel/fs/pipe.h"
#include "../../../../kernel/panic.h"
#include "../../../../kernel/log.h"
#include "../../../../kernel/mm/slab.h"
#include "../../../../kernel/cap/handle_table.h"
#include "../../../../kernel/cap/object.h"
#include "../../../../kernel/cap/deprecated.h"
#include "../../../../kernel/mm/vmo.h"
#include "../../../../kernel/pid_hash.h"
#include "../../../../kernel/resource/rlimit.h"
#include "../../../../kernel/audit.h"
#include "../tsc.h"
#include "work_steal.h"

// Phase 14: task_t objects now live in task_cache (Bonwick slab). The
// scheduler still keeps a fixed-size index array for stable task ids
// and O(1) slot lookup. task_ptrs[i] is NULL when slot i is free, or
// points to a slab-allocated task_t when occupied.
static task_t *task_ptrs[MAX_TASKS];
static kmem_cache_t *task_cache = NULL;
static int next_task_id = 0;
static int current_task_index = 0;

// Scheduler spinlock with static initialization
spinlock_t sched_lock = SPINLOCK_INITIALIZER("scheduler");

// Debug counters (for post-mortem analysis only)
volatile uint32_t schedule_count = 0;
volatile uint32_t context_switches = 0;

// Simple memset implementation
static void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n-- > 0) {
        *p++ = (uint8_t)c;
    }
    return s;
}

// Phase 20: create a per-AP idle task. Each AP needs a distinct idle
// task (separate task_t + separate kernel stack) so that schedule() can
// save the AP's interrupt frame into a task other than the BSP idle.
// Called from kmain (BSP context) for each active AP before APs are
// released via g_ap_scheduler_go. Returns the task_t* or NULL on failure.
//
// The idle task's entry is ap_idle_entry — a simple `while (1) hlt` loop.
// It never appears on the runq; schedule() dispatches it by falling
// through when both the local runq and work-stealing came up empty.
static void ap_idle_entry(void) {
    while (1) {
        asm volatile("sti; hlt");
    }
}

task_t *sched_create_idle_for_cpu(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return NULL;
    if (!task_cache) return NULL;

    spinlock_acquire(&sched_lock);
    if (next_task_id >= MAX_TASKS) {
        spinlock_release(&sched_lock);
        return NULL;
    }
    int id = next_task_id++;

    task_t *t = kmem_cache_alloc(task_cache);
    if (!t) {
        next_task_id--;
        spinlock_release(&sched_lock);
        return NULL;
    }
    task_ptrs[id] = t;

    t->id = id;
    t->state = TASK_STATE_RUNNING;  // about to run on target CPU
    t->parent_id = 0;
    t->waiting_for_child = -1;
    t->pgid = 0;
    t->pending_signals = 0;
    t->name[0] = 'i'; t->name[1] = 'd'; t->name[2] = 'l'; t->name[3] = 'e';
    t->name[4] = '0' + (cpu_id % 10); t->name[5] = '\0';
    for (int i = 0; i < MAX_SIGNALS; i++) t->signal_handlers[i] = SIG_DFL;
    t->event_head = 0; t->event_tail = 0; t->event_count = 0; t->event_waiting = 0;
    for (int f = 0; f < PROC_MAX_FDS; f++) {
        t->fd_table[f].type = FD_TYPE_UNUSED;
        t->fd_table[f].ref = -1;
        t->fd_table[f].flags = 0;
    }
    (void)cap_handle_table_init(&t->cap_handles);
    pledge_init(t, (pledge_mask_t){.raw = PLEDGE_ALL});
    rlimit_init_defaults(t, NULL);
    t->is_idle = true;
    t->cpu_pinned = (int32_t)cpu_id;
    t->last_ran_cpu = cpu_id;
    t->cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());

    pid_hash_insert(t);
    g_task_count++;
    spinlock_release(&sched_lock);

    // Allocate a kernel stack for this idle task (outside sched_lock).
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void *kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) {
        // Best-effort cleanup: mark zombie and return NULL.
        spinlock_acquire(&sched_lock);
        t->state = TASK_STATE_ZOMBIE;
        spinlock_release(&sched_lock);
        return NULL;
    }
    uint64_t kstack_virt_base = (uint64_t)kstack_phys + g_hhdm_offset;
    uint64_t kstack_top = kstack_virt_base + KERNEL_STACK_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        vmm_map_page(vmm_get_kernel_space(),
                     kstack_virt_base + i * PAGE_SIZE,
                     (uint64_t)kstack_phys + i * PAGE_SIZE,
                     PTE_PRESENT | PTE_WRITABLE);
    }

    spinlock_acquire(&sched_lock);
    t->kernel_stack_top = kstack_top;
    for (size_t i = 0; i < sizeof(t->regs); i++) ((uint8_t *)&t->regs)[i] = 0;
    t->regs.rip = (uint64_t)ap_idle_entry;
    t->regs.cs = 0x08;
    t->regs.ss = 0x10;
    t->regs.ds = 0x10;
    t->regs.rflags = 0x202;
    t->regs.rsp = ((kstack_top - 128) & ~0xFULL) - 8;
    t->regs.rbp = t->regs.rsp;

    // Install as this CPU's "current" so its first schedule() finds a
    // valid outgoing task to save regs into. Also set idle_task so the
    // schedule() fallback (runq empty + work-stealing failed) routes
    // here instead of racing on task_ptrs[0] (BSP's idle).
    g_cpu_locals[cpu_id].runq.current = t;
    g_cpu_locals[cpu_id].runq.idle_task = t;
    g_cpu_locals[cpu_id].tss.rsp0 = kstack_top;
    spinlock_release(&sched_lock);

    klog(KLOG_INFO, SUBSYS_SCHED,
         "[SCHED] Per-CPU idle task id=%d created for CPU %u", id, cpu_id);
    return t;
}

// Phase 20: place a READY task onto a per-CPU runqueue. Central helper used
// by every state → READY transition (create + wake paths + per-tick deadline
// scan). Routes based on:
//   - cpu_pinned: if >= 0 and valid, respected absolutely (per-CPU idle tasks
//     and epoch task end up here).
//   - last_ran_cpu: cache-affinity preference for tasks that have run before.
//   - default: CPU 0. Work-stealing will redistribute if imbalance develops.
void sched_enqueue_ready(task_t *task) {
    if (!task) return;
    uint32_t target_cpu = 0;
    if (task->cpu_pinned >= 0 && (uint32_t)task->cpu_pinned < g_cpu_count) {
        target_cpu = (uint32_t)task->cpu_pinned;
    } else if (task->last_ran_cpu != 0xFFFFFFFFu && task->last_ran_cpu < g_cpu_count) {
        target_cpu = task->last_ran_cpu;
    }
    runq_t *rq = &g_cpu_locals[target_cpu].runq;
    spinlock_acquire(&rq->lock);
    runq_enqueue_ready(rq, task);
    spinlock_release(&rq->lock);
}

void sched_init(void) {
    // Initialize the scheduler lock
    spinlock_init(&sched_lock, "scheduler");

    // Phase 14: lazy-create the task_t slab cache the first time
    // sched_init runs. Requires kheap/slab to already be initialised
    // (main.c runs kmem_slab_init + kheap_init before sched_init).
    if (!task_cache) {
        task_cache = kmem_cache_create("task_t",
                                       sizeof(task_t),
                                       _Alignof(task_t),
                                       /*ctor=*/NULL,
                                       SUBSYS_SCHED);
        if (!task_cache) {
            kpanic("sched_init: kmem_cache_create(task_cache) failed");
        }
    }

    spinlock_acquire(&sched_lock);

    // All slots start empty.
    for (int i = 0; i < MAX_TASKS; ++i) task_ptrs[i] = NULL;

    // Allocate task 0 (kernel idle task) from the slab.
    task_ptrs[0] = kmem_cache_alloc(task_cache);
    if (!task_ptrs[0]) {
        spinlock_release(&sched_lock);
        kpanic("sched_init: task_cache alloc failed for task 0");
    }

    // Task 0 is the kernel's idle task
    (*task_ptrs[0]).id = next_task_id++;
    (*task_ptrs[0]).state = TASK_STATE_RUNNING;
    (*task_ptrs[0]).cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    (*task_ptrs[0]).parent_id = -1;
    (*task_ptrs[0]).waiting_for_child = -1;
    (*task_ptrs[0]).pgid = 0;
    (*task_ptrs[0]).pending_signals = 0;
    (*task_ptrs[0]).name[0] = 'k'; (*task_ptrs[0]).name[1] = 'e'; (*task_ptrs[0]).name[2] = 'r';
    (*task_ptrs[0]).name[3] = 'n'; (*task_ptrs[0]).name[4] = 'e'; (*task_ptrs[0]).name[5] = 'l';
    (*task_ptrs[0]).name[6] = '\0';
    for (int i = 0; i < MAX_SIGNALS; i++) {
        (*task_ptrs[0]).signal_handlers[i] = SIG_DFL;
    }
    (*task_ptrs[0]).event_head = 0;
    (*task_ptrs[0]).event_tail = 0;
    (*task_ptrs[0]).event_count = 0;
    (*task_ptrs[0]).event_waiting = 0;

    // Phase 15a: initialize the handle table for task 0 (kernel).
    // Non-fatal if this fails — task 0 rarely makes cap syscalls.
    (void)cap_handle_table_init(&(*task_ptrs[0]).cap_handles);

    // Phase 15b: task 0 gets the full pledge (kernel idle has full authority,
    // though it does no syscalls). Child tasks inherit via pledge_init below.
    pledge_init(task_ptrs[0], (pledge_mask_t){.raw = PLEDGE_ALL});

    // Phase 20: seed resource-limit defaults (kernel-internal tasks get
    // unlimited everything) and register with the PID hash / global list.
    rlimit_init_defaults(task_ptrs[0], NULL);
    task_ptrs[0]->is_idle = true;         // BSP's idle
    task_ptrs[0]->cpu_pinned = 0;         // stays on CPU 0
    task_ptrs[0]->last_ran_cpu = 0;
    pid_hash_insert(task_ptrs[0]);
    g_task_count++;

    // Seed this CPU's runq.current so schedule() has a valid outgoing
    // task on the very first tick. Also set idle_task — the schedule()
    // fallback path uses this to avoid racing two CPUs on task_ptrs[0]
    // when the local runq is empty + work-stealing fails.
    g_cpu_locals[0].runq.current = task_ptrs[0];
    g_cpu_locals[0].runq.idle_task = task_ptrs[0];

    // Phase 10a: Kernel idle task has no FDs
    for (int f = 0; f < PROC_MAX_FDS; f++) {
        (*task_ptrs[0]).fd_table[f].type = FD_TYPE_UNUSED;
        (*task_ptrs[0]).fd_table[f].ref = -1;
        (*task_ptrs[0]).fd_table[f].flags = 0;
    }

    // Get current stack pointer
    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    
    // Set kernel stack for idle task
    (*task_ptrs[0]).kernel_stack_top = (current_rsp & ~0xFFF) + 0x4000;
    
    // Initialize the interrupt frame for idle task
    memset(&(*task_ptrs[0]).regs, 0, sizeof(struct interrupt_frame));
    (*task_ptrs[0]).regs.cs = 0x08;      // Kernel code segment
    (*task_ptrs[0]).regs.ss = 0x10;      // Kernel data segment  
    (*task_ptrs[0]).regs.ds = 0x10;      // Data segment
    (*task_ptrs[0]).regs.rflags = 0x202; // IF=1, Reserved=1
    (*task_ptrs[0]).regs.rsp = current_rsp;
    (*task_ptrs[0]).regs.rbp = current_rsp;
    
    // Update the per-CPU TSS
    uint32_t cpu_id = smp_get_current_cpu();
    g_cpu_locals[cpu_id].tss.rsp0 = (*task_ptrs[0]).kernel_stack_top;
    
    current_task_index = 0;
    
    spinlock_release(&sched_lock);
    
    // Safe to print here - before interrupts are enabled
    framebuffer_draw_string("Scheduler initialized with interrupt-safe locks", 700, 20, COLOR_GREEN, 0x00101828);
}

int sched_create_task(void (*entry_point)(void)) {
    // CRITICAL: Check the actual parameter value, not what debugger shows
    volatile uint64_t entry_addr = (uint64_t)entry_point;
    
    // Validate entry point
    if (!entry_point || entry_addr == 0) {
        // Try to get it from RDI directly in case of calling convention issue
        uint64_t rdi_value;
        asm volatile("mov %%rdi, %0" : "=r"(rdi_value));
        
        if (rdi_value >= 0xFFFFFFFF80000000 && rdi_value != 0) {
            // Use the RDI value instead
            entry_point = (void (*)(void))rdi_value;
            entry_addr = rdi_value;
        } else {
            // Really is NULL
            return -1;
        }
    }
    
    // Double-check it's in kernel space
    if (entry_addr < 0xFFFFFFFF80000000) {
        return -1;
    }
    
    spinlock_acquire(&sched_lock);

    if (next_task_id >= MAX_TASKS) {
        spinlock_release(&sched_lock);
        return -1;
    }

    int id = next_task_id++;
    // Phase 14: allocate task_t from the slab. The cache was created
    // by sched_init; its existence is an invariant here.
    task_ptrs[id] = kmem_cache_alloc(task_cache);
    if (!task_ptrs[id]) {
        next_task_id--;  // Roll back id allocation on alloc failure.
        spinlock_release(&sched_lock);
        klog(KLOG_ERROR, SUBSYS_SCHED,
             "sched_create_task: task_cache alloc failed");
        return -1;
    }
    (*task_ptrs[id]).id = id;
    (*task_ptrs[id]).state = TASK_STATE_BLOCKED;  // BLOCKED until fully initialized
    (*task_ptrs[id]).parent_id = current_task_index >= 0 ? (*task_ptrs[current_task_index]).id : -1;
    (*task_ptrs[id]).waiting_for_child = -1;
    (*task_ptrs[id]).pgid = current_task_index >= 0 ? (*task_ptrs[current_task_index]).pgid : 0;
    (*task_ptrs[id]).pending_signals = 0;
    (*task_ptrs[id]).name[0] = '\0';
    for (int s = 0; s < MAX_SIGNALS; s++) {
        (*task_ptrs[id]).signal_handlers[s] = SIG_DFL;
    }
    (*task_ptrs[id]).event_head = 0;
    (*task_ptrs[id]).event_tail = 0;
    (*task_ptrs[id]).event_count = 0;
    (*task_ptrs[id]).event_waiting = 0;

    // Phase 10a: Kernel tasks have no FDs
    for (int f = 0; f < PROC_MAX_FDS; f++) {
        (*task_ptrs[id]).fd_table[f].type = FD_TYPE_UNUSED;
        (*task_ptrs[id]).fd_table[f].ref = -1;
        (*task_ptrs[id]).fd_table[f].flags = 0;
    }

    // Phase 15a: initialize per-process cap handle table. Must happen while
    // the task_t memory is zeroed (slab NULL-ctor guarantees this).
    (void)cap_handle_table_init(&(*task_ptrs[id]).cap_handles);

    // Phase 15b: kernel threads default to PLEDGE_ALL. They have full
    // authority; no need to narrow (flusher, keyboard, indexer etc. touch
    // every subsystem).
    pledge_init(task_ptrs[id], (pledge_mask_t){.raw = PLEDGE_ALL});

    // Phase 20: unlimited resource defaults for kernel threads; register
    // with the PID hash + global list so psinfo and epoch tick see them.
    // Kernel threads are pinned to CPU 0 — Mongoose, audit flusher, GC,
    // recluster, stream worker etc. were never designed for SMP migration
    // and their internal state assumes single-CPU execution. User
    // processes (sched_create_user_process) keep cpu_pinned = -1 so
    // work-stealing can redistribute them.
    rlimit_init_defaults(task_ptrs[id], NULL);
    task_ptrs[id]->cpu_pinned = 0;
    pid_hash_insert(task_ptrs[id]);
    g_task_count++;

    spinlock_release(&sched_lock);

    // Allocate kernel stack (outside lock to avoid holding lock during allocation)
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) {
        // Mark task slot as unusable
        spinlock_acquire(&sched_lock);
        (*task_ptrs[id]).state = TASK_STATE_ZOMBIE;
        spinlock_release(&sched_lock);
        return -1;
    }

    // Calculate virtual address
    uint64_t kstack_virt_base = (uint64_t)kstack_phys + g_hhdm_offset;
    uint64_t kstack_top = kstack_virt_base + KERNEL_STACK_SIZE;

    // Map kernel stack pages
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = kstack_virt_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)kstack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }

    spinlock_acquire(&sched_lock);

    // Set stack
    (*task_ptrs[id]).kernel_stack_top = kstack_top;

    // Initialize the interrupt frame
    memset(&(*task_ptrs[id]).regs, 0, sizeof(struct interrupt_frame));

    // Set up initial context
    (*task_ptrs[id]).regs.rip = entry_addr;  // Use validated address
    (*task_ptrs[id]).regs.cs = 0x08;         // Kernel code segment
    (*task_ptrs[id]).regs.ss = 0x10;         // Kernel data segment
    (*task_ptrs[id]).regs.ds = 0x10;         // Data segment
    (*task_ptrs[id]).regs.rflags = 0x202;    // IF=1, Reserved=1
    // ABI: at function entry, (RSP+8) must be 16-byte aligned (RSP%16==8)
    // because normally a `call` pushes an 8-byte return address.
    // Since the scheduler jumps directly without `call`, we subtract 8.
    (*task_ptrs[id]).regs.rsp = ((kstack_top - 128) & ~0xF) - 8;
    (*task_ptrs[id]).regs.rbp = (*task_ptrs[id]).regs.rsp;         // Base pointer

    // Use kernel address space
    (*task_ptrs[id]).cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());

    // NOW mark as READY - fully initialized, safe for scheduler
    (*task_ptrs[id]).state = TASK_STATE_READY;

    spinlock_release(&sched_lock);

    // Phase 20: runq insertion outside sched_lock (lock hierarchy).
    sched_enqueue_ready(task_ptrs[id]);

    return id;
}

int sched_create_user_process(uint64_t rip, uint64_t cr3) {
    spinlock_acquire(&sched_lock);
    int id;
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));
    if (next_task_id >= MAX_TASKS) {
        asm volatile("push %0; popfq" : : "r"(flags));
        spinlock_release(&sched_lock);
        return -1;
    }
    // Phase 20 U15: live-task soft cap. Callers see -EAGAIN (-11) which the
    // user-libc translates to EAGAIN. Past the cap the system is still
    // functioning — it just refuses new spawns until some task exits.
    if (g_task_count >= RLIMIT_MAX_TASKS) {
        asm volatile("push %0; popfq" : : "r"(flags));
        spinlock_release(&sched_lock);
        return -11;  // -EAGAIN
    }
    id = next_task_id++;

    // Phase 14: allocate task_t from the slab.
    task_ptrs[id] = kmem_cache_alloc(task_cache);
    if (!task_ptrs[id]) {
        next_task_id--;
        asm volatile("push %0; popfq" : : "r"(flags));
        spinlock_release(&sched_lock);
        klog(KLOG_ERROR, SUBSYS_SCHED,
             "sched_create_user_process: task_cache alloc failed");
        return -1;
    }
    (*task_ptrs[id]).id = id;
    (*task_ptrs[id]).state = TASK_STATE_BLOCKED;  // BLOCKED until fully initialized
    (*task_ptrs[id]).cr3 = cr3;
    (*task_ptrs[id]).parent_id = (*task_ptrs[current_task_index]).id;
    (*task_ptrs[id]).waiting_for_child = -1;
    (*task_ptrs[id]).exit_status = 0;
    (*task_ptrs[id]).pgid = (*task_ptrs[current_task_index]).pgid; // Inherit parent's process group
    (*task_ptrs[id]).pending_signals = 0;
    (*task_ptrs[id]).name[0] = '\0';
    for (int s = 0; s < MAX_SIGNALS; s++) {
        (*task_ptrs[id]).signal_handlers[s] = SIG_DFL;
    }
    (*task_ptrs[id]).event_head = 0;
    (*task_ptrs[id]).event_tail = 0;
    (*task_ptrs[id]).event_count = 0;
    (*task_ptrs[id]).event_waiting = 0;

    // Phase 10a: Initialize per-process FD table with stdin/stdout/stderr
    for (int f = 0; f < PROC_MAX_FDS; f++) {
        (*task_ptrs[id]).fd_table[f].type = FD_TYPE_UNUSED;
        (*task_ptrs[id]).fd_table[f].ref = -1;
        (*task_ptrs[id]).fd_table[f].flags = 0;
    }
    (*task_ptrs[id]).fd_table[0] = (proc_fd_t){FD_TYPE_CONSOLE, 0, 0}; // stdin
    (*task_ptrs[id]).fd_table[1] = (proc_fd_t){FD_TYPE_CONSOLE, 0, 0}; // stdout
    (*task_ptrs[id]).fd_table[2] = (proc_fd_t){FD_TYPE_CONSOLE, 0, 0}; // stderr

    // Phase 15a: init handle table. Bootstrap caps are PUBLIC, so child
    // doesn't need inherited entries — cap_token_resolve bypasses audience
    // for PUBLIC objects. Non-PUBLIC user-derived handles must be explicitly
    // granted post-spawn (Phase 17 channels).
    (void)cap_handle_table_init(&(*task_ptrs[id]).cap_handles);

    // Phase 15b: default user-process pledge is PLEDGE_ALL. sched_spawn_process
    // will override with parent_mask & attrs.pledge_subset; direct callers of
    // sched_create_user_process (notably the initial user bootstrap) get ALL.
    pledge_init(task_ptrs[id], (pledge_mask_t){.raw = PLEDGE_ALL});

    // Phase 20: inherit parent's resource limits (if any parent).
    // Use sched_get_current_task() (per-CPU runq.current) instead of the
    // global current_task_index, which is shared across CPUs and gives
    // wrong inheritance under SMP. The bootstrap user process has no
    // parent yet — sched_get_current_task may return NULL pre-scheduler,
    // which yields default unlimited limits.
    task_t *parent_for_limits = sched_get_current_task();
    rlimit_init_defaults(task_ptrs[id], parent_for_limits);
    pid_hash_insert(task_ptrs[id]);
    g_task_count++;

    asm volatile("push %0; popfq" : : "r"(flags));
    spinlock_release(&sched_lock);

    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) return -1;
    (*task_ptrs[id]).kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;
    
    uint64_t kstack_base = (*task_ptrs[id]).kernel_stack_top - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = kstack_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)kstack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }
    
    // Allocate user stack: 64 pages = 256KB
    #define USER_STACK_PAGES 64
    uint64_t user_stack_top = 0x7FFFFFFFF000;
    uint64_t user_stack_base = user_stack_top - (USER_STACK_PAGES * PAGE_SIZE);

    vmm_address_space_t* proc_space = NULL;
    for(int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (vmm_get_pml4_phys(&address_space_pool[i]) == cr3) {
            proc_space = &address_space_pool[i];
            break;
        }
    }
    if (proc_space == NULL) {
        pmm_free_pages(kstack_phys, num_pages);
        return -1;
    }

    uint64_t flags_map = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        void* user_stack_phys = pmm_alloc_page();
        if (!user_stack_phys) {
            // Cleanup: free already-mapped stack pages and kernel stack
            // The address space cleanup will handle mapped pages on process destroy
            pmm_free_pages(kstack_phys, num_pages);
            return -1;
        }
        // Clear the page via HHDM
        uint8_t *page_virt = (uint8_t *)((uint64_t)user_stack_phys + g_hhdm_offset);
        for (int j = 0; j < PAGE_SIZE; j++) page_virt[j] = 0;

        uint64_t page_vaddr = user_stack_base + (i * PAGE_SIZE);
        if (!vmm_map_page(proc_space, page_vaddr, (uint64_t)user_stack_phys, flags_map)) {
            pmm_free_page(user_stack_phys);
            pmm_free_pages(kstack_phys, num_pages);
            return -1;
        }
    }
    
    // Safe to print here - not in interrupt context
    framebuffer_draw_string("User process created successfully!", 700, 120, COLOR_GREEN, 0x00101828);

    spinlock_acquire(&sched_lock);
    
    memset(&(*task_ptrs[id]).regs, 0, sizeof(struct interrupt_frame));
    (*task_ptrs[id]).regs.rip = rip;
    (*task_ptrs[id]).regs.rflags = 0x202;
    // System V ABI: at function entry, RSP%16 must be 8 (because a `call`
    // would have pushed a return address). _start has no caller, so the
    // kernel must simulate the post-`call` alignment. user_stack_top is
    // page-aligned (%16==0), so subtracting 8 gives %16==8 at entry.
    // GCC's prologue (`sub $N, %rsp` with N%16==8 for typical N) then
    // brings RSP%16 back to 0 inside the function, which is what SSE
    // movaps/movdqa instructions require for 16-byte-aligned local stack
    // slots. Pre-Phase-20-rlimittest `- 16` made RSP%16==0 at entry, which
    // GCC then offset to %16==8 — SSE locals at fixed offsets crashed.
    (*task_ptrs[id]).regs.rsp = user_stack_top - 8;
    (*task_ptrs[id]).regs.cs = 0x20 | 3;
    (*task_ptrs[id]).regs.ss = 0x18 | 3;

    // Phase 7c: Initialize heap management fields
    // Heap starts at 4GB (0x100000000) in user space
    // This is well above typical code/data sections and below stack
    (*task_ptrs[id]).heap_start = 0x100000000ULL;
    (*task_ptrs[id]).brk = (*task_ptrs[id]).heap_start;  // Initially empty heap
    (*task_ptrs[id]).stack_top = user_stack_top;   // Top of stack for collision detection

    // NOW mark as READY - fully initialized, safe for scheduler
    (*task_ptrs[id]).state = TASK_STATE_READY;

    spinlock_release(&sched_lock);

    // Phase 20: enqueue on a per-CPU runq outside sched_lock.
    sched_enqueue_ready(task_ptrs[id]);

    return id;
}

void wake_waiting_parent(int child_id) {
    // NO FRAMEBUFFER CALLS - might be called from interrupt context
    //
    // SMP-safe: don't dereference task_ptrs[child_id]. The caller (SYS_EXIT
    // / PF / exception kill paths) already set the child's state to ZOMBIE
    // before us, which means a parent on another CPU may have already raced
    // through SYS_WAIT, called sched_reap_zombie, and zeroed task_ptrs[child_id].
    // Use the child's parent_id captured at task-creation time, not via a
    // re-read. Pass it through a separate snapshot below.
    if (child_id < 0 || child_id >= next_task_id || child_id >= MAX_TASKS) {
        return;
    }

    // Snapshot the task pointer once. If it's already NULL (reaped), the
    // parent already has the zombie info — nothing left for us to wake.
    task_t *child = task_ptrs[child_id];
    if (!child) return;

    int parent_id = child->parent_id;

    if (parent_id < 0 || parent_id >= next_task_id) {
        return;
    }

    task_t *parent = task_ptrs[parent_id];
    if (!parent) return;

    // Check if parent is waiting for this child or any child
    if (parent->state == TASK_STATE_BLOCKED &&
        (parent->waiting_for_child == child_id ||
         parent->waiting_for_child == -1)) {

        // Wake up the parent
        parent->state = TASK_STATE_READY;
        parent->waiting_for_child = -1;
        // Phase 20: state → READY must be paired with runq insertion.
        sched_enqueue_ready(parent);
    }
}

// CRITICAL: This function is called from interrupt context
// NO LOCKS THAT CAN BE HELD BY NORMAL CODE!
// NO FRAMEBUFFER CALLS!
//
// Phase 20 rewrite: dispatch is now runq-driven instead of task_ptrs[] round-
// robin. Every READY task sits on some per-CPU runq (for now, CPU 0's — U14
// brings APs online). schedule() pops the local runq's head; if empty, U8's
// work-stealer scans peer runqs. Falls through to the per-CPU idle task
// (task 0 on BSP; per-AP idle in U14) when no work is available.
//
// The ad-hoc silent-timeout sched_lock acquire is gone — we use the standard
// budgeted spinlock_acquire, which panics with a structured oops on timeout.
// Any lock-held-too-long bug now surfaces as a parseable ==OOPS== frame.
void schedule(struct interrupt_frame *frame) {
    schedule_count++;

    if (!frame) { asm volatile("cli; hlt"); while (1); }
    uint64_t frame_addr = (uint64_t)frame;
    if (frame_addr < 0xFFFF800000000000) { asm volatile("cli; hlt"); while (1); }

    if (next_task_id == 0) return;  // scheduler not yet initialised

    uint64_t cpu_id = smp_get_current_cpu();

    // Reentrancy guard: if this CPU is already inside schedule() (e.g.,
    // timer IRQ fired while we were doing a context switch), bail.
    if (sched_lock.locked && sched_lock.owner == cpu_id) {
        return;
    }

    // Phase 20 SMP fix: deadline scan is done by BSP only. APs skip it —
    // any expired CHAN_WAIT task gets re-enqueued by BSP onto its
    // last_ran_cpu's runq (sched_enqueue_ready routes by cpu_pinned/
    // last_ran_cpu). Without this, all 4 CPUs serialize through sched_lock
    // every tick which causes 100ms+ holds and SPINLOCK_PANIC under load.
    if (cpu_id == 0) {
        spinlock_acquire(&sched_lock);
        for (int i = 0; i < next_task_id; i++) {
            if (!task_ptrs[i]) continue;
            task_t *tk = task_ptrs[i];
            if (tk->state != TASK_STATE_CHAN_WAIT) continue;
            if (tk->deadline_tsc == 0) continue;
            if (g_timer_ticks < tk->deadline_tsc) continue;
            tk->wait_result = -110;  // -ETIMEDOUT
            tk->state = TASK_STATE_READY;
            sched_enqueue_ready(tk);
        }
        spinlock_release(&sched_lock);
    }

    // Phase 20: per-CPU runq.current is the authoritative "running task on
    // this CPU". BSP pre-AP-release used global current_task_index; on APs
    // we always have a per-CPU idle task pre-installed.
    runq_t *rq = &g_cpu_locals[cpu_id].runq;
    task_t *cur = rq->current;
    if (!cur) {
        // First dispatch on this CPU. Fall through to BSP's idle task
        // (only happens on BSP before AP-idle tasks are created).
        cur = task_ptrs[current_task_index];
    }

    // Save outgoing regs + handle CPU-budget bookkeeping. No global lock
    // needed: cur is THIS CPU's task, no other CPU touches its regs (work-
    // stealing only takes READY tasks off the ready list, never RUNNING).
    bool starved = false;
    bool need_audit_starvation = false;
    uint64_t starvation_used_ns = 0;
    int32_t starvation_pid = -1;
    uint64_t starvation_budget = 0;
    if (cur) {
        cur->regs = *frame;
        if (cur->state == TASK_STATE_RUNNING) {
            if (!cur->is_idle && cur->cpu_time_slice_budget_ns > 0 &&
                cur->last_ran_tsc != 0 && tsc_is_ready()) {
                uint64_t now_tsc = rdtsc();
                uint64_t elapsed_ns = tsc_to_ns(now_tsc - cur->last_ran_tsc);
                int64_t remaining = rlimit_consume_cpu(cur, elapsed_ns);
                if (remaining <= 0) {
                    starved = true;
                    uint64_t epoch_now = g_timer_ticks / 100u;
                    if (cur->last_starvation_epoch != epoch_now) {
                        cur->last_starvation_epoch = epoch_now;
                        starvation_used_ns = cur->cpu_time_slice_budget_ns;
                        if (cur->cpu_budget_remaining_ns < 0) {
                            starvation_used_ns += (uint64_t)(-cur->cpu_budget_remaining_ns);
                        }
                        starvation_pid = (int32_t)cur->id;
                        starvation_budget = cur->cpu_time_slice_budget_ns;
                        need_audit_starvation = true;
                    }
                }
            }
            if (starved) {
                spinlock_acquire(&rq->lock);
                runq_push_starved(rq, cur);
                spinlock_release(&rq->lock);
            } else {
                cur->state = TASK_STATE_READY;
                if (!cur->is_idle) {
                    sched_enqueue_ready(cur);
                }
            }
            rlimit_refill_io_tokens(cur);
        }
    }

    // Pick next task from this CPU's runq under rq->lock. If empty,
    // attempt a work-steal (uses trylock — never blocks). Falls through
    // to per-CPU idle if both fail.
    spinlock_acquire(&rq->lock);
    task_t *next = runq_dequeue_ready(rq);
    if (!next) {
        if (sched_steal_from_busiest(rq) > 0) {
            next = runq_dequeue_ready(rq);
        }
    }
    spinlock_release(&rq->lock);

    if (!next) {
        next = rq->idle_task;
        if (!next) next = task_ptrs[0];  // pre-init bootstrap fallback only
        if (!next || next->state == TASK_STATE_ZOMBIE) {
            kpanic("no runnable tasks");
        }
    }

    next->state = TASK_STATE_RUNNING;
    rq->current = next;
    rq->context_switches++;
    next->last_ran_cpu = (uint32_t)cpu_id;
    next->last_ran_tsc = rdtsc();
    context_switches++;

    // Best-effort legacy field (single-CPU code paths that haven't migrated
    // to sched_get_current_task yet). Racy under SMP, used only as a fallback.
    if (cpu_id == 0) current_task_index = next->id;

    // Phase 7d: signal delivery. If it terminates the task, pull a
    // replacement off the runq.
    if (sched_deliver_signals(next)) {
        spinlock_acquire(&rq->lock);
        task_t *replacement = runq_dequeue_ready(rq);
        spinlock_release(&rq->lock);
        if (!replacement) {
            replacement = rq->idle_task;
            if (!replacement) replacement = task_ptrs[0];
        }
        if (!replacement || replacement->state == TASK_STATE_ZOMBIE) {
            kpanic("no runnable tasks post-signal");
        }
        if (cpu_id == 0) current_task_index = replacement->id;
        replacement->state = TASK_STATE_RUNNING;
        rq->current = replacement;
        next = replacement;
    }

    // TSS rsp0 + CR3 switch — outside any global lock.
    g_cpu_locals[cpu_id].tss.rsp0 = next->kernel_stack_top;

    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != next->cr3) {
        vmm_switch_address_space_phys(next->cr3);
    }

    *frame = next->regs;

    // Audit starvation OUTSIDE the hot path — audit_write_rlimit_cpu can
    // do disk I/O and would otherwise stall the dispatcher.
    if (need_audit_starvation) {
        audit_write_rlimit_cpu(starvation_pid, starvation_budget,
                               starvation_used_ns);
    }
}

// These functions are safe - not called from interrupt context
task_t* sched_get_current_task(void) {
    // Phase 20: per-CPU aware. runq.current is the authoritative "what is
    // this CPU running right now", which is correct even after work-stealing
    // has migrated tasks across CPUs. Falls back to the old task_ptrs[]
    // lookup for the pre-U14 BSP-only path (runq.current can be NULL in
    // very early boot before the first dispatch).
    uint32_t cpu = smp_get_current_cpu();
    task_t *rq_current = g_cpu_locals[cpu].runq.current;
    if (rq_current) return rq_current;

    // Fallback: pre-scheduler boot or a CPU that hasn't dispatched yet.
    if (current_task_index >= MAX_TASKS || current_task_index >= next_task_id) {
        return NULL;
    }
    return task_ptrs[current_task_index];  // may be NULL if slot reaped
}

task_t* sched_get_task(int id) {
    if (id < 0 || id >= MAX_TASKS || id >= next_task_id) {
        return NULL;
    }
    if (!task_ptrs[id]) return NULL;
    if ((*task_ptrs[id]).state == TASK_STATE_ZOMBIE) {
        return NULL;
    }
    return task_ptrs[id];
}

int sched_check_children(int parent_id, int *status) {
    // Safe to print - not called from interrupt context
    framebuffer_draw_string("Checking for zombie children...", 700, 500, COLOR_CYAN, 0x00101828);
    
    for (int i = 0; i < next_task_id; i++) {
        if (i >= MAX_TASKS) break;
        if (!task_ptrs[i]) continue;

        if ((*task_ptrs[i]).state == TASK_STATE_ZOMBIE &&
            (*task_ptrs[i]).parent_id == parent_id) {
            if (status) {
                *status = (*task_ptrs[i]).exit_status;
            }
            return i;
        }
    }
    return -1;
}

void sched_orphan_children(int parent_id) {
    for (int i = 0; i < next_task_id; i++) {
        if (i >= MAX_TASKS) break;
        if (!task_ptrs[i]) continue;

        if ((*task_ptrs[i]).parent_id == parent_id && (*task_ptrs[i]).state != TASK_STATE_ZOMBIE) {
            (*task_ptrs[i]).parent_id = 0; // Reparent to init
        }
    }
}

void sched_reap_zombie(int task_id) {
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return;
    if (!task_ptrs[task_id]) return;  // Already reaped / never allocated.
    if ((*task_ptrs[task_id]).state != TASK_STATE_ZOMBIE) return;
    klog(KLOG_INFO, SUBSYS_SCHED, "[REAP] task_id=%d entering",
         task_id);

    // Free kernel stack
    uint64_t kstack_base = (*task_ptrs[task_id]).kernel_stack_top - KERNEL_STACK_SIZE;
    uint64_t kstack_phys = kstack_base - g_hhdm_offset;
    pmm_free_pages((void*)kstack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);

    // Free user address space (page tables + all user-mapped physical pages)
    // Only for user processes (cr3 != kernel PML4)
    uint64_t kernel_cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    if ((*task_ptrs[task_id]).cr3 != 0 && (*task_ptrs[task_id]).cr3 != kernel_cr3) {
        vmm_destroy_address_space_by_cr3((*task_ptrs[task_id]).cr3);
    }

    // Phase 15a: free the process's handle table (does not revoke the
    // referenced cap_object_t — that's orphan_collection's job), then
    // revoke every object owned by the dying process.
    cap_handle_table_free(&(*task_ptrs[task_id]).cap_handles);
    (void)revoke_collect_orphans((*task_ptrs[task_id]).id);

    // Phase 16: release any deprecated-syscall tracker slot held by this pid.
    // Prevents a long-lived tracker leak as pids churn.
    deprecated_forget_pid((int32_t)(*task_ptrs[task_id]).id);

    // Phase 17: release any VMO mappings the task still held. The
    // address-space pages are being torn down in vmm_destroy_address_space_by_cr3
    // already; this call reconciles refcount bookkeeping on the VMO side.
    vmo_cleanup_task((int32_t)(*task_ptrs[task_id]).id);

    // Phase 21: if the dying task owned any PCI devices via the userdrv
    // framework, reap that ownership: PIC-mask the IRQ line, mark IRQ
    // channels dead, revoke MMIO/IRQ/downstream caps, audit, clear entries.
    extern void userdrv_on_owner_death(int32_t pid);
    userdrv_on_owner_death((int32_t)(*task_ptrs[task_id]).id);

    // Phase 22: if the dying task had published any /sys/net/* names in
    // the rawnet registry (e.g. e1000d published /sys/net/rawframe, netd
    // published /sys/net/service), clear those entries so the next spawn
    // sees -ENOENT and reconnects cleanly after republish.
    extern void rawnet_on_peer_death(int32_t pid);
    rawnet_on_peer_death((int32_t)(*task_ptrs[task_id]).id);

    // Phase 20: remove from PID hash and global list before releasing to the
    // slab. mem_pages_used counter is zeroed since vmm_destroy_address_space_by_cr3
    // released the underlying frames; per-task accounting is now consistent.
    pid_hash_remove(task_ptrs[task_id]);
    task_ptrs[task_id]->mem_pages_used = 0;
    if (g_task_count > 0) g_task_count--;

    // Phase 14: return the task_t to the slab; slot goes empty.
    // No need to memset — slab alloc zeroes on next use.
    kmem_cache_free(task_cache, task_ptrs[task_id]);
    task_ptrs[task_id] = NULL;
    klog(KLOG_INFO, SUBSYS_SCHED, "[REAP] task_id=%d done", task_id);
}

// Get task by ID including zombies (needed for wait/reap operations)
task_t* sched_get_task_any(int id) {
    if (id < 0 || id >= MAX_TASKS || id >= next_task_id) {
        return NULL;
    }
    return task_ptrs[id];
}

// Get current task index
int sched_get_current_task_index(void) {
    return current_task_index;
}

// Helper to copy a process name from path
static void copy_process_name(char *dest, const char *path, size_t max_len) {
    // Find last '/' in path
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    size_t i;
    for (i = 0; i < max_len - 1 && name[i]; i++) {
        dest[i] = name[i];
    }
    dest[i] = '\0';
}

// Phase 7d: Spawn a new process from an ELF binary
// This is the modern replacement for fork+exec
int sched_spawn_process(const char *path, int parent_id) {
    if (!path) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SPAWN] ERROR: NULL path");
        return -1;
    }

    klog(KLOG_INFO, SUBSYS_SCHED, "[SPAWN] Spawning: %s", path);

    // Look up the file in the initrd
    size_t file_size;
    void *file_data = initrd_lookup(path, &file_size);
    if (!file_data) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SPAWN] ERROR: File not found: %s", path);
        klog(KLOG_INFO, SUBSYS_SCHED, "");
        return -1;
    }

    klog(KLOG_INFO, SUBSYS_SCHED, "[SPAWN] File found, size=%lu", (unsigned long)(file_size));

    // Load the ELF binary into a new address space
    uint64_t entry_point, cr3;
    if (!elf_load(file_data, &entry_point, &cr3)) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SPAWN] ERROR: ELF load failed");
        return -1;
    }

    klog(KLOG_INFO, SUBSYS_SCHED, "[SPAWN] ELF loaded: entry=0x%lx cr3=0x%lx", (unsigned long)(entry_point), (unsigned long)(cr3));

    // Create the user process (uses existing infrastructure)
    int pid = sched_create_user_process(entry_point, cr3);
    if (pid < 0) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SPAWN] ERROR: Process creation failed");
        return -1;
    }

    // Override the parent_id set by sched_create_user_process
    // (it defaults to current_task_index, but we want the explicit parent)
    spinlock_acquire(&sched_lock);
    (*task_ptrs[pid]).parent_id = parent_id;
    (*task_ptrs[pid]).pgid = parent_id; // Initially same process group as parent
    copy_process_name((*task_ptrs[pid]).name, path, sizeof((*task_ptrs[pid]).name));

    // Phase 15b: child inherits parent's pledge mask. If the caller wants a
    // narrower mask, they can pass it via future spawn_attrs; for now the
    // child simply carries the parent's authority forward. Further narrowing
    // is always possible post-spawn via SYS_PLEDGE.
    if (parent_id >= 0 && parent_id < MAX_TASKS && task_ptrs[parent_id]) {
        (*task_ptrs[pid]).pledge_mask = (*task_ptrs[parent_id]).pledge_mask;
    }

    // Phase 10c: Only inherit FDs 0-2 (stdin/stdout/stderr)
    // This prevents pipe FD leaks that would block EOF detection.
    // Higher FDs (3+) remain UNUSED in the child.
    for (int f = 0; f < 3; f++) {
        uint8_t ptype = (*task_ptrs[parent_id]).fd_table[f].type;
        (*task_ptrs[pid]).fd_table[f] = (*task_ptrs[parent_id]).fd_table[f];
        if (ptype == FD_TYPE_PIPE_READ || ptype == FD_TYPE_PIPE_WRITE) {
            pipe_ref_inc((*task_ptrs[pid]).fd_table[f].ref, ptype);
        }
    }
    // FDs 3-15 remain UNUSED (set by sched_create_user_process)

    spinlock_release(&sched_lock);

    klog(KLOG_INFO, SUBSYS_SCHED, "[SPAWN] Process created: pid=%lu name=", (unsigned long)(pid));
    klog(KLOG_INFO, SUBSYS_SCHED, "%s", (*task_ptrs[pid]).name);

    return pid;
}

// Phase 7d: Send a signal to a process
int sched_send_signal(int pid, int signal) {
    if (signal < 1 || signal >= MAX_SIGNALS) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SIGNAL] ERROR: Invalid signal number");
        return -1;
    }

    if (pid < 0 || pid >= next_task_id || pid >= MAX_TASKS) {
        klog(KLOG_ERROR, SUBSYS_SCHED, "[SIGNAL] ERROR: Invalid PID");
        return -1;
    }

    task_t *target = &(*task_ptrs[pid]);

    // Can't signal a zombie or unused task
    if (target->state == TASK_STATE_ZOMBIE) {
        return -1;
    }

    // SIGKILL is always fatal and cannot be caught or ignored
    if (signal == SIGKILL) {
        klog(KLOG_INFO, SUBSYS_SCHED, "[SIGNAL] SIGKILL sent to pid=%lu", (unsigned long)(pid));

        spinlock_acquire(&sched_lock);
        target->exit_status = 128 + SIGKILL;
        sched_orphan_children(pid);
        wake_waiting_parent(pid);
        // ZOMBIE LAST so a parent reaper on another CPU can't free
        // task_ptrs[pid] mid-cleanup. See SYS_EXIT for the same pattern.
        __atomic_store_n((volatile int *)&target->state, (int)TASK_STATE_ZOMBIE, __ATOMIC_RELEASE);
        spinlock_release(&sched_lock);
        return 0;
    }

    // Set the pending signal bit and wake blocked tasks under lock
    spinlock_acquire(&sched_lock);
    target->pending_signals |= (1 << signal);
    bool signal_woke = false;
    if (target->state == TASK_STATE_BLOCKED) {
        target->state = TASK_STATE_READY;
        signal_woke = true;
    }
    spinlock_release(&sched_lock);
    // Phase 20: runq insertion outside sched_lock.
    if (signal_woke) sched_enqueue_ready(target);

    klog(KLOG_INFO, SUBSYS_SCHED, "[SIGNAL] Signal %lu sent to pid=%lu", (unsigned long)(signal), (unsigned long)(pid));

    return 0;
}

// Phase 7d: Set a signal handler for the current process
void* sched_set_signal_handler(int signal, void (*handler)(int)) {
    if (signal < 1 || signal >= MAX_SIGNALS) {
        return SIG_DFL;
    }

    // SIGKILL cannot be caught or ignored
    if (signal == SIGKILL) {
        return SIG_DFL;
    }

    task_t *current = sched_get_current_task();
    if (!current) return SIG_DFL;

    void *old_handler = (void*)current->signal_handlers[signal];
    current->signal_handlers[signal] = handler;
    return old_handler;
}

// Phase 7d: Check and deliver pending signals for a task
// Called by the scheduler before returning to user mode
// Returns 1 if a signal caused the task to terminate, 0 otherwise
int sched_deliver_signals(task_t *task) {
    if (!task || task->pending_signals == 0) return 0;

    for (int sig = 1; sig < MAX_SIGNALS; sig++) {
        if (!(task->pending_signals & (1 << sig))) continue;

        // Clear the pending bit
        task->pending_signals &= ~(1 << sig);

        void (*handler)(int) = task->signal_handlers[sig];

        if (handler == SIG_IGN) {
            // Signal ignored
            continue;
        }

        if (handler == SIG_DFL) {
            // Default action: terminate the process
            klog(KLOG_INFO, SUBSYS_SCHED, "[SIGNAL] Default action (terminate) for signal %lu on pid=%lu", (unsigned long)(sig), (unsigned long)(task->id));

            task->exit_status = 128 + sig;
            sched_orphan_children(task->id);
            wake_waiting_parent(task->id);
            // ZOMBIE LAST — same use-after-free hazard as SYS_EXIT.
            __atomic_store_n((volatile int *)&task->state, (int)TASK_STATE_ZOMBIE, __ATOMIC_RELEASE);
            return 1; // Task was terminated
        }

        // User-defined handler - we would need to redirect execution to the handler
        // For now, we call the handler in kernel context (simplified approach)
        // A full implementation would manipulate the user stack to redirect RIP
        // This is sufficient for basic signal handling
        klog(KLOG_INFO, SUBSYS_SCHED, "[SIGNAL] Delivering signal %lu to handler at 0x%lx", (unsigned long)(sig), (unsigned long)((uint64_t)handler));

        // TODO: Full user-space signal delivery (manipulate user stack frame)
        // For now, treat custom handlers as SIG_IGN (acknowledge but don't crash)
    }

    return 0;
}

// Debug function - can be called from kernel debugger or panic handler
void sched_dump_stats(void) {
    // This is NOT called from interrupt context, so it's safe
    char msg[64];
    
    // Use simple number conversion
    msg[0] = 'S'; msg[1] = 'c'; msg[2] = 'h'; msg[3] = 'e'; msg[4] = 'd';
    msg[5] = ' '; msg[6] = 'c'; msg[7] = 'a'; msg[8] = 'l'; msg[9] = 'l';
    msg[10] = 's'; msg[11] = ':'; msg[12] = ' ';
    
    // Convert schedule_count to string (simplified)
    uint32_t count = schedule_count;
    int pos = 13;
    if (count == 0) {
        msg[pos++] = '0';
    } else {
        char digits[10];
        int digit_count = 0;
        while (count > 0) {
            digits[digit_count++] = '0' + (count % 10);
            count /= 10;
        }
        while (digit_count > 0) {
            msg[pos++] = digits[--digit_count];
        }
    }
    msg[pos] = '\0';
    
    framebuffer_draw_string(msg, 10, 750, COLOR_WHITE, COLOR_BLACK);
}

// Phase 8a: Snapshot all processes for system state reporting
int sched_snapshot_processes(state_process_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    spinlock_acquire(&sched_lock);

    int count = 0;
    int limit = next_task_id;
    if (limit > max_count) limit = max_count;
    if (limit > MAX_TASKS) limit = MAX_TASKS;

    for (int i = 0; i < limit; i++) {
        if (!task_ptrs[i]) continue;  // skip reaped slots
        out[count].pid = (*task_ptrs[i]).id;
        out[count].parent_pid = (*task_ptrs[i]).parent_id;
        out[count].pgid = (*task_ptrs[i]).pgid;
        out[count].state = (uint32_t)(*task_ptrs[i]).state;

        // Copy name
        int j;
        for (j = 0; j < STATE_PROC_NAME_LEN - 1 && (*task_ptrs[i]).name[j]; j++) {
            out[count].name[j] = (*task_ptrs[i]).name[j];
        }
        out[count].name[j] = '\0';

        out[count].heap_start = (*task_ptrs[i]).heap_start;
        out[count].brk = (*task_ptrs[i]).brk;
        out[count].stack_top = (*task_ptrs[i]).stack_top;
        out[count].heap_used = ((*task_ptrs[i]).brk > (*task_ptrs[i]).heap_start) ?
                                (*task_ptrs[i]).brk - (*task_ptrs[i]).heap_start : 0;
        out[count].pending_signals = (*task_ptrs[i]).pending_signals;
        out[count].exit_status = (*task_ptrs[i]).exit_status;
        // Phase 21.1: copy pledge mask for gash `ps` PLEDGE column.
        out[count].pledge_mask = (*task_ptrs[i]).pledge_mask.raw;
        out[count]._pad_pledge[0] = 0;
        out[count]._pad_pledge[1] = 0;
        out[count]._pad_pledge[2] = 0;
        count++;
    }

    spinlock_release(&sched_lock);
    return count;
}

// Phase 8d: Enqueue a CAN event to a process's event queue
void sched_enqueue_cap_event(int32_t pid, const state_cap_event_t *event) {
    if (!event) return;
    if (pid < 0 || pid >= next_task_id || pid >= MAX_TASKS) return;

    spinlock_acquire(&sched_lock);

    task_t *task = &(*task_ptrs[pid]);

    // Don't deliver to zombie/dead tasks
    if (task->state == TASK_STATE_ZOMBIE) {
        spinlock_release(&sched_lock);
        return;
    }

    // Write event to circular buffer
    task->event_queue[task->event_tail] = *event;
    task->event_tail = (task->event_tail + 1) % STATE_CAP_EVENT_QUEUE_SIZE;

    if (task->event_count < STATE_CAP_EVENT_QUEUE_SIZE) {
        task->event_count++;
    } else {
        // Queue full — overwrite oldest by advancing head
        task->event_head = (task->event_head + 1) % STATE_CAP_EVENT_QUEUE_SIZE;
    }

    // Wake up if blocked waiting for events
    bool event_woke = false;
    if (task->event_waiting && task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
        task->event_waiting = 0;
        event_woke = true;
    }

    spinlock_release(&sched_lock);
    // Phase 20: runq insertion outside sched_lock.
    if (event_woke) sched_enqueue_ready(task);
}

// Phase 8d: Dequeue one CAN event from a process's event queue
// Returns 1 if an event was dequeued, 0 if queue is empty
int sched_dequeue_cap_event(int task_id, state_cap_event_t *out) {
    if (!out) return 0;
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return 0;

    spinlock_acquire(&sched_lock);

    task_t *task = &(*task_ptrs[task_id]);

    if (task->event_count == 0) {
        spinlock_release(&sched_lock);
        return 0;
    }

    *out = task->event_queue[task->event_head];
    task->event_head = (task->event_head + 1) % STATE_CAP_EVENT_QUEUE_SIZE;
    task->event_count--;

    spinlock_release(&sched_lock);
    return 1;
}

// Phase 20: epoch task. Pinned to CPU 0 (set by sched_create_task's default).
// Sleeps via `sti; hlt`, checks g_timer_ticks every wake. Fires
// rlimit_epoch_tick once per epoch boundary (100 ticks @ 100 Hz = 1 s).
// Uses a local "last seen" counter to edge-trigger rather than
// level-trigger; doesn't matter if we skip a tick due to long-running
// scheduler work on CPU 0, we just fire at the next boundary.
void sched_epoch_task_entry(void) {
    uint64_t last_epoch_id = g_timer_ticks / 100u;
    while (1) {
        asm volatile("sti; hlt");
        uint64_t now_epoch = g_timer_ticks / 100u;
        if (now_epoch != last_epoch_id) {
            last_epoch_id = now_epoch;
            rlimit_epoch_tick();
        }
    }
}

// Phase 8d: Get pending event count for a process
int sched_pending_event_count(int task_id) {
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return 0;
    return (int)(*task_ptrs[task_id]).event_count;
}

// ---------------------------------------------------------------------------
// Phase 17: channel-wait primitives.
//
// Blocking model: the caller transitions its own task from RUNNING to
// TASK_STATE_CHAN_WAIT, links it into the channel's waiter list, and then
// loops on `sti; hlt; cli`. A hardware interrupt (timer tick from IRQ0 on
// legacy PIC or the LAPIC timer) drives the scheduler, which context-
// switches this task out while its state remains CHAN_WAIT. When another
// task calls sched_wake_one_on_channel, it sets our state=READY and stores
// wait_result; the next schedule() picks us up and the hlt loop exits.
//
// The timer tick rate is 100 Hz in Phase 13 (10 ms per tick); deadline_tsc
// is stored in tick units derived from g_timer_ticks. Submillisecond
// precision is not available without TSC calibration (Phase 20+ territory).
// ---------------------------------------------------------------------------

extern volatile uint64_t g_timer_ticks;

static uint64_t ns_to_ticks(uint64_t ns) {
    // 10 ms per tick; round up.
    return (ns + 9999999ULL) / 10000000ULL;
}

int sched_block_on_channel(void *channel, uint8_t dir, uint64_t timeout_ns,
                           struct task_struct **list_head) {
    task_t *cur = sched_get_current_task();
    if (!cur || !list_head) return -1;

    uint64_t deadline_tick = 0;
    if (timeout_ns != 0 && timeout_ns != 0xFFFFFFFFFFFFFFFFULL) {
        uint64_t dt = ns_to_ticks(timeout_ns);
        if (dt == 0) dt = 1;
        deadline_tick = g_timer_ticks + dt;
    }

    spinlock_acquire(&sched_lock);
    cur->wait_next     = *list_head;
    *list_head         = cur;
    cur->wait_reason   = dir;
    cur->wait_channel  = channel;
    cur->wait_result   = 0;
    cur->deadline_tsc  = deadline_tick;
    cur->state         = TASK_STATE_CHAN_WAIT;
    spinlock_release(&sched_lock);

    // Sleep until woken or deadline. sti allows the timer tick to run the
    // scheduler, which will context-switch us out while state=CHAN_WAIT.
    // The scheduler's per-tick deadline scan (in schedule()) flips our
    // state to READY and stamps -ETIMEDOUT in wait_result on timeout.
    while (cur->state == TASK_STATE_CHAN_WAIT) {
        asm volatile("sti; hlt" ::: "memory");
        asm volatile("cli" ::: "memory");
    }

    // Unlink ourselves from the waiter list (idempotent — sched_wake_one
    // may have already removed us).
    spinlock_acquire(&sched_lock);
    task_t **p = list_head;
    while (*p && *p != cur) {
        p = (task_t **)&((*p)->wait_next);
    }
    if (*p == cur) *p = (task_t *)cur->wait_next;
    spinlock_release(&sched_lock);

    int result = cur->wait_result;
    cur->wait_next    = NULL;
    cur->wait_channel = NULL;
    cur->deadline_tsc = 0;
    return result;
}

task_t *sched_wake_one_on_channel(struct task_struct **list_head,
                                  int32_t wait_result) {
    if (!list_head) return NULL;
    spinlock_acquire(&sched_lock);
    task_t *t = *list_head;
    if (!t) {
        spinlock_release(&sched_lock);
        return NULL;
    }
    *list_head = (task_t *)t->wait_next;
    t->wait_next   = NULL;
    t->wait_result = wait_result;
    bool waking = (t->state == TASK_STATE_CHAN_WAIT);
    if (waking) {
        t->state = TASK_STATE_READY;
    }
    spinlock_release(&sched_lock);
    // Phase 20: runq insertion happens outside sched_lock to keep the
    // hierarchy flat (sched_lock → runq.lock; never inverted).
    if (waking) sched_enqueue_ready(t);
    return t;
}

int sched_wake_all_on_channel(struct task_struct **list_head,
                              int32_t wait_result) {
    if (!list_head) return 0;
    int count = 0;
    // Drain the waiter list into a local chain under sched_lock, then flip
    // states + enqueue onto the runq outside the lock. This preserves the
    // sched_lock → runq.lock ordering invariant.
    task_t *to_wake_head = NULL;
    task_t *to_wake_tail = NULL;
    spinlock_acquire(&sched_lock);
    task_t *t = *list_head;
    *list_head = NULL;
    while (t) {
        task_t *next = (task_t *)t->wait_next;
        t->wait_next   = NULL;
        t->wait_result = wait_result;
        if (t->state == TASK_STATE_CHAN_WAIT) {
            t->state = TASK_STATE_READY;
            // Reuse runq_next as a temporary chain pointer until we enqueue.
            t->runq_next = NULL;
            if (to_wake_tail) {
                to_wake_tail->runq_next = t;
            } else {
                to_wake_head = t;
            }
            to_wake_tail = t;
        }
        count++;
        t = next;
    }
    spinlock_release(&sched_lock);

    // Phase 20: enqueue drained waiters onto runq(s) outside sched_lock.
    task_t *w = to_wake_head;
    while (w) {
        task_t *next = w->runq_next;
        w->runq_next = NULL;  // reset before enqueue
        sched_enqueue_ready(w);
        w = next;
    }
    return count;
}
