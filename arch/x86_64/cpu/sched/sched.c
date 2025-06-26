//arch/x86_64/cpu/sched/sched.c
#include "sched.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../gdt.h" // Include GDT header to access the TSS
#include "../../../../drivers/video/framebuffer.h"

static task_t tasks[MAX_TASKS];
static int next_task_id = 0;
static int current_task_index = 0; // Start with task 0 (kernel task)

// Simple memset implementation
static void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n-- > 0) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));

    // Task 0 is the kernel's idle task.
    // We get its kernel stack from gdt_init.
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING; // The kernel starts as the running task
    tasks[0].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    tasks[0].kernel_stack_top = kernel_tss.rsp0; // Use the stack created in gdt_init
    
    current_task_index = 0;
    
    // Debug: Show scheduler initialization
    framebuffer_draw_string("Scheduler initialized with SWAPGS support", 700, 20, COLOR_GREEN, 0x00101828);
}

int sched_create_task(void (*entry_point)(void)) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;

    // FIXED: Allocate multiple pages for kernel stack
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) return -1;
    tasks[id].kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // Map all pages of the kernel stack
    uint64_t kstack_base = tasks[id].kernel_stack_top - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = kstack_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)kstack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }

    // Set up the initial register state for the new task.
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = (uint64_t)entry_point;
    tasks[id].regs.cs = 0x08; // Kernel code segment
    tasks[id].regs.rflags = 0x202; // Interrupts enabled
    tasks[id].regs.rsp = (tasks[id].kernel_stack_top - 16) & ~0xF;
    tasks[id].regs.ss = 0x10; // Kernel data segment
    
    tasks[id].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space()); // Kernel address space

    // Debug: Show kernel task creation
    framebuffer_draw_string("Kernel task created with dedicated stack", 700, 40, COLOR_CYAN, 0x00101828);

    return id;
}

// FIXED: Proper 64-bit hex conversion
static void sched_hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17]; // 16 hex digits + null terminator
    int i = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    // Process all 16 hex digits for 64-bit value
    for (i = 0; i < 16; i++) {
        temp[i] = hex_chars[(value >> ((15 - i) * 4)) & 0xF];
    }
    temp[16] = '\0';

    // Copy without leading zeros, but keep at least one digit
    int start = 0;
    while (start < 15 && temp[start] == '0') start++;
    
    int j = 0;
    while (start < 16) {
        buffer[j++] = temp[start++];
    }
    buffer[j] = '\0';
}

