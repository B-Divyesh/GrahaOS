// arch/x86_64/cpu/syscall/syscall.c
#include "syscall.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../gdt.h"
#include "../../../../kernel/fs/vfs.h"
#include "../../../../kernel/gcp.h"
#include "../../../../kernel/elf.h"
#include "../sched/sched.h"
#include "../../../../kernel/initrd.h"

// MSR definitions
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);

static void write_msr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
    // Enable syscall/sysret instructions
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | 1);
    
    // Set up STAR MSR with kernel and user segments
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);
    
    // Set syscall entry point
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    // CRITICAL FIX: Set MSR_KERNEL_GS_BASE to point to per-CPU data
    // At this point, GS_BASE is already set up by smp_init()
    uint32_t cpu_id = smp_get_current_cpu();
    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)&g_cpu_locals[cpu_id]);
    
    // Clear interrupt flag on syscall
    write_msr(MSR_FMASK, 0x200);
}

// Standard C functions (needed for safe copies)
extern void *memcpy(void *dest, const void *src, size_t n);

// Terminal cursor position for SYS_PUTC
static uint32_t term_x = 0;
static uint32_t term_y = 0;

// Enhanced debug variables that assembly can write to
volatile uint64_t syscall_entry_reached = 0;
volatile uint64_t syscall_about_to_return = 0;
volatile uint64_t syscall_frame_created = 0;
volatile uint64_t syscall_pre_dispatch = 0;
volatile uint64_t syscall_stack_switched = 0;

// Helper to safely copy string from user-space
static int copy_string_from_user(const char *user_src, char *k_dest, size_t max_len) {
    if (user_src == NULL || k_dest == NULL) return -1;
    size_t i = 0;
    for (i = 0; i < max_len - 1; ++i) {
        k_dest[i] = user_src[i];
        if (user_src[i] == '\0') return i + 1;
    }
    k_dest[max_len - 1] = '\0';
    return max_len;
}

// Forward declaration for functions in sched.c
extern int sched_check_children(int parent_id, int *status);
extern void sched_orphan_children(int parent_id);
extern void wake_waiting_parent(int child_id);

// CRITICAL: Special return value for wait() when it needs to block
#define WAIT_BLOCK_RETRY 0x80000000

