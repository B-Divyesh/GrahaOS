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
static spinlock_t sched_lock = SPINLOCK_INITIALIZER("scheduler");

// Debug counters (for post-mortem analysis only)
static volatile uint32_t schedule_count = 0;
static volatile uint32_t context_switches = 0;

// Simple memset implementation
static void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n-- > 0) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void sched_init(void) {
    // Initialize the scheduler lock with proper name
    spinlock_init(&sched_lock, "scheduler");
    
    spinlock_acquire(&sched_lock);
    memset(tasks, 0, sizeof(tasks));

    // Task 0 is the kernel's idle task
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    tasks[0].parent_id = -1;
    tasks[0].waiting_for_child = -1;
    
    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    
    tasks[0].kernel_stack_top = (current_rsp & ~0xFFF) + 0x4000;
    
    // Update the per-CPU TSS
    uint32_t cpu_id = smp_get_current_cpu();
    g_cpu_locals[cpu_id].tss.rsp0 = tasks[0].kernel_stack_top;
    
    current_task_index = 0;
    
    spinlock_release(&sched_lock);
    
    // This is BEFORE interrupts are enabled, so it's safe
    framebuffer_draw_string("Scheduler initialized with wait() support", 700, 20, COLOR_GREEN, 0x00101828);
}

int sched_create_task(void (*entry_point)(void)) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;
    tasks[id].parent_id = tasks[current_task_index].id;
    tasks[id].waiting_for_child = -1;

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

    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = (uint64_t)entry_point;
    tasks[id].regs.cs = 0x08;
    tasks[id].regs.rflags = 0x202;
    tasks[id].regs.rsp = (tasks[id].kernel_stack_top - 16) & ~0xF;
    tasks[id].regs.ss = 0x10;
    
    tasks[id].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());

    // Safe to print here - not in interrupt context
    framebuffer_draw_string("Kernel task created", 700, 40, COLOR_CYAN, 0x00101828);

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
    // Increment counters for debugging (can be read later)
    schedule_count++;
    
    if (!frame) {
        // Can't print - just halt
        asm volatile("cli; hlt");
    }
    
    if (current_task_index >= next_task_id || current_task_index < 0) {
        // Can't print - just halt
        asm volatile("cli; hlt");
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
        if (tasks[start_index].state == TASK_STATE_READY || 
            tasks[start_index].state == TASK_STATE_RUNNING) {
            current_task_index = start_index;
        } else {
            // Check for any non-zombie task
            int non_zombie_count = 0;
            for (int i = 0; i < next_task_id; i++) {
                if (tasks[i].state != TASK_STATE_ZOMBIE) {
                    non_zombie_count++;
                    if (tasks[i].state == TASK_STATE_BLOCKED) {
                        current_task_index = i;
                        break;
                    }
                }
            }
            if (non_zombie_count == 0) {
                // All tasks dead - halt
                asm volatile("cli; hlt");
            }
        }
    }

    tasks[current_task_index].state = TASK_STATE_RUNNING;
    
    // Update TSS RSP0
    uint32_t cpu_id = smp_get_current_cpu();
    g_cpu_locals[cpu_id].tss.rsp0 = tasks[current_task_index].kernel_stack_top;

    // Switch address space if needed
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
    }

    // Validate stack pointer
    uint64_t new_rsp = tasks[current_task_index].regs.rsp;
    
    if (current_task_index != 0) {
        if (tasks[current_task_index].regs.cs & 3) {
            // User mode - check user stack bounds
            if (new_rsp < 0x7FFFFFFFE000 || new_rsp > 0x7FFFFFFFF000) {
                asm volatile("cli; hlt");
            }
        } else {
            // Kernel mode - check kernel stack bounds
            uint64_t stack_base = tasks[current_task_index].kernel_stack_top - KERNEL_STACK_SIZE;
            if (new_rsp < stack_base || new_rsp > tasks[current_task_index].kernel_stack_top) {
                asm volatile("cli; hlt");
            }
        }
    }

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