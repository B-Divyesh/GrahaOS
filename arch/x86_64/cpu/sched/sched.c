#include "sched.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../../../drivers/video/framebuffer.h"

static task_t tasks[MAX_TASKS];
static int next_task_id = 0;
static int current_task_index = 0;

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

    // Task 0 is the kernel task
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space());
    
    current_task_index = 0;
}

int sched_create_task(void (*entry_point)(void)) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;

    // Allocate a kernel stack for the new task.
    uint64_t stack_phys = (uint64_t)pmm_alloc_page();
    if (!stack_phys) return -1;
    tasks[id].kernel_stack_top = stack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // Set up the initial register state for the new task.
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    tasks[id].regs.rip = (uint64_t)entry_point;
    tasks[id].regs.cs = 0x08; // Kernel code segment
    tasks[id].regs.rflags = 0x202; // Interrupts enabled
    tasks[id].regs.rsp = (tasks[id].kernel_stack_top - 16) & ~0xF;
    tasks[id].regs.ss = 0x10; // Kernel data segment
    
    tasks[id].cr3 = vmm_get_pml4_phys(vmm_get_kernel_space()); // Kernel address space

    return id;
}

// Simple function to convert a hex value to a string (kept for useful debugging)
static void sched_hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17]; // 16 hex digits + null terminator
    int i = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0 && i < 16) {
        temp[i++] = hex_chars[value & 0xF];
        value >>= 4;
    }

    // Reverse the string
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}

int sched_create_user_process(void *elf_data) {
    if (next_task_id >= MAX_TASKS) {
        framebuffer_draw_string("Error: MAX_TASKS reached", 10, 400, COLOR_RED, 0x00101828);
        return -1;
    }

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY;

    // Allocate a kernel stack for this process (for syscalls/interrupts)
    uint64_t kstack_phys = (uint64_t)pmm_alloc_page();
    if (!kstack_phys) {
        framebuffer_draw_string("Error: Failed to allocate kernel stack", 10, 400, COLOR_RED, 0x00101828);
        return -1;
    }
    tasks[id].kernel_stack_top = kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // Create a new address space for the process
    vmm_address_space_t *proc_space = vmm_create_address_space();
    if (!proc_space) {
        framebuffer_draw_string("Error: Failed to create address space", 10, 400, COLOR_RED, 0x00101828);
        return -1;
    }

    // --- FINAL BUG FIX: Calculate RIP Offset ---
    // The physical address of the user code symbol.
    uint64_t user_code_phys_addr = (uint64_t)elf_data;
    // The offset of the code within its 4K page.
    uint64_t page_offset = user_code_phys_addr & 0xFFF;
    // The page-aligned physical address of the page containing the code.
    uint64_t user_code_phys = user_code_phys_addr & ~(PAGE_SIZE - 1);
    
    // Debug the physical address (kept - this is useful and not performance-critical)
    char phys_str[32];
    sched_hex_to_string(user_code_phys, phys_str);
    framebuffer_draw_string("User code phys in sched: 0x", 10, 420, COLOR_CYAN, 0x00101828);
    framebuffer_draw_string(phys_str, 200, 420, COLOR_CYAN, 0x00101828);
    
    // --- FINAL BUG FIX: Correct Page Permissions ---
    // Map the physical page containing the code to virtual address 0x400000.
    // The page should be Present, User-accessible, and Executable (NOT Writable).
    if (!vmm_map_page(proc_space, 0x400000, user_code_phys, PTE_PRESENT | PTE_USER)) {
        framebuffer_draw_string("Error: Failed to map user code", 10, 440, COLOR_RED, 0x00101828);
        return -1;
    }

    // Allocate and map a stack for the user process at 0x700000
    uint64_t ustack_phys = (uint64_t)pmm_alloc_page();
    if (!ustack_phys) {
        framebuffer_draw_string("Error: Failed to allocate user stack", 10, 460, COLOR_RED, 0x00101828);
        return -1;
    }
    // Map the user stack with proper permissions: Present, User-accessible, Writable, and NOT Executable.
    if (!vmm_map_page(proc_space, 0x700000, ustack_phys, PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX)) {
        framebuffer_draw_string("Error: Failed to map user stack", 10, 480, COLOR_RED, 0x00101828);
        return -1;
    }

    tasks[id].cr3 = vmm_get_pml4_phys(proc_space);

    // Set up the initial register state to enter user mode via iretq
    memset(&tasks[id].regs, 0, sizeof(struct interrupt_frame));
    
    // --- FINAL BUG FIX: Set the Correct RIP ---
    // The instruction pointer must point to the start of the virtual page
    // PLUS the offset the code was at in its original page.
    tasks[id].regs.rip = 0x400000 + page_offset;
    
    tasks[id].regs.rflags = 0x202; // Interrupts enabled
    tasks[id].regs.rsp = 0x700000 + PAGE_SIZE;

    // User Code is at selector 0x20, User Data is at selector 0x18
    // The RPL (3) must be OR'd in.
    tasks[id].regs.cs = 0x20 | 3; // User Code Selector (0x20) + RPL 3
    tasks[id].regs.ss = 0x18 | 3; // User Data Selector (0x18) + RPL 3

    // Debug output for successful process creation (kept - useful and not performance-critical)
    framebuffer_draw_string("User process created successfully!", 10, 500, COLOR_GREEN, 0x00101828);

    return id;
}

void schedule(struct interrupt_frame *frame) {
    // Save state of the current task
    tasks[current_task_index].regs = *frame;
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    // Find the next ready task (round-robin)
    int next_task_index = (current_task_index + 1) % next_task_id;
    while (tasks[next_task_index].state != TASK_STATE_READY) {
        next_task_index = (next_task_index + 1) % next_task_id;
    }
    
    current_task_index = next_task_index;
    tasks[current_task_index].state = TASK_STATE_RUNNING;

    // Switch address space if necessary
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
    }

    // Load state of the new task into the interrupt frame
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