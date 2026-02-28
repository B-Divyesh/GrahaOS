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

static task_t tasks[MAX_TASKS];
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

void sched_init(void) {
    // Initialize the scheduler lock
    spinlock_init(&sched_lock, "scheduler");
    
    spinlock_acquire(&sched_lock);
    memset(tasks, 0, sizeof(tasks));

    // Task 0 is the kernel's idle task
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    tasks[0].parent_id = -1;
    tasks[0].waiting_for_child = -1;
    tasks[0].pgid = 0;
    tasks[0].pending_signals = 0;
    tasks[0].name[0] = 'k'; tasks[0].name[1] = 'e'; tasks[0].name[2] = 'r';
    tasks[0].name[3] = 'n'; tasks[0].name[4] = 'e'; tasks[0].name[5] = 'l';
    tasks[0].name[6] = '\0';
    for (int i = 0; i < MAX_SIGNALS; i++) {
        tasks[0].signal_handlers[i] = SIG_DFL;
    }
    tasks[0].event_head = 0;
    tasks[0].event_tail = 0;
    tasks[0].event_count = 0;
    tasks[0].event_waiting = 0;

    // Get current stack pointer
    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    
    // Set kernel stack for idle task
    tasks[0].kernel_stack_top = (current_rsp & ~0xFFF) + 0x4000;
    
    // Initialize the interrupt frame for idle task
    memset(&tasks[0].regs, 0, sizeof(struct interrupt_frame));
    tasks[0].regs.cs = 0x08;      // Kernel code segment
    tasks[0].regs.ss = 0x10;      // Kernel data segment  
    tasks[0].regs.ds = 0x10;      // Data segment
    tasks[0].regs.rflags = 0x202; // IF=1, Reserved=1
    tasks[0].regs.rsp = current_rsp;
    tasks[0].regs.rbp = current_rsp;
    
    // Update the per-CPU TSS
    uint32_t cpu_id = smp_get_current_cpu();
    g_cpu_locals[cpu_id].tss.rsp0 = tasks[0].kernel_stack_top;
    
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
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_BLOCKED;  // BLOCKED until fully initialized
    tasks[id].parent_id = current_task_index >= 0 ? tasks[current_task_index].id : -1;
    tasks[id].waiting_for_child = -1;
    tasks[id].pgid = current_task_index >= 0 ? tasks[current_task_index].pgid : 0;
    tasks[id].pending_signals = 0;
    tasks[id].name[0] = '\0';
    for (int s = 0; s < MAX_SIGNALS; s++) {
        tasks[id].signal_handlers[s] = SIG_DFL;
    }
    tasks[id].event_head = 0;
    tasks[id].event_tail = 0;
    tasks[id].event_count = 0;
    tasks[id].event_waiting = 0;

    spinlock_release(&sched_lock);

    // Allocate kernel stack (outside lock to avoid holding lock during allocation)
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) {
        // Mark task slot as unusable
        spinlock_acquire(&sched_lock);
        tasks[id].state = TASK_STATE_ZOMBIE;
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
    tasks[id].kernel_stack_top = kstack_top;

    // Initialize the interrupt frame
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));

    // Set up initial context
    tasks[id].regs.rip = entry_addr;  // Use validated address
    tasks[id].regs.cs = 0x08;         // Kernel code segment
    tasks[id].regs.ss = 0x10;         // Kernel data segment
    tasks[id].regs.ds = 0x10;         // Data segment
    tasks[id].regs.rflags = 0x202;    // IF=1, Reserved=1
    tasks[id].regs.rsp = (kstack_top - 128) & ~0xF;  // Stack pointer
    tasks[id].regs.rbp = tasks[id].regs.rsp;         // Base pointer

    // Use kernel address space
    tasks[id].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());

    // NOW mark as READY - fully initialized, safe for scheduler
    tasks[id].state = TASK_STATE_READY;

    spinlock_release(&sched_lock);
    
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
    id = next_task_id++;

    tasks[id].id = id;
    tasks[id].state = TASK_STATE_BLOCKED;  // BLOCKED until fully initialized
    tasks[id].cr3 = cr3;
    tasks[id].parent_id = tasks[current_task_index].id;
    tasks[id].waiting_for_child = -1;
    tasks[id].exit_status = 0;
    tasks[id].pgid = tasks[current_task_index].pgid; // Inherit parent's process group
    tasks[id].pending_signals = 0;
    tasks[id].name[0] = '\0';
    for (int s = 0; s < MAX_SIGNALS; s++) {
        tasks[id].signal_handlers[s] = SIG_DFL;
    }
    tasks[id].event_head = 0;
    tasks[id].event_tail = 0;
    tasks[id].event_count = 0;
    tasks[id].event_waiting = 0;

    asm volatile("push %0; popfq" : : "r"(flags));
    spinlock_release(&sched_lock);

    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) return -1;
    tasks[id].kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;
    
    uint64_t kstack_base = tasks[id].kernel_stack_top - KERNEL_STACK_SIZE;
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
    
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = rip;
    tasks[id].regs.rflags = 0x202;
    tasks[id].regs.rsp = user_stack_top - 16;  // RSP near top of user stack
    tasks[id].regs.cs = 0x20 | 3;
    tasks[id].regs.ss = 0x18 | 3;

    // Phase 7c: Initialize heap management fields
    // Heap starts at 4GB (0x100000000) in user space
    // This is well above typical code/data sections and below stack
    tasks[id].heap_start = 0x100000000ULL;
    tasks[id].brk = tasks[id].heap_start;  // Initially empty heap
    tasks[id].stack_top = user_stack_top;   // Top of stack for collision detection

    // NOW mark as READY - fully initialized, safe for scheduler
    tasks[id].state = TASK_STATE_READY;

    spinlock_release(&sched_lock);

    return id;
}

