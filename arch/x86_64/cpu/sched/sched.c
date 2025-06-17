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
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING;

    // Capture current stack pointer for task 0
    asm volatile ("mov %%rsp, %0" : "=r"(tasks[0].kernel_stack_top));

    current_task_index = 0;
}

int sched_create_task(void (*entry_point)(void)) {
    if (next_task_id >= MAX_TASKS) return -1; // No more tasks available

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

    return id;
}

// The core scheduler and context switcher.
void schedule(struct interrupt_frame *frame) {
    // 1. Save the state of the current task.
    tasks[current_task_index].regs = *frame;
    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }

    // 2. Find the next ready task to run (round-robin).
    int next_task_index = (current_task_index + 1) % next_task_id;
    int attempts = 0;
    while (tasks[next_task_index].state != TASK_STATE_READY && attempts < next_task_id) {
        next_task_index = (next_task_index + 1) % next_task_id;
        attempts++;
    }
    // If no ready tasks found, stay with current task
    if (attempts >= next_task_id) {
        tasks[current_task_index].state = TASK_STATE_RUNNING;
        return; // Don't switch
    }

    // 3. Update the current task index.
    current_task_index = next_task_index;
    tasks[current_task_index].state = TASK_STATE_RUNNING;

    // 4. Load the state of the new task.
    *frame = tasks[current_task_index].regs;
}

task_t* sched_get_current_task(void) {
    return &tasks[current_task_index];
}

task_t* sched_get_task(int id) {
    if (id < 0 || id >= MAX_TASKS) {
        return NULL;
    }
    if (tasks[id].state == TASK_STATE_ZOMBIE) {
        return NULL;
    }
    return &tasks[id];
}