// CRITICAL FIX: Now allocates and maps the user-space stack
int sched_create_user_process(uint64_t rip, uint64_t cr3) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;
    tasks[id].cr3 = cr3;

    // FIXED: Allocate multiple pages for kernel stack
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void* kstack_phys = pmm_alloc_pages(num_pages);
    if (!kstack_phys) return -1;
    tasks[id].kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // Map all pages of the kernel stack
    uint64_t kstack_base = tasks[id].kernel_stack_top - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = kstack_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)kstack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }

    // We must allocate and map the user-space stack.
    uint64_t user_stack_addr = 0x7FFFFFFFF000;
    void* user_stack_phys = pmm_alloc_page();
    if (!user_stack_phys) {
        // Clean up previously allocated kernel stack
        pmm_free_pages(kstack_phys, num_pages);
        return -1;
    }

    // We need a pointer to the process's address space to map the new page.
    vmm_address_space_t* proc_space = NULL;
    for(int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (vmm_get_pml4_phys(&address_space_pool[i]) == cr3) {
            proc_space = &address_space_pool[i];
            break;
        }
    }

    if (proc_space == NULL) {
        // This should never happen if elf_load was successful
        pmm_free_pages(kstack_phys, num_pages);
        pmm_free_page(user_stack_phys);
        return -1;
    }

    // Map the physical stack page to the virtual user stack address.
    uint64_t user_stack_page_base = user_stack_addr - PAGE_SIZE;
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    if (!vmm_map_page(proc_space, user_stack_page_base, (uint64_t)user_stack_phys, flags)) {
        // Clean up allocated pages
        pmm_free_pages(kstack_phys, num_pages);
        pmm_free_page(user_stack_phys);
        return -1;
    }

    tasks[id].regs.rsp = user_stack_page_base + PAGE_SIZE - 16;
    
    // Debug: Show user stack mapping
    char stack_msg[64] = "User stack mapped: 0x";
    sched_hex_to_string(user_stack_page_base, stack_msg + 22);
    framebuffer_draw_string(stack_msg, 700, 60, COLOR_CYAN, 0x00101828);

    // Debug output for the provided parameters
    char rip_str[32], cr3_str[32];
    sched_hex_to_string(rip, rip_str);
    sched_hex_to_string(cr3, cr3_str);
    framebuffer_draw_string("Creating user process - RIP: 0x", 700, 80, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(rip_str, 950, 80, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string("CR3: 0x", 700, 100, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(cr3_str, 800, 100, COLOR_CYAN, 0x00101828);
    
    // Set up the initial register state to enter user mode
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = rip;
    tasks[id].regs.rflags = 0x202; // Interrupts enabled
    tasks[id].regs.rsp = user_stack_addr ;// Set RSP to the top of the mapped page
    tasks[id].regs.cs = 0x20 | 3; // User Code Selector (0x20) + RPL 3
    tasks[id].regs.ss = 0x18 | 3; // User Data Selector (0x18) + RPL 3

    // Debug output for successful process creation
    framebuffer_draw_string("User process created successfully!", 700, 120, COLOR_GREEN, 0x00101828);

    return id;
}

// External variables from syscall.c for debugging
extern volatile uint64_t syscall_entry_reached;
extern volatile uint64_t syscall_about_to_return;
extern volatile uint64_t syscall_stack_switched;

void schedule(struct interrupt_frame *frame) {
    // Debug: Check if syscall entry and return were reached
    static int check_count = 0;
    check_count++;
    if (check_count == 10) {  // Check after a few timer ticks
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
    
    // CRITICAL FIX: Simply save the state without modification
    // RCX and R11 are allowed to have ANY value in user space, including kernel addresses
    // The CPU's sysretq instruction leaves these in an undefined state, which is NORMAL
    tasks[current_task_index].regs = *frame;
    
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    // Find the next ready task
    int original_task = current_task_index;
    do {
        current_task_index = (current_task_index + 1) % next_task_id;
        if (current_task_index == original_task) {
            break;
        }
    } while (tasks[current_task_index].state != TASK_STATE_READY);

    // Mark the chosen task as running
    tasks[current_task_index].state = TASK_STATE_RUNNING;

    // Debug output for task switching
    static int switch_count = 0;
    if (switch_count < 10) {
        switch_count++;
        char switch_msg[80];
        char *p = switch_msg;
        
        // Build message manually to avoid sprintf
        const char *s1 = "Switch to task ";
        while (*s1) *p++ = *s1++;
        *p++ = '0' + current_task_index;
        
        const char *s2 = " RIP=0x";
        while (*s2) *p++ = *s2++;
        
        // Add RIP in hex
        uint64_t rip = tasks[current_task_index].regs.rip;
        char rip_str[17];
        sched_hex_to_string(rip, rip_str);
        char *rs = rip_str;
        while (*rs) *p++ = *rs++;
        
        // Add CS info to debug user vs kernel mode
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
    
    // Update the TSS's rsp0 to point to the new task's kernel stack.
    kernel_tss.rsp0 = tasks[current_task_index].kernel_stack_top;
    
    // Debug: Show TSS RSP0 update for first few switches
    static int tss_debug_count = 0;
    if (tss_debug_count < 5 && current_task_index == 1) {
        tss_debug_count++;
        char tss_msg[48] = "TSS RSP0 updated: 0x";
        sched_hex_to_string(kernel_tss.rsp0, tss_msg + 21);
        framebuffer_draw_string(tss_msg, 700, 340 + (tss_debug_count * 16), COLOR_MAGENTA, 0x00101828);
    }

    // Switch address space if necessary
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
        
        // Debug: Show address space switch
        static int cr3_switch_count = 0;
        if (cr3_switch_count < 3) {
            cr3_switch_count++;
            framebuffer_draw_string("Address space switched", 700, 400 + (cr3_switch_count * 20), COLOR_CYAN, 0x00101828);
        }
    }

    // Restore the complete state of the next task
    *frame = tasks[current_task_index].regs;
}

task_t* sched_get_current_task(void) {
    return &tasks[current_task_index];
}

task_t* sched_get_task(int id) {
    if (id < 0 || id >= next_task_id || tasks[id].state == TASK_STATE_ZOMBIE) {
        return NULL;
    }
    return &tasks[id];
}