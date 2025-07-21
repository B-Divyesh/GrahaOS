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
    kernel_tss.rsp0 = tasks[0].kernel_stack_top;
    
    current_task_index = 0;
    
    spinlock_release(&sched_lock);
    
    framebuffer_draw_string("Scheduler initialized with wait() support", 700, 20, COLOR_GREEN, 0x00101828);
}

int sched_create_task(void (*entry_point)(void)) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;
    tasks[id].parent_id = tasks[current_task_index].id; // Set parent
    tasks[id].waiting_for_child = -1; // Not waiting

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

    framebuffer_draw_string("Kernel task created", 700, 40, COLOR_CYAN, 0x00101828);

    return id;
}

static void sched_hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17];
    int i = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    for (i = 0; i < 16; i++) {
        temp[i] = hex_chars[(value >> ((15 - i) * 4)) & 0xF];
    }
    temp[16] = '\0';

    int start = 0;
    while (start < 15 && temp[start] == '0') start++;
    
    int j = 0;
    while (start < 16) {
        buffer[j++] = temp[start++];
    }
    buffer[j] = '\0';
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
    tasks[id].parent_id = tasks[current_task_index].id; // CRITICAL: Set parent!
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
    
    char stack_msg[64] = "User process created, parent=";
    char parent_str[4];
    int parent_id = tasks[id].parent_id;
    parent_str[0] = '0' + parent_id;
    parent_str[1] = '\0';
    int i = 0;
    while (stack_msg[i]) i++;
    int j = 0;
    while (parent_str[j]) stack_msg[i++] = parent_str[j++];
    stack_msg[i] = '\0';
    framebuffer_draw_string(stack_msg, 700, 60, COLOR_CYAN, 0x00101828);

    spinlock_acquire(&sched_lock);
    
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = rip;
    tasks[id].regs.rflags = 0x202;
    tasks[id].regs.rsp = user_stack_page_base + PAGE_SIZE - 16;
    tasks[id].regs.cs = 0x20 | 3;
    tasks[id].regs.ss = 0x18 | 3;
    
    spinlock_release(&sched_lock);
    
    framebuffer_draw_string("User process created successfully!", 700, 120, COLOR_GREEN, 0x00101828);
    
    
    return id;
}

// NEW: Wake up parent waiting for this child
void wake_waiting_parent(int child_id) {
    // Debug
    char msg[64] = "wake_waiting_parent: child ";
    msg[28] = '0' + child_id;
    msg[29] = '\0';
    framebuffer_draw_string(msg, 700, 540, COLOR_MAGENTA, 0x00101828);
    
    // Find the parent of this child
    if (child_id < 0 || child_id >= next_task_id || child_id >= MAX_TASKS) {
        framebuffer_draw_string("  Invalid child_id!", 700, 560, COLOR_RED, 0x00101828);
        return;
    }
    
    int parent_id = tasks[child_id].parent_id;
    
    // Debug: show parent
    char pmsg[32] = "  Parent is ";
    pmsg[12] = '0' + parent_id;
    pmsg[13] = '\0';
    framebuffer_draw_string(pmsg, 700, 560, COLOR_MAGENTA, 0x00101828);
    
    if (parent_id < 0 || parent_id >= next_task_id) {
        framebuffer_draw_string("  No valid parent", 700, 580, COLOR_YELLOW, 0x00101828);
        return;
    }
    
    // Check if parent is waiting for this child or any child
    if (tasks[parent_id].state == TASK_STATE_BLOCKED &&
        (tasks[parent_id].waiting_for_child == child_id || 
         tasks[parent_id].waiting_for_child == -1)) {
        
        // Wake up the parent
        tasks[parent_id].state = TASK_STATE_READY;
        tasks[parent_id].waiting_for_child = -1;
        
        // Debug message
        framebuffer_draw_string("  Woke up parent!", 700, 600, COLOR_GREEN, 0x00101828);
    } else {
        // Debug: show why we didn't wake parent
        char smsg[64] = "  Parent state: ";
        if (tasks[parent_id].state == TASK_STATE_READY) smsg[16] = 'R';
        else if (tasks[parent_id].state == TASK_STATE_RUNNING) smsg[16] = 'N';
        else if (tasks[parent_id].state == TASK_STATE_BLOCKED) smsg[16] = 'B';
        else if (tasks[parent_id].state == TASK_STATE_ZOMBIE) smsg[16] = 'Z';
        else smsg[16] = '?';
        smsg[17] = '\0';
        framebuffer_draw_string(smsg, 700, 600, COLOR_YELLOW, 0x00101828);
    }
}

// External variables from syscall.c for debugging
extern volatile uint64_t syscall_entry_reached;
extern volatile uint64_t syscall_about_to_return;
extern volatile uint64_t syscall_stack_switched;

