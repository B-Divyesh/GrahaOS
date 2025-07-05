// arch/x86_64/cpu/syscall/syscall.c
#include "syscall.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../gdt.h"
#include "../../../../kernel/fs/vfs.h"
#include "../../../../kernel/gcp.h" // <-- ADDED

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
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | 1);
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)&kernel_tss);
    write_msr(MSR_FMASK, 0x200);
}

// Standard C functions (needed for safe copies)
extern void *memcpy(void *dest, const void *src, size_t n);

// Terminal cursor position for SYS_PUTC
static uint32_t term_x = 410;
static uint32_t term_y = 240;

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

// The C-level dispatcher
void syscall_dispatcher(struct syscall_frame *frame) {
    uint64_t syscall_num = frame->int_no;
    
    switch (syscall_num) {
        case SYS_PUTC: {
            char c = (char)frame->rdi;
            if (c == '\n') {
                term_x = 410;
                term_y += 16;
            } else {
                framebuffer_draw_char(c, term_x, term_y, COLOR_WHITE);
                term_x += 8;
            }
            if (term_x >= framebuffer_get_width() - 20) {
                term_x = 410;
                term_y += 16;
            }
            if (term_y >= framebuffer_get_height() - 20) {
                term_y = 240;
                framebuffer_draw_rect(410, 240, framebuffer_get_width() - 410, framebuffer_get_height() - 240, 0x00101828);
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

        // --- NEW: GCP Syscall Handler ---
        case SYS_GCP_EXECUTE: {
            gcp_command_t *user_cmd = (gcp_command_t *)frame->rdi;
            gcp_command_t kernel_cmd;

            // SECURITY: Copy the command from user-space to a kernel-space buffer
            // to prevent TOCTOU attacks and ensure we're working with safe data.
            // A more advanced kernel would use a copy_from_user function that
            // also validates the user pointer.
            memcpy(&kernel_cmd, user_cmd, sizeof(gcp_command_t));

            // Dispatch based on the command ID from our safe kernel copy
            switch (kernel_cmd.command_id) {
                case GCP_CMD_DRAW_RECT: {
                    framebuffer_draw_rect(
                        kernel_cmd.params.draw_rect.x,
                        kernel_cmd.params.draw_rect.y,
                        kernel_cmd.params.draw_rect.width,
                        kernel_cmd.params.draw_rect.height,
                        kernel_cmd.params.draw_rect.color
                    );
                    frame->rax = 0; // Success
                    break;
                }
                case GCP_CMD_DRAW_STRING: {
                    // Ensure the string is null-terminated within our buffer
                    kernel_cmd.params.draw_string.text[GCP_MAX_STRING_LEN - 1] = '\0';
                    framebuffer_draw_string(
                        kernel_cmd.params.draw_string.text,
                        kernel_cmd.params.draw_string.x,
                        kernel_cmd.params.draw_string.y,
                        kernel_cmd.params.draw_string.fg_color,
                        kernel_cmd.params.draw_string.bg_color
                    );
                    frame->rax = 0; // Success
                    break;
                }
                default:
                    frame->rax = (uint64_t)-2; // Unknown GCP command
                    break;
            }
            break;
        }

        default:
            frame->rax = (uint64_t)-1; // Unknown syscall
            break;
    }
}