void wake_waiting_parent(int child_id) {
    // NO FRAMEBUFFER CALLS - might be called from interrupt context
    
    if (child_id < 0 || child_id >= next_task_id || child_id >= MAX_TASKS) {
        return;
    }
    
    int parent_id = tasks[child_id].parent_id;
    
    if (parent_id < 0 || parent_id >= next_task_id) {
        return;
    }
    
    // Check if parent is waiting for this child or any child
    if (tasks[parent_id].state == TASK_STATE_BLOCKED &&
        (tasks[parent_id].waiting_for_child == child_id || 
         tasks[parent_id].waiting_for_child == -1)) {
        
        // Wake up the parent
        tasks[parent_id].state = TASK_STATE_READY;
        tasks[parent_id].waiting_for_child = -1;
    }
}

// CRITICAL: This function is called from interrupt context
// NO LOCKS THAT CAN BE HELD BY NORMAL CODE!
// NO FRAMEBUFFER CALLS!
void schedule(struct interrupt_frame *frame) {
    // Increment counter
    schedule_count++;

    // Minimal logging for debugging context switches
    static volatile uint32_t sched_log_count = 0;
    if ((sched_log_count++ & 0xFF) == 0) {  // Log every 256 calls
        serial_write("[SCHED] schedule() called\n");
    }
    
    // Validate frame
    if (!frame) {
        asm volatile("cli; hlt");
        while(1);
    }
    
    uint64_t frame_addr = (uint64_t)frame;
    if (frame_addr < 0xFFFF800000000000) {
        asm volatile("cli; hlt");
        while(1);
    }
    
    // Check if scheduler is initialized
    if (next_task_id == 0) {
        return;
    }
    
    // Validate current task index
    if (current_task_index >= next_task_id || current_task_index < 0) {
        current_task_index = 0;
    }
    
    // CRITICAL: Don't try to lock if we're already in a critical section
    // This can happen if timer interrupt fires while we're in spinlock code
    uint64_t cpu_id = smp_get_current_cpu();
    
    // Check if we're holding ANY lock
    if (sched_lock.locked && sched_lock.owner == cpu_id) {
        // We're already in scheduler - skip this tick
        return;
    }
    
    // Try to acquire scheduler lock with timeout
    int lock_attempts = 1000;
    while (lock_attempts-- > 0) {
        if (!sched_lock.locked) {
            if (!__atomic_test_and_set(&sched_lock.locked, __ATOMIC_ACQUIRE)) {
                sched_lock.owner = cpu_id;
                sched_lock.count = 1;
                break;
            }
        }
        asm volatile("pause");
    }
    
    if (lock_attempts <= 0) {
        // Couldn't get lock - skip this scheduling round
        return;
    }
    
    // Save current task state
    tasks[current_task_index].regs = *frame;
    
    // Update task state
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    // Find next task to run
    int start_index = current_task_index;
    int next_index = current_task_index;
    bool found = false;
    
    for (int attempts = 0; attempts < next_task_id; attempts++) {
        next_index = (next_index + 1) % next_task_id;
        if (tasks[next_index].state == TASK_STATE_READY) {
            found = true;
            current_task_index = next_index;
            context_switches++;

            // Log first switch to each task
            static uint8_t task_switched[MAX_TASKS] = {0};
            if (!task_switched[next_index]) {
                task_switched[next_index] = 1;
                serial_write("[SCHED] First switch to task ");
                serial_write_dec(next_index);
                serial_write(" (id=");
                serial_write_dec(tasks[next_index].id);
                serial_write(")\n");
            }
            break;
        }
    }
    
    if (!found) {
        // No ready tasks - stay with current
        if (tasks[start_index].state != TASK_STATE_ZOMBIE) {
            current_task_index = start_index;
        } else {
            // Find any non-zombie task
            for (int i = 0; i < next_task_id; i++) {
                if (tasks[i].state != TASK_STATE_ZOMBIE) {
                    current_task_index = i;
                    break;
                }
            }
        }
    }

    tasks[current_task_index].state = TASK_STATE_RUNNING;

    // Phase 7d: Deliver pending signals before resuming the task
    if (sched_deliver_signals(&tasks[current_task_index])) {
        // Task was terminated by a signal - find another runnable task
        bool found_replacement = false;
        for (int i = 0; i < next_task_id; i++) {
            if (tasks[i].state == TASK_STATE_READY) {
                current_task_index = i;
                tasks[i].state = TASK_STATE_RUNNING;
                found_replacement = true;
                break;
            }
        }
        if (!found_replacement) {
            // No runnable tasks - fall back to the idle task (task 0)
            // The idle task should always exist and never be in zombie state
            current_task_index = 0;
            if (tasks[0].state == TASK_STATE_ZOMBIE) {
                // Kernel panic - no runnable task at all
                serial_write("[SCHED] PANIC: No runnable tasks!\n");
                sched_lock.owner = (uint64_t)-1;
                sched_lock.count = 0;
                __atomic_clear(&sched_lock.locked, __ATOMIC_RELEASE);
                asm volatile("cli; hlt");
                while(1);
            }
            tasks[0].state = TASK_STATE_RUNNING;
        }
    }

    // Update TSS RSP0
    g_cpu_locals[cpu_id].tss.rsp0 = tasks[current_task_index].kernel_stack_top;

    // Switch address space if needed
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        // Log CR3 switch (only first time per task)
        static uint8_t task_cr3_switched[MAX_TASKS] = {0};
        if (!task_cr3_switched[current_task_index]) {
            task_cr3_switched[current_task_index] = 1;
            serial_write("[SCHED] Switching CR3 to 0x");
            serial_write_hex(tasks[current_task_index].cr3);
            serial_write(" for task ");
            serial_write_dec(current_task_index);
            serial_write("\n");
        }
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
    }

    // Release the scheduler lock properly
    sched_lock.owner = (uint64_t)-1;
    sched_lock.count = 0;
    
    // Memory barrier
    asm volatile("mfence" ::: "memory");
    
    __atomic_clear(&sched_lock.locked, __ATOMIC_RELEASE);

    // Restore new task context
    *frame = tasks[current_task_index].regs;
}