void schedule(struct interrupt_frame *frame) {
    static int check_count = 0;
    check_count++;
    if (check_count == 10) {
        if (syscall_entry_reached) {
            framebuffer_draw_string("SYSCALL ENTRY WAS REACHED!", 700, 160, COLOR_RED, 0x00101828);
        }
        if (syscall_about_to_return) {
            framebuffer_draw_string("SYSCALL RETURN WAS REACHED!", 700, 180, COLOR_GREEN, 0x00101828);
        }
        if (syscall_stack_switched) {
            framebuffer_draw_string("SWAPGS STACK SWITCH OK!", 700, 200, COLOR_GREEN, 0x00101828);
        }
    }
    if (!frame) {
        framebuffer_draw_string("PANIC: NULL frame in scheduler!", 10, 10, COLOR_WHITE, COLOR_RED);
        asm volatile("cli; hlt");
    }
    
    if (current_task_index >= next_task_id || current_task_index < 0) {
        framebuffer_draw_string("PANIC: Invalid current_task_index!", 10, 30, COLOR_WHITE, COLOR_RED);
        asm volatile("cli; hlt");
    }
    
    tasks[current_task_index].regs = *frame;
    
    // Don't change zombie or blocked tasks back to ready
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    int start_index = current_task_index;
    int next_index = current_task_index;
    bool found = false;
    
    for (int attempts = 0; attempts < next_task_id; attempts++) {
        next_index = (next_index + 1) % next_task_id;
        if (tasks[next_index].state == TASK_STATE_READY) {
            found = true;
            current_task_index = next_index;
            break;
        }
    }
    
    if (!found) {
        if (tasks[start_index].state == TASK_STATE_READY || tasks[start_index].state == TASK_STATE_RUNNING) {
            current_task_index = start_index;
        } else {
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
                framebuffer_draw_string("KERNEL: All tasks terminated", 10, 700, COLOR_RED, 0x00101828);
                asm volatile("cli; hlt");
            }
        }
    }

    tasks[current_task_index].state = TASK_STATE_RUNNING;

    static int switch_count = 0;
    if (switch_count < 10) {
        switch_count++;
        char switch_msg[80];
        char *p = switch_msg;
        
        const char *s1 = "Switch to task ";
        while (*s1) *p++ = *s1++;
        *p++ = '0' + current_task_index;
        
        const char *s2 = " RIP=0x";
        while (*s2) *p++ = *s2++;
        
        uint64_t rip = tasks[current_task_index].regs.rip;
        char rip_str[17];
        sched_hex_to_string(rip, rip_str);
        char *rs = rip_str;
        while (*rs) *p++ = *rs++;
        
        const char *s3 = " CS=0x";
        while (*s3) *p++ = *s3++;
        uint16_t cs = tasks[current_task_index].regs.cs;
        *p++ = ((cs >> 4) & 0xF) < 10 ? '0' + ((cs >> 4) & 0xF) : 'A' + ((cs >> 4) & 0xF) - 10;
        *p++ = (cs & 0xF) < 10 ? '0' + (cs & 0xF) : 'A' + (cs & 0xF) - 10;
        
        if (cs & 3) {
            const char *s4 = " (user)";
            while (*s4) *p++ = *s4++;
        } else {
            const char *s4 = " (kernel)";
            while (*s4) *p++ = *s4++;
        }
        
        *p = '\0';
        
        framebuffer_draw_string(switch_msg, 700, 240 + (switch_count * 20), COLOR_GREEN, 0x00101828);
    }
    
    kernel_tss.rsp0 = tasks[current_task_index].kernel_stack_top;
    
    static int tss_debug_count = 0;
    if (tss_debug_count < 5 && current_task_index == 1) {
        tss_debug_count++;
        char tss_msg[48] = "TSS RSP0 updated: 0x";
        sched_hex_to_string(kernel_tss.rsp0, tss_msg + 21);
        framebuffer_draw_string(tss_msg, 700, 340 + (tss_debug_count * 16), COLOR_MAGENTA, 0x00101828);
    }

    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
        
        static int cr3_switch_count = 0;
        if (cr3_switch_count < 3) {
            cr3_switch_count++;
            framebuffer_draw_string("Address space switched", 700, 400 + (cr3_switch_count * 20), COLOR_CYAN, 0x00101828);
        }
    }

    uint64_t new_rsp = tasks[current_task_index].regs.rsp;
    
    if (current_task_index != 0) {
        if (tasks[current_task_index].regs.cs & 3) {
            if (new_rsp < 0x7FFFFFFFE000 || new_rsp > 0x7FFFFFFFF000) {
                framebuffer_draw_string("PANIC: Invalid user RSP!", 10, 50, COLOR_WHITE, COLOR_RED);
                char rsp_str[32];
                sched_hex_to_string(new_rsp, rsp_str);
                framebuffer_draw_string(rsp_str, 200, 50, COLOR_WHITE, COLOR_RED);
                asm volatile("cli; hlt");
            }
        } else {
            uint64_t stack_base = tasks[current_task_index].kernel_stack_top - KERNEL_STACK_SIZE;
            if (new_rsp < stack_base || new_rsp > tasks[current_task_index].kernel_stack_top) {
                framebuffer_draw_string("PANIC: Invalid kernel RSP!", 10, 50, COLOR_WHITE, COLOR_RED);
                char rsp_str[32];
                sched_hex_to_string(new_rsp, rsp_str);
                framebuffer_draw_string(rsp_str, 200, 50, COLOR_WHITE, COLOR_RED);
                char base_str[32], top_str[32];
                sched_hex_to_string(stack_base, base_str);
                sched_hex_to_string(tasks[current_task_index].kernel_stack_top, top_str);
                framebuffer_draw_string("Expected range:", 10, 70, COLOR_WHITE, COLOR_RED);
                framebuffer_draw_string(base_str, 150, 70, COLOR_WHITE, COLOR_RED);
                framebuffer_draw_string(" - ", 250, 70, COLOR_WHITE, COLOR_RED);
                framebuffer_draw_string(top_str, 280, 70, COLOR_WHITE, COLOR_RED);
                asm volatile("cli; hlt");
            }
        }
    }

    *frame = tasks[current_task_index].regs;
}

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

