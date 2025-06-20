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

    // Allocate a DEDICATED kernel stack for this new task.
    void* kstack_phys = pmm_alloc_page();
    if (!kstack_phys) return -1;
    tasks[id].kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // --- THE DEFINITIVE FIX ---
    // Map the newly allocated physical page for the kernel stack into the kernel's virtual address space
    uint64_t kstack_base = tasks[id].kernel_stack_top - KERNEL_STACK_SIZE;
    vmm_map_page(vmm_get_kernel_space(), kstack_base, (uint64_t)kstack_phys, PTE_PRESENT | PTE_WRITABLE);

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

// CRITICAL FIX: Now allocates and maps the user-space stack
int sched_create_user_process(uint64_t rip, uint64_t cr3) {
    if (next_task_id >= MAX_TASKS) return -1;

    int id = next_task_id++;
    tasks[id].id = id;
    tasks[id].state = TASK_STATE_READY; // The new process is ready to run
    tasks[id].cr3 = cr3;

    // Allocate a DEDICATED kernel stack for this new process.
    void* kstack_phys = pmm_alloc_page();
    if (!kstack_phys) return -1;
    tasks[id].kernel_stack_top = (uint64_t)kstack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // --- THE DEFINITIVE FIX ---
    // Map the newly allocated physical page for the kernel stack into the kernel's virtual address space
    uint64_t kstack_base = tasks[id].kernel_stack_top - KERNEL_STACK_SIZE;
    vmm_map_page(vmm_get_kernel_space(), kstack_base, (uint64_t)kstack_phys, PTE_PRESENT | PTE_WRITABLE);

    // --- THE CRITICAL FIX ---
    // We must allocate and map the user-space stack.
    uint64_t user_stack_addr = 0x7FFFFFFFF000;
    void* user_stack_phys = pmm_alloc_page();
    if (!user_stack_phys) {
        // Clean up previously allocated kernel stack
        pmm_free_page(kstack_phys);
        return -1;
    }

    // We need a pointer to the process's address space to map the new page.
    // Since we don't have a direct reverse lookup, we'll find it by iterating
    // through the address space pool.
    vmm_address_space_t* proc_space = NULL;
    for(int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (vmm_get_pml4_phys(&address_space_pool[i]) == cr3) {
            proc_space = &address_space_pool[i];
            break;
        }
    }

    if (proc_space == NULL) {
        // This should never happen if elf_load was successful
        pmm_free_page(kstack_phys);
        pmm_free_page(user_stack_phys);
        return -1;
    }

    // Map the physical stack page to the virtual user stack address.
    // The virtual address for mapping is the base of the page.
    uint64_t user_stack_page_base = user_stack_addr - PAGE_SIZE;
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    if (!vmm_map_page(proc_space, user_stack_page_base, (uint64_t)user_stack_phys, flags)) {
        // Clean up allocated pages
        pmm_free_page(kstack_phys);
        pmm_free_page(user_stack_phys);
        return -1;
    }
    
    // Debug: Show user stack mapping (moved to right side)
    char stack_msg[64] = "User stack mapped: 0x";
    uint64_t stack_base = user_stack_page_base;
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (stack_base >> (28 - i * 4)) & 0xF;
        stack_msg[22 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    stack_msg[30] = '\0';
    framebuffer_draw_string(stack_msg, 700, 60, COLOR_CYAN, 0x00101828);

    // Debug output for the provided parameters (moved to right side)
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
    tasks[id].regs.rsp = user_stack_addr; // Set RSP to the top of the mapped page
    tasks[id].regs.cs = 0x20 | 3; // User Code Selector (0x20) + RPL 3
    tasks[id].regs.ss = 0x18 | 3; // User Data Selector (0x18) + RPL 3

    // --- CRITICAL FOR SWAPGS MECHANISM ---
    // Initialize RCX and R11 for proper sysretq handling
    // RCX will hold the user RIP, R11 will hold user RFLAGS
    tasks[id].regs.rcx = rip;       // User return address
    tasks[id].regs.r11 = 0x202;    // User RFLAGS

    // Debug output for successful process creation (moved to right side)
    framebuffer_draw_string("User process created successfully!", 700, 120, COLOR_GREEN, 0x00101828);
    framebuffer_draw_string("SWAPGS registers initialized", 700, 140, COLOR_GREEN, 0x00101828);

    return id;
}

// External variable from syscall.c for debugging
extern volatile uint64_t syscall_entry_reached;
extern volatile uint64_t syscall_about_to_return;
extern volatile uint64_t syscall_stack_switched;

void schedule(struct interrupt_frame *frame) {
    // Debug: Check if syscall entry and return were reached (moved to right side)
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
    
    // --- CRITICAL SWAPGS FIX ---
    // Save the complete interrupt frame state to the current task
    // This is essential for the SWAPGS mechanism to work correctly
    tasks[current_task_index].regs = *frame;
    
    // For user processes, we need to ensure RCX and R11 are properly maintained
    // since they're critical for sysretq after SWAPGS
    if (current_task_index > 0) { // User process
        // Debug: Show critical register preservation
        static int reg_debug_count = 0;
        reg_debug_count++;
        if (reg_debug_count <= 3) {
            char rcx_msg[32] = "Saved RCX: 0x";
            uint64_t rcx_val = frame->rcx;
            for (int i = 0; i < 8; i++) {
                uint8_t nibble = (rcx_val >> (28 - i * 4)) & 0xF;
                rcx_msg[14 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
            }
            rcx_msg[22] = '\0';
            framebuffer_draw_string(rcx_msg, 700, 220 + reg_debug_count * 16, COLOR_YELLOW, 0x00101828);
        }
    }
    
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    // --- THE DEFINITIVE FIX ---
    // This loop correctly finds the next available ready task.
    int original_task = current_task_index;
    do {
        current_task_index = (current_task_index + 1) % next_task_id;
        // If we've looped all the way around and found nothing, just stay on the original task.
        if (current_task_index == original_task) {
            break;
        }
    } while (tasks[current_task_index].state != TASK_STATE_READY);

    // Mark the chosen task as running
    tasks[current_task_index].state = TASK_STATE_RUNNING;

    // Debug output for task switching (moved to right side and enhanced)
    char switch_msg[64];
    switch_msg[0] = 'S'; switch_msg[1] = 'w'; switch_msg[2] = 'i'; switch_msg[3] = 't';
    switch_msg[4] = 'c'; switch_msg[5] = 'h'; switch_msg[6] = ' '; switch_msg[7] = 't';
    switch_msg[8] = 'o'; switch_msg[9] = ' '; switch_msg[10] = 't'; switch_msg[11] = 'a';
    switch_msg[12] = 's'; switch_msg[13] = 'k'; switch_msg[14] = ' ';
    switch_msg[15] = '0' + current_task_index; 
    switch_msg[16] = ' '; switch_msg[17] = 'R'; switch_msg[18] = 'I'; switch_msg[19] = 'P';
    switch_msg[20] = '='; switch_msg[21] = '0'; switch_msg[22] = 'x';
    // Add hex representation of RIP
    uint64_t rip = tasks[current_task_index].regs.rip;
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (rip >> (28 - i * 4)) & 0xF;
        switch_msg[23 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    switch_msg[31] = '\0';
    framebuffer_draw_string(switch_msg, 700, 300 + (current_task_index * 20), COLOR_GREEN, 0x00101828);
    
    // --- THE DEFINITIVE FIX FOR SWAPGS ---
    // Update the TSS's rsp0 to point to the new task's kernel stack.
    // This is CRITICAL for the SWAPGS mechanism - the syscall handler reads gs:[4]
    // which points to TSS.rsp0, so this must be updated before any syscalls.
    uint64_t old_rsp0 = kernel_tss.rsp0;
    kernel_tss.rsp0 = tasks[current_task_index].kernel_stack_top;
    
    // Debug: Show TSS RSP0 update (moved to right side and enhanced)
    static int tss_debug_count = 0;
    tss_debug_count++;
    if (tss_debug_count <= 5 || current_task_index == 1) {  // Show for user task switches
        char tss_msg[48] = "TSS RSP0 updated: 0x";
        uint64_t rsp0 = kernel_tss.rsp0;
        for (int i = 0; i < 8; i++) {
            uint8_t nibble = (rsp0 >> (28 - i * 4)) & 0xF;
            tss_msg[21 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        tss_msg[29] = '\0';
        framebuffer_draw_string(tss_msg, 700, 340 + (tss_debug_count * 16), COLOR_MAGENTA, 0x00101828);
        
        // Show change in TSS value
        if (old_rsp0 != kernel_tss.rsp0) {
            framebuffer_draw_string("TSS RSP0 CHANGED for SWAPGS!", 700, 360 + (tss_debug_count * 16), COLOR_RED, 0x00101828);
        }
    }

    // Switch address space if necessary
    uint64_t current_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != tasks[current_task_index].cr3) {
        vmm_switch_address_space_phys(tasks[current_task_index].cr3);
        
        // Debug: Show address space switch
        framebuffer_draw_string("Address space switched", 700, 400, COLOR_CYAN, 0x00101828);
    }

    // --- CRITICAL FOR SWAPGS MECHANISM ---
    // Load the complete state of the new task into the interrupt frame
    // This ensures that RCX and R11 are properly restored for user processes
    *frame = tasks[current_task_index].regs;
    
    // Debug: Verify frame restoration for user processes
    if (current_task_index > 0) { // User process
        static int frame_debug_count = 0;
        frame_debug_count++;
        if (frame_debug_count <= 3) {
            char frame_msg[32] = "Restored RCX: 0x";
            uint64_t restored_rcx = frame->rcx;
            for (int i = 0; i < 8; i++) {
                uint8_t nibble = (restored_rcx >> (28 - i * 4)) & 0xF;
                frame_msg[16 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
            }
            frame_msg[24] = '\0';
            framebuffer_draw_string(frame_msg, 700, 400 + frame_debug_count * 16, COLOR_GREEN, 0x00101828);
            
            framebuffer_draw_string("Frame restored for SWAPGS", 700, 420 + frame_debug_count * 16, COLOR_GREEN, 0x00101828);
        }
    }
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