// These functions are safe - not called from interrupt context
task_t* sched_get_current_task(void) {
    if (current_task_index >= MAX_TASKS || current_task_index >= next_task_id) {
        return NULL;
    }
    return &tasks[current_task_index];
}

task_t* sched_get_task(int id) {
    if (id < 0 || id >= MAX_TASKS || id >= next_task_id) {
        return NULL;
    }
    if (tasks[id].state == TASK_STATE_ZOMBIE) {
        return NULL;
    }
    return &tasks[id];
}

int sched_check_children(int parent_id, int *status) {
    // Safe to print - not called from interrupt context
    framebuffer_draw_string("Checking for zombie children...", 700, 500, COLOR_CYAN, 0x00101828);
    
    for (int i = 0; i < next_task_id; i++) {
        if (i >= MAX_TASKS) break;
        
        if (tasks[i].state == TASK_STATE_ZOMBIE && 
            tasks[i].parent_id == parent_id) {
            if (status) {
                *status = tasks[i].exit_status;
            }
            return i;
        }
    }
    return -1;
}

void sched_orphan_children(int parent_id) {
    for (int i = 0; i < next_task_id; i++) {
        if (i >= MAX_TASKS) break;
        
        if (tasks[i].parent_id == parent_id && tasks[i].state != TASK_STATE_ZOMBIE) {
            tasks[i].parent_id = 0; // Reparent to init
        }
    }
}

