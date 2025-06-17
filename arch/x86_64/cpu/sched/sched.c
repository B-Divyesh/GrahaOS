#include "sched.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h" // <-- Include vmm.h to get the global variable
#include "../../../../drivers/video/framebuffer.h"

// --- REMOVED ---
// extern uint64_t g_hhdm_offset; // This is no longer needed.

static task_t tasks[MAX_TASKS];
static int next_task_id = 0;
static int current_task_id = 0;

// Simple memset implementation
static void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n-- > 0) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void sched_init(void) {
    // Clear the task list
    memset(tasks, 0, sizeof(tasks));

    // Create the initial kernel task (task 0)
    // This task represents the code we are currently running
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_STATE_RUNNING;

    // Allocate a kernel stack for the initial task
    uint64_t stack_phys = (uint64_t)pmm_alloc_page();
    if (!stack_phys) {
        framebuffer_draw_string("SCHED: Failed to allocate kernel stack!", 0, 0, COLOR_RED, COLOR_BLACK);
        for(;;);
    }

    // This will now link correctly because g_hhdm_offset is defined in vmm.c
    tasks[0].kernel_stack = stack_phys + g_hhdm_offset + KERNEL_STACK_SIZE;

    // Initialize register state to zero (we're already running so don't need to set them)
    memset(&tasks[0].regs, 0, sizeof(register_state_t));

    current_task_id = 0;
}

task_t* sched_get_current_task(void) {
    return &tasks[current_task_id];
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