// NEW: Check if a process has exited children to reap
int sched_check_children(int parent_id, int *status) {
    // Debug
    char msg[64] = "sched_check_children: parent ";
    msg[30] = '0' + parent_id;
    msg[31] = '\0';
    framebuffer_draw_string(msg, 700, 500, COLOR_CYAN, 0x00101828);
    
    // Look for zombie children of this parent
    for (int i = 0; i < next_task_id; i++) {
        // Check validity of task slot
        if (i >= MAX_TASKS) break;
        
        // Debug: show what we're checking
        if (tasks[i].parent_id == parent_id) {
            char cmsg[64] = "  Found child ";
            cmsg[14] = '0' + i;
            cmsg[15] = ' ';
            cmsg[16] = 's';
            cmsg[17] = 't';
            cmsg[18] = 'a';
            cmsg[19] = 't';
            cmsg[20] = 'e';
            cmsg[21] = '=';
            if (tasks[i].state == TASK_STATE_ZOMBIE) cmsg[22] = 'Z';
            else if (tasks[i].state == TASK_STATE_READY) cmsg[22] = 'R';
            else if (tasks[i].state == TASK_STATE_RUNNING) cmsg[22] = 'N';
            else if (tasks[i].state == TASK_STATE_BLOCKED) cmsg[22] = 'B';
            else cmsg[22] = '?';
            cmsg[23] = '\0';
            framebuffer_draw_string(cmsg, 700, 520, COLOR_CYAN, 0x00101828);
        }
        
        // Only check zombie children
        if (tasks[i].state == TASK_STATE_ZOMBIE && 
            tasks[i].parent_id == parent_id) {
            // Found a zombie child
            if (status) {
                *status = tasks[i].exit_status;
            }
            framebuffer_draw_string("  Returning zombie child!", 700, 540, COLOR_GREEN, 0x00101828);
            return i; // Return child PID
        }
    }
    framebuffer_draw_string("  No zombie children found", 700, 540, COLOR_YELLOW, 0x00101828);
    return -1; // No zombie children
}

// NEW: Mark a child as an orphan when parent dies
void sched_orphan_children(int parent_id) {
    for (int i = 0; i < next_task_id; i++) {
        if (i >= MAX_TASKS) break;
        
        if (tasks[i].parent_id == parent_id && tasks[i].state != TASK_STATE_ZOMBIE) {
            // Reparent to init (task 0)
            tasks[i].parent_id = 0;
            
            // Debug
            char msg[64] = "Orphaned task ";
            msg[14] = '0' + i;
            msg[15] = ' ';
            msg[16] = 't';
            msg[17] = 'o';
            msg[18] = ' ';
            msg[19] = 'i';
            msg[20] = 'n';
            msg[21] = 'i';
            msg[22] = 't';
            msg[23] = '\0';
            framebuffer_draw_string(msg, 700, 560, COLOR_YELLOW, 0x00101828);
        }
    }
}

// NEW: Reap a zombie task
void sched_reap_zombie(int task_id) {
    if (task_id < 0 || task_id >= next_task_id || task_id >= MAX_TASKS) return;
    if (tasks[task_id].state != TASK_STATE_ZOMBIE) return;
    
    // Debug: show what we're reaping
    char msg[64] = "Reaping zombie task ";
    msg[20] = '0' + task_id;
    msg[21] = ' ';
    msg[22] = 'w';
    msg[23] = 'i';
    msg[24] = 't';
    msg[25] = 'h';
    msg[26] = ' ';
    msg[27] = 's';
    msg[28] = 't';
    msg[29] = 'a';
    msg[30] = 't';
    msg[31] = 'u';
    msg[32] = 's';
    msg[33] = ' ';
    msg[34] = '0' + tasks[task_id].exit_status;
    msg[35] = '\0';
    framebuffer_draw_string(msg, 700, 620, COLOR_YELLOW, 0x00101828);
    
    // Free kernel stack
    uint64_t kstack_base = tasks[task_id].kernel_stack_top - KERNEL_STACK_SIZE;
    uint64_t kstack_phys = kstack_base - g_hhdm_offset;
    pmm_free_pages((void*)kstack_phys, KERNEL_STACK_SIZE / PAGE_SIZE);
    
    // Clear the task slot
    memset(&tasks[task_id], 0, sizeof(task_t));
}