void sched_reap_zombie(int task_id) {
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return;
    if (tasks[task_id].state != TASK_STATE_ZOMBIE) return;

    // Free kernel stack
    uint64_t kstack_base = tasks[task_id].kernel_stack_top - KERNEL_STACK_SIZE;
    uint64_t kstack_phys = kstack_base - g_hhdm_offset;
    pmm_free_pages((void*)kstack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);

    // Free user address space (page tables + all user-mapped physical pages)
    // Only for user processes (cr3 != kernel PML4)
    uint64_t kernel_cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    if (tasks[task_id].cr3 != 0 && tasks[task_id].cr3 != kernel_cr3) {
        vmm_destroy_address_space_by_cr3(tasks[task_id].cr3);
    }

    // Clear the task slot
    memset(&tasks[task_id], 0, sizeof(task_t));
}

// Get task by ID including zombies (needed for wait/reap operations)
task_t* sched_get_task_any(int id) {
    if (id < 0 || id >= MAX_TASKS || id >= next_task_id) {
        return NULL;
    }
    return &tasks[id];
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
        serial_write("[SPAWN] ERROR: NULL path\n");
        return -1;
    }

    serial_write("[SPAWN] Spawning: ");
    serial_write(path);
    serial_write("\n");

    // Look up the file in the initrd
    size_t file_size;
    void *file_data = initrd_lookup(path, &file_size);
    if (!file_data) {
        serial_write("[SPAWN] ERROR: File not found: ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    serial_write("[SPAWN] File found, size=");
    serial_write_dec(file_size);
    serial_write("\n");

    // Load the ELF binary into a new address space
    uint64_t entry_point, cr3;
    if (!elf_load(file_data, &entry_point, &cr3)) {
        serial_write("[SPAWN] ERROR: ELF load failed\n");
        return -1;
    }

    serial_write("[SPAWN] ELF loaded: entry=");
    serial_write_hex(entry_point);
    serial_write(" cr3=");
    serial_write_hex(cr3);
    serial_write("\n");

    // Create the user process (uses existing infrastructure)
    int pid = sched_create_user_process(entry_point, cr3);
    if (pid < 0) {
        serial_write("[SPAWN] ERROR: Process creation failed\n");
        return -1;
    }

    // Override the parent_id set by sched_create_user_process
    // (it defaults to current_task_index, but we want the explicit parent)
    spinlock_acquire(&sched_lock);
    tasks[pid].parent_id = parent_id;
    tasks[pid].pgid = parent_id; // Initially same process group as parent
    copy_process_name(tasks[pid].name, path, sizeof(tasks[pid].name));
    spinlock_release(&sched_lock);

    serial_write("[SPAWN] Process created: pid=");
    serial_write_dec(pid);
    serial_write(" name=");
    serial_write(tasks[pid].name);
    serial_write("\n");

    return pid;
}

// Phase 7d: Send a signal to a process
int sched_send_signal(int pid, int signal) {
    if (signal < 1 || signal >= MAX_SIGNALS) {
        serial_write("[SIGNAL] ERROR: Invalid signal number\n");
        return -1;
    }

    if (pid < 0 || pid >= next_task_id || pid >= MAX_TASKS) {
        serial_write("[SIGNAL] ERROR: Invalid PID\n");
        return -1;
    }

    task_t *target = &tasks[pid];

    // Can't signal a zombie or unused task
    if (target->state == TASK_STATE_ZOMBIE) {
        return -1;
    }

    // SIGKILL is always fatal and cannot be caught or ignored
    if (signal == SIGKILL) {
        serial_write("[SIGNAL] SIGKILL sent to pid=");
        serial_write_dec(pid);
        serial_write("\n");

        spinlock_acquire(&sched_lock);
        target->exit_status = 128 + SIGKILL;
        target->state = TASK_STATE_ZOMBIE;
        sched_orphan_children(pid);
        wake_waiting_parent(pid);
        spinlock_release(&sched_lock);
        return 0;
    }

    // Set the pending signal bit and wake blocked tasks under lock
    spinlock_acquire(&sched_lock);
    target->pending_signals |= (1 << signal);
    if (target->state == TASK_STATE_BLOCKED) {
        target->state = TASK_STATE_READY;
    }
    spinlock_release(&sched_lock);

    serial_write("[SIGNAL] Signal ");
    serial_write_dec(signal);
    serial_write(" sent to pid=");
    serial_write_dec(pid);
    serial_write("\n");

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
            serial_write("[SIGNAL] Default action (terminate) for signal ");
            serial_write_dec(sig);
            serial_write(" on pid=");
            serial_write_dec(task->id);
            serial_write("\n");

            task->exit_status = 128 + sig;
            task->state = TASK_STATE_ZOMBIE;
            sched_orphan_children(task->id);
            wake_waiting_parent(task->id);
            return 1; // Task was terminated
        }

        // User-defined handler - we would need to redirect execution to the handler
        // For now, we call the handler in kernel context (simplified approach)
        // A full implementation would manipulate the user stack to redirect RIP
        // This is sufficient for basic signal handling
        serial_write("[SIGNAL] Delivering signal ");
        serial_write_dec(sig);
        serial_write(" to handler at ");
        serial_write_hex((uint64_t)handler);
        serial_write("\n");

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
        out[count].pid = tasks[i].id;
        out[count].parent_pid = tasks[i].parent_id;
        out[count].pgid = tasks[i].pgid;
        out[count].state = (uint32_t)tasks[i].state;

        // Copy name
        int j;
        for (j = 0; j < STATE_PROC_NAME_LEN - 1 && tasks[i].name[j]; j++) {
            out[count].name[j] = tasks[i].name[j];
        }
        out[count].name[j] = '\0';

        out[count].heap_start = tasks[i].heap_start;
        out[count].brk = tasks[i].brk;
        out[count].stack_top = tasks[i].stack_top;
        out[count].heap_used = (tasks[i].brk > tasks[i].heap_start) ?
                                tasks[i].brk - tasks[i].heap_start : 0;
        out[count].pending_signals = tasks[i].pending_signals;
        out[count].exit_status = tasks[i].exit_status;
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

    task_t *task = &tasks[pid];

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
    if (task->event_waiting && task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
        task->event_waiting = 0;
    }

    spinlock_release(&sched_lock);
}

// Phase 8d: Dequeue one CAN event from a process's event queue
// Returns 1 if an event was dequeued, 0 if queue is empty
int sched_dequeue_cap_event(int task_id, state_cap_event_t *out) {
    if (!out) return 0;
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return 0;

    spinlock_acquire(&sched_lock);

    task_t *task = &tasks[task_id];

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

// Phase 8d: Get pending event count for a process
int sched_pending_event_count(int task_id) {
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return 0;
    return (int)tasks[task_id].event_count;
}