// The C-level dispatcher
void syscall_dispatcher(struct syscall_frame *frame) {

    if (!frame) {
        framebuffer_draw_string("PANIC: NULL syscall frame!", 10, 10, COLOR_WHITE, COLOR_RED);
        asm volatile("cli; hlt");
    }

    uint64_t syscall_num = frame->int_no;
    
    switch (syscall_num) {
        case SYS_PUTC: {
            char c = (char)frame->rdi;
            if (c == '\n') {
                term_x = 0;
                term_y += 16;
            } else if (c == '\b') {
                if (term_x >= 8) {
                    term_x -= 8;
                    framebuffer_draw_rect(term_x, term_y, 8, 16, 0x00101828);
                }
            } else {
                framebuffer_draw_char(c, term_x, term_y, COLOR_WHITE);
                term_x += 8;
            }
            if (term_x >= framebuffer_get_width() - 20) {
                term_x = 0;
                term_y += 16;
            }
            if (term_y >= framebuffer_get_height() - 20) {
                framebuffer_clear(0x00101828);
                term_x = 0;
                term_y = 0;
            }
            frame->rax = 0;
            break;
        }

        case SYS_OPEN: {
            const char *pathname_user = (const char *)frame->rdi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                frame->rax = vfs_open(pathname_kernel);
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_READ: {
            int fd = (int)frame->rdi;
            void *buffer_user = (void *)frame->rsi;
            size_t count = (size_t)frame->rdx;
            frame->rax = vfs_read(fd, buffer_user, count);
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)frame->rdi;
            frame->rax = vfs_close(fd);
            break;
        }

        case SYS_GCP_EXECUTE: {
            gcp_command_t *user_cmd = (gcp_command_t *)frame->rdi;
            gcp_command_t kernel_cmd;

            memcpy(&kernel_cmd, user_cmd, sizeof(gcp_command_t));

            switch (kernel_cmd.command_id) {
                case GCP_CMD_DRAW_RECT: {
                    framebuffer_draw_rect(
                        kernel_cmd.params.draw_rect.x,
                        kernel_cmd.params.draw_rect.y,
                        kernel_cmd.params.draw_rect.width,
                        kernel_cmd.params.draw_rect.height,
                        kernel_cmd.params.draw_rect.color
                    );
                    frame->rax = 0;
                    break;
                }
                case GCP_CMD_DRAW_STRING: {
                    kernel_cmd.params.draw_string.text[GCP_MAX_STRING_LEN - 1] = '\0';
                    framebuffer_draw_string(
                        kernel_cmd.params.draw_string.text,
                        kernel_cmd.params.draw_string.x,
                        kernel_cmd.params.draw_string.y,
                        kernel_cmd.params.draw_string.fg_color,
                        kernel_cmd.params.draw_string.bg_color
                    );
                    frame->rax = 0;
                    break;
                }
                default:
                    frame->rax = (uint64_t)-2;
                    break;
            }
            break;
        }

        case SYS_GETC: {
            char c;
            asm volatile("sti");
            
            while ((c = keyboard_getchar()) == 0) {
                asm ("hlt");
            }
            
            asm volatile("cli");
            
            frame->rax = c;
            break;
        }

        case SYS_EXEC: {
            const char *path_user = (const char *)frame->rdi;
            char path_kernel[256];
            
            uint64_t flags;
            asm volatile("pushfq; pop %0; cli" : "=r"(flags));
            
            framebuffer_draw_string("SYS_EXEC: Starting...", 400, 400, COLOR_YELLOW, 0x00101828);
            
            if (copy_string_from_user(path_user, path_kernel, sizeof(path_kernel)) <= 0) {
                framebuffer_draw_string("SYS_EXEC: Bad path", 400, 420, COLOR_RED, 0x00101828);
                frame->rax = -1;
                asm volatile("push %0; popfq" : : "r"(flags));
                break;
            }
            
            framebuffer_draw_string("SYS_EXEC: Path=", 400, 440, COLOR_CYAN, 0x00101828);
            framebuffer_draw_string(path_kernel, 520, 440, COLOR_CYAN, 0x00101828);

            size_t file_size;
            void *file_data = initrd_lookup(path_kernel, &file_size);

            if (file_data == NULL) {
                framebuffer_draw_string("SYS_EXEC: File not found!", 400, 460, COLOR_RED, 0x00101828);
                frame->rax = -1;
                asm volatile("push %0; popfq" : : "r"(flags));
                break;
            }
            
            framebuffer_draw_string("SYS_EXEC: File found, loading ELF...", 400, 480, COLOR_YELLOW, 0x00101828);

            uint64_t entry_point, cr3;
            if (!elf_load(file_data, &entry_point, &cr3)) {
                framebuffer_draw_string("SYS_EXEC: ELF load failed!", 400, 500, COLOR_RED, 0x00101828);
                frame->rax = -2;
                asm volatile("push %0; popfq" : : "r"(flags));
                break;
            }
            
            framebuffer_draw_string("SYS_EXEC: Creating process...", 400, 520, COLOR_YELLOW, 0x00101828);

            int pid = sched_create_user_process(entry_point, cr3);
            if (pid < 0) {
                framebuffer_draw_string("SYS_EXEC: Process creation failed!", 400, 540, COLOR_RED, 0x00101828);
                frame->rax = -3;
            } else {
                framebuffer_draw_string("SYS_EXEC: Success!", 400, 560, COLOR_GREEN, 0x00101828);
                frame->rax = pid;
            }
            
            asm volatile("push %0; popfq" : : "r"(flags));
            break;
        }
        
        case SYS_EXIT: {
            int status = (int)frame->rdi;
            
            task_t *current = sched_get_current_task();
            if (!current) {
                framebuffer_draw_string("FATAL: No current task in SYS_EXIT!", 10, 600, COLOR_RED, 0x00101828);
                while (1) asm volatile("hlt");
            }
            
            if (current->state == TASK_STATE_ZOMBIE) {
                frame->rax = -1;
                break;
            }
            
            current->exit_status = status;
            current->state = TASK_STATE_ZOMBIE;
            
            sched_orphan_children(current->id);
            wake_waiting_parent(current->id);
            
            framebuffer_draw_string("Process exited", 10, 600, COLOR_YELLOW, 0x00101828);
            
            frame->rax = 0;
            break;
        }

        case SYS_WAIT: {
            int *status_ptr = (int *)frame->rdi;
            
            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = -1;
                break;
            }
            
            // Debug: show current state
            char debug_msg[80] = "wait(): Current task ";
            debug_msg[21] = '0' + current->id;
            debug_msg[22] = ' ';
            debug_msg[23] = 'c';
            debug_msg[24] = 'h';
            debug_msg[25] = 'e';
            debug_msg[26] = 'c';
            debug_msg[27] = 'k';
            debug_msg[28] = 'i';
            debug_msg[29] = 'n';
            debug_msg[30] = 'g';
            debug_msg[31] = '\0';
            framebuffer_draw_string(debug_msg, 400, 620, COLOR_CYAN, 0x00101828);
            
            // First, check if we have any children at all
            int has_children = 0;
            for (int i = 0; i < MAX_TASKS; i++) {
                task_t *task = sched_get_task(i);
                if (task && task->parent_id == current->id) {
                    has_children = 1;
                    break;
                }
            }
            
            // Check for zombie children
            int exit_status;
            int child_pid = sched_check_children(current->id, &exit_status);
            
            if (child_pid >= 0) {
                // Found a zombie child - reap it immediately
                if (status_ptr) {
                    *status_ptr = exit_status;
                }
                sched_reap_zombie(child_pid);
                frame->rax = child_pid;
                
                framebuffer_draw_string("wait(): Found and reaped zombie child", 400, 640, COLOR_GREEN, 0x00101828);
            } else if (!has_children) {
                // No children at all (neither alive nor zombie)
                frame->rax = -1;
                framebuffer_draw_string("wait(): No children to wait for", 400, 660, COLOR_YELLOW, 0x00101828);
            } else {
                // We have living children - need to block and wait
                current->state = TASK_STATE_BLOCKED;
                current->waiting_for_child = -1;
                
                // CRITICAL: We need to ensure that when we wake up, we retry the wait
                // The simplest way is to return a special value that the user-space
                // wrapper will recognize and retry
                frame->rax = -99; // Special "retry" value
                
                framebuffer_draw_string("wait(): Parent blocked waiting for children", 400, 660, COLOR_YELLOW, 0x00101828);
            }
            break;
        }

        default:
            frame->rax = (uint64_t)-1;
            break;
    }
}