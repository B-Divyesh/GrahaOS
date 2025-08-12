//arch/x86_64/cpu/sched/sched.c
#include "sched.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../gdt.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/sync/spinlock.h"

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
    tasks[id].state = TASK_STATE_READY;
    tasks[id].parent_id = current_task_index >= 0 ? tasks[current_task_index].id : -1;
    tasks[id].waiting_for_child = -1;

    spinlock_release(&sched_lock);

    // Allocate kernel stack
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) {
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
    
    // Clear all other registers
    tasks[id].regs.rax = 0;
    tasks[id].regs.rbx = 0;
    tasks[id].regs.rcx = 0;
    tasks[id].regs.rdx = 0;
    tasks[id].regs.rsi = 0;
    tasks[id].regs.rdi = 0;
    tasks[id].regs.r8 = 0;
    tasks[id].regs.r9 = 0;
    tasks[id].regs.r10 = 0;
    tasks[id].regs.r11 = 0;
    tasks[id].regs.r12 = 0;
    tasks[id].regs.r13 = 0;
    tasks[id].regs.r14 = 0;
    tasks[id].regs.r15 = 0;
    
    // Use kernel address space
    tasks[id].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    
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
    tasks[id].state = TASK_STATE_READY;
    tasks[id].cr3 = cr3;
    tasks[id].parent_id = tasks[current_task_index].id;
    tasks[id].waiting_for_child = -1;
    tasks[id].exit_status = 0;
    
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
    
    uint64_t user_stack_addr = 0x7FFFFFFFF000;
    void* user_stack_phys = pmm_alloc_page();
    if (!user_stack_phys) {
        pmm_free_pages(kstack_phys, num_pages);
        return -1;
    }
    
    vmm_address_space_t* proc_space = NULL;
    for(int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (vmm_get_pml4_phys(&address_space_pool[i]) == cr3) {
            proc_space = &address_space_pool[i];
            break;
        }
    }
    if (proc_space == NULL) {
        pmm_free_pages(kstack_phys, num_pages);
        pmm_free_page(user_stack_phys);
        return -1;
    }
    
    uint64_t user_stack_page_base = user_stack_addr - PAGE_SIZE;
    uint64_t flags_map = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    if (!vmm_map_page(proc_space, user_stack_page_base, (uint64_t)user_stack_phys, flags_map)) {
        pmm_free_pages(kstack_phys, num_pages);
        pmm_free_page(user_stack_phys);
        return -1;
    }
    
    // Safe to print here - not in interrupt context
    framebuffer_draw_string("User process created successfully!", 700, 120, COLOR_GREEN, 0x00101828);

    spinlock_acquire(&sched_lock);
    
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = rip;
    tasks[id].regs.rflags = 0x202;
    tasks[id].regs.rsp = user_stack_page_base + PAGE_SIZE - 16;
    tasks[id].regs.cs = 0x20 | 3;
    tasks[id].regs.ss = 0x18 | 3;
    
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
    
    // Update TSS RSP0
    g_cpu_locals[cpu_id].tss.rsp0 = tasks[current_task_index].kernel_stack_top;

    // Switch address space if needed
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
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
    
    // Clear the task slot
    memset(&tasks[task_id], 0, sizeof(task_t));
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
