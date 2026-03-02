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
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../drivers/serial/serial.h"
#include "../../../../kernel/state.h"
#include "../../../../kernel/capability.h"
#include "../../../../kernel/fs/grahafs.h"
#include "../../drivers/e1000/e1000.h"
#include "../../../../kernel/net/net.h"
#include <stdbool.h>

// Forward declarations for state collection (kernel/state.c)
extern int state_get_size(uint32_t category);
extern void state_collect_memory(state_memory_t *out);
extern void state_collect_processes(state_process_list_t *out);
extern void state_collect_filesystem(state_filesystem_t *out);
extern void state_collect_system(state_system_t *out);
extern void state_collect_drivers(state_driver_list_t *out);
extern int state_collect_all(state_snapshot_t *out);
extern void state_collect_capabilities(state_cap_list_t *out);

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

// Validate that a pointer is in user-space (below kernel boundary)
static inline bool is_user_pointer(const void *ptr, size_t size) {
    uint64_t addr = (uint64_t)ptr;
    // User space is below 0x0000800000000000 (canonical address boundary)
    // Reject NULL, kernel addresses, and overflow
    if (addr == 0) return false;
    if (addr >= 0x0000800000000000ULL) return false;
    if (addr + size < addr) return false;  // overflow check
    if (addr + size > 0x0000800000000000ULL) return false;
    return true;
}

// Helper to safely copy string from user-space
static int copy_string_from_user(const char *user_src, char *k_dest, size_t max_len) {
    if (user_src == NULL || k_dest == NULL) return -1;
    if (!is_user_pointer(user_src, 1)) return -1;
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
        serial_write("[SYSCALL] ERROR: NULL frame!\n");
        framebuffer_draw_string("PANIC: NULL syscall frame!", 10, 10, COLOR_WHITE, COLOR_RED);
        asm volatile("cli; hlt");
    }

    uint64_t syscall_num = frame->int_no;
    
    switch (syscall_num) {
        case SYS_PUTC: {
            char c = (char)frame->rdi;
            // DEBUG: Also output to serial for automated testing
            serial_putc(c);
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
            if (!is_user_pointer(buffer_user, count)) {
                frame->rax = -1;
                break;
            }
            frame->rax = vfs_read(fd, buffer_user, count);
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)frame->rdi;
            frame->rax = vfs_close(fd);
            break;
        }

        case SYS_WRITE: {
            int fd = (int)frame->rdi;
            const void *buffer_user = (const void *)frame->rsi;
            size_t count = (size_t)frame->rdx;
            if (!is_user_pointer(buffer_user, count)) {
                frame->rax = -1;
                break;
            }
            frame->rax = vfs_write(fd, (void*)buffer_user, count);
            break;
        }

        case SYS_CREATE: {
            const char *pathname_user = (const char *)frame->rdi;
            uint32_t mode = (uint32_t)frame->rsi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                frame->rax = vfs_create(pathname_kernel, mode);
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_MKDIR: {
            const char *pathname_user = (const char *)frame->rdi;
            uint32_t mode = (uint32_t)frame->rsi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                frame->rax = vfs_mkdir(pathname_kernel, mode);
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_READDIR: {
            const char *pathname_user = (const char *)frame->rdi;
            uint32_t index = (uint32_t)frame->rsi;
            void *dirent_buffer = (void *)frame->rdx;
            
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) <= 0) {
                frame->rax = -1;
                break;
            }
            
            // Open the directory
            vfs_node_t* dir = vfs_path_to_node(pathname_kernel);
            if (!dir) {
                frame->rax = -1;
                break;
            }
            
            if (dir->type != VFS_DIRECTORY) {
                vfs_destroy_node(dir);
                frame->rax = -2; // Not a directory
                break;
            }
            
            // Read the directory entry at index
            vfs_node_t* entry = NULL;
            if (dir->readdir) {
                entry = dir->readdir(dir, index);
            }
            
            if (!entry) {
                vfs_destroy_node(dir);
                frame->rax = 0; // No more entries
                break;
            }
            
            // Copy entry info to user buffer
            // Simple format: 4 bytes type, 28 bytes name
            if (dirent_buffer) {
                if (!is_user_pointer(dirent_buffer, 32)) {
                    vfs_destroy_node(entry);
                    vfs_destroy_node(dir);
                    frame->rax = -1;
                    break;
                }
                uint32_t type = entry->type;
                memcpy(dirent_buffer, &type, 4);
                memcpy((uint8_t*)dirent_buffer + 4, entry->name, 28);
            }
            
            vfs_destroy_node(entry);
            vfs_destroy_node(dir);
            frame->rax = 1; // Success
            break;
        }

        case SYS_SYNC: {
            // Flush all pending writes to disk
            vfs_sync();
            frame->rax = 0;
            break;
        }

        case SYS_BRK: {
            void *addr = (void *)frame->rdi;
            task_t *current = sched_get_current_task();

            serial_write("[SYS_BRK] addr=");
            serial_write_hex((uint64_t)addr);
            serial_write("\n");

            if (!current) {
                serial_write("[SYS_BRK] ERROR: No current process!\n");
                frame->rax = -1;
                break;
            }

            // If addr is NULL, just return current brk
            if (addr == NULL) {
                serial_write("[SYS_BRK] Return current brk=");
                serial_write_hex(current->brk);
                serial_write("\n");
                frame->rax = (uint64_t)current->brk;
                break;
            }

            serial_write("[SYS_BRK] curr_brk=");
            serial_write_hex(current->brk);
            serial_write(" heap_start=");
            serial_write_hex(current->heap_start);
            serial_write("\n");

            // Validate address bounds
            // Must be >= heap_start and < stack_top with guard space
            #define HEAP_STACK_GUARD (16 * 4096)  // 64KB guard

            if ((uint64_t)addr < current->heap_start ||
                (uint64_t)addr >= current->stack_top - HEAP_STACK_GUARD) {
                frame->rax = (uint64_t)-1;  // ENOMEM
                break;
            }

            // Calculate page-aligned boundaries
            #define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
            #define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

            uint64_t old_brk_page = ALIGN_UP(current->brk, PAGE_SIZE);
            uint64_t new_brk_page = ALIGN_UP((uint64_t)addr, PAGE_SIZE);

            if (new_brk_page > old_brk_page) {
                // Growing heap - allocate and map new pages
                bool success = true;
                for (uint64_t page = old_brk_page; page < new_brk_page; page += PAGE_SIZE) {
                    void *phys = pmm_alloc_page();
                    if (!phys) {
                        // Out of memory - rollback what we allocated
                        for (uint64_t p = old_brk_page; p < page; p += PAGE_SIZE) {
                            // Get physical address using vmm
                            uint64_t old_phys = vmm_get_physical_address(current->cr3, p);
                            if (old_phys) {
                                vmm_unmap_page_by_cr3(current->cr3, p);
                                pmm_free_page((void*)old_phys);
                            }
                        }
                        success = false;
                        break;
                    }

                    // Map with user permissions (writable, user-accessible)
                    if (!vmm_map_page_by_cr3(current->cr3, page, (uint64_t)phys,
                                            PTE_PRESENT | PTE_WRITABLE | PTE_USER)) {
                        // Mapping failed - free this page and rollback
                        pmm_free_page(phys);
                        for (uint64_t p = old_brk_page; p < page; p += PAGE_SIZE) {
                            uint64_t old_phys = vmm_get_physical_address(current->cr3, p);
                            if (old_phys) {
                                vmm_unmap_page_by_cr3(current->cr3, p);
                                pmm_free_page((void*)old_phys);
                            }
                        }
                        success = false;
                        break;
                    }
                }

                if (success) {
                    current->brk = (uint64_t)addr;
                    frame->rax = current->brk;
                    serial_write("[SYS_BRK] SUCCESS: new_brk=");
                    serial_write_hex(current->brk);
                    serial_write("\n");
                } else {
                    frame->rax = (uint64_t)-1;  // ENOMEM
                    serial_write("[SYS_BRK] FAILED: Out of memory or mapping failed\n");
                }
            } else if (new_brk_page < old_brk_page) {
                // Shrinking heap - unmap and free pages
                for (uint64_t page = new_brk_page; page < old_brk_page; page += PAGE_SIZE) {
                    uint64_t phys = vmm_get_physical_address(current->cr3, page);
                    if (phys) {
                        vmm_unmap_page_by_cr3(current->cr3, page);
                        pmm_free_page((void*)phys);
                    }
                }

                current->brk = (uint64_t)addr;
                frame->rax = current->brk;
            } else {
                // No page boundary crossed, just update brk
                current->brk = (uint64_t)addr;
                frame->rax = current->brk;
            }

            break;
        }

        case SYS_GCP_EXECUTE: {
            gcp_command_t *user_cmd = (gcp_command_t *)frame->rdi;
            if (!is_user_pointer(user_cmd, sizeof(gcp_command_t))) {
                frame->rax = (uint64_t)-1;
                break;
            }
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

            // Remove all CAN event watchers for this process
            cap_unwatch_all_for_pid(current->id);

            // Unregister all user-owned capabilities for this process
            cap_unregister_by_owner(current->id);

            // Clean up any pending network request
            net_cleanup_task(current->id);

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
                    if (!is_user_pointer(status_ptr, sizeof(int))) {
                        frame->rax = -1;
                        break;
                    }
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

        case SYS_SPAWN: {
            const char *path_user = (const char *)frame->rdi;
            char path_kernel[256];

            if (copy_string_from_user(path_user, path_kernel, sizeof(path_kernel)) <= 0) {
                serial_write("[SYS_SPAWN] Bad path\n");
                frame->rax = -1;
                break;
            }

            serial_write("[SYS_SPAWN] Path: ");
            serial_write(path_kernel);
            serial_write("\n");

            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = -1;
                break;
            }

            int pid = sched_spawn_process(path_kernel, current->id);
            if (pid < 0) {
                serial_write("[SYS_SPAWN] Failed\n");
                frame->rax = -1;
            } else {
                serial_write("[SYS_SPAWN] Success: pid=");
                serial_write_dec(pid);
                serial_write("\n");
                frame->rax = pid;
            }
            break;
        }

        case SYS_KILL: {
            int target_pid = (int)frame->rdi;
            int signal = (int)frame->rsi;

            serial_write("[SYS_KILL] pid=");
            serial_write_dec(target_pid);
            serial_write(" sig=");
            serial_write_dec(signal);
            serial_write("\n");

            frame->rax = sched_send_signal(target_pid, signal);
            break;
        }

        case SYS_SIGNAL: {
            int signal = (int)frame->rdi;
            void (*handler)(int) = (void (*)(int))frame->rsi;

            void *old_handler = sched_set_signal_handler(signal, handler);
            frame->rax = (uint64_t)old_handler;
            break;
        }

        case SYS_GETPID: {
            task_t *current = sched_get_current_task();
            if (current) {
                frame->rax = current->id;
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_GET_SYSTEM_STATE: {
            uint32_t category = (uint32_t)frame->rdi;
            void *user_buf = (void *)frame->rsi;
            size_t buf_size = (size_t)frame->rdx;

            // If user_buf is NULL, return the required size
            int required = state_get_size(category);
            if (required < 0) {
                frame->rax = (uint64_t)-1;
                break;
            }

            if (user_buf == NULL) {
                frame->rax = (uint64_t)required;
                break;
            }

            if ((size_t)required > buf_size) {
                // Buffer too small - return negative of required size
                frame->rax = (uint64_t)(-(long)required);
                break;
            }

            if (!is_user_pointer(user_buf, required)) {
                frame->rax = (uint64_t)-1;
                break;
            }

            // Collect the requested category into user buffer
            switch (category) {
                case STATE_CAT_MEMORY:
                    state_collect_memory((state_memory_t *)user_buf);
                    break;
                case STATE_CAT_PROCESSES:
                    state_collect_processes((state_process_list_t *)user_buf);
                    break;
                case STATE_CAT_FILESYSTEM:
                    state_collect_filesystem((state_filesystem_t *)user_buf);
                    break;
                case STATE_CAT_SYSTEM:
                    state_collect_system((state_system_t *)user_buf);
                    break;
                case STATE_CAT_DRIVERS:
                    state_collect_drivers((state_driver_list_t *)user_buf);
                    break;
                case STATE_CAT_CAPABILITIES:
                    state_collect_capabilities((state_cap_list_t *)user_buf);
                    break;
                case STATE_CAT_ALL:
                    state_collect_all((state_snapshot_t *)user_buf);
                    break;
                default:
                    frame->rax = (uint64_t)-1;
                    break;
            }

            if (frame->rax != (uint64_t)-1) {
                frame->rax = (uint64_t)required;
            }
            break;
        }

        case SYS_CAP_ACTIVATE: {
            // RDI = user string pointer (capability name)
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            int id = cap_find(kname);
            if (id < 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            frame->rax = (uint64_t)(long)cap_activate(id);
            break;
        }

        case SYS_CAP_DEACTIVATE: {
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            int id = cap_find(kname);
            if (id < 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            frame->rax = (uint64_t)(long)cap_deactivate(id);
            break;
        }

        case SYS_CAP_REGISTER: {
            // RDI=name, RSI=type, RDX=dep_names array, R10=dep_count
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NAME_EMPTY;
                break;
            }

            uint32_t cap_type = (uint32_t)frame->rsi;

            // Only APPLICATION, FEATURE, COMPOSITE allowed from user-space
            if (cap_type < CAP_APPLICATION) {
                frame->rax = (uint64_t)(long)CAP_ERR_LAYER_VIOLATION;
                break;
            }

            int dep_count = (int)frame->r10;
            if (dep_count < 0 || dep_count > MAX_CAP_DEPS) {
                frame->rax = (uint64_t)(long)CAP_ERR_DEP_UNRESOLVED;
                break;
            }

            // Copy dep names from user-space
            const char **user_dep_names = (const char **)frame->rdx;
            const char *k_dep_ptrs[MAX_CAP_DEPS];
            char k_dep_bufs[MAX_CAP_DEPS][CAP_NAME_LEN];

            for (int i = 0; i < dep_count; i++) {
                if (!is_user_pointer(user_dep_names, (dep_count) * sizeof(char *))) {
                    frame->rax = (uint64_t)(long)CAP_ERR_DEP_UNRESOLVED;
                    goto cap_reg_done;
                }
                const char *user_dep = user_dep_names[i];
                if (copy_string_from_user(user_dep, k_dep_bufs[i], CAP_NAME_LEN) <= 0) {
                    frame->rax = (uint64_t)(long)CAP_ERR_DEP_UNRESOLVED;
                    goto cap_reg_done;
                }
                k_dep_ptrs[i] = k_dep_bufs[i];
            }

            {
                task_t *current = sched_get_current_task();
                int32_t owner = current ? current->id : -1;
                int ret = cap_register(kname, cap_type, 0, owner,
                                       dep_count > 0 ? k_dep_ptrs : NULL, dep_count,
                                       NULL, NULL, NULL, 0, NULL);
                frame->rax = (uint64_t)(long)ret;
            }
            cap_reg_done:
            break;
        }

        case SYS_CAP_UNREGISTER: {
            // RDI=name
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NAME_EMPTY;
                break;
            }

            int id = cap_find(kname);
            if (id < 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NOT_FOUND;
                break;
            }

            // Verify caller owns the cap
            task_t *current = sched_get_current_task();
            int32_t caller_pid = current ? current->id : -1;
            int32_t cap_owner = cap_get_owner(id);
            if (cap_owner == -1 || cap_owner != caller_pid) {
                frame->rax = (uint64_t)(long)CAP_ERR_KERNEL_OWNED;
                break;
            }

            frame->rax = (uint64_t)(long)cap_unregister(id);
            break;
        }

        // Phase 8c: AI Metadata syscalls
        case SYS_SET_AI_METADATA: {
            // RDI=path, RSI=metadata_ptr
            char kpath[256];
            if (copy_string_from_user((const char *)frame->rdi, kpath, 256) <= 0) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            if (!is_user_pointer((const void *)frame->rsi, sizeof(grahafs_ai_metadata_t))) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            // Resolve path to inode number
            vfs_node_t *node = vfs_path_to_node(kpath);
            if (!node) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }
            uint32_t ino = node->inode;
            vfs_destroy_node(node);

            // Copy metadata from user space to kernel stack
            grahafs_ai_metadata_t kmeta;
            const uint8_t *usrc = (const uint8_t *)frame->rsi;
            uint8_t *kdst = (uint8_t *)&kmeta;
            for (size_t i = 0; i < sizeof(grahafs_ai_metadata_t); i++)
                kdst[i] = usrc[i];

            frame->rax = (uint64_t)(long)grahafs_set_ai_metadata(ino, &kmeta);
            break;
        }

        case SYS_GET_AI_METADATA: {
            // RDI=path, RSI=out_buf
            char kpath[256];
            if (copy_string_from_user((const char *)frame->rdi, kpath, 256) <= 0) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            if (!is_user_pointer((const void *)frame->rsi, sizeof(grahafs_ai_metadata_t))) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            // Resolve path to inode number
            vfs_node_t *node = vfs_path_to_node(kpath);
            if (!node) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }
            uint32_t ino = node->inode;
            vfs_destroy_node(node);

            grahafs_ai_metadata_t kmeta;
            int ret = grahafs_get_ai_metadata(ino, &kmeta);
            if (ret == 0) {
                // Copy result to user space
                uint8_t *udst = (uint8_t *)frame->rsi;
                const uint8_t *ksrc = (const uint8_t *)&kmeta;
                for (size_t i = 0; i < sizeof(grahafs_ai_metadata_t); i++)
                    udst[i] = ksrc[i];
            }

            frame->rax = (uint64_t)(long)ret;
            break;
        }

        case SYS_SEARCH_BY_TAG: {
            // RDI=tag_str, RSI=results_buf, RDX=max_results
            char ktag[96];
            if (copy_string_from_user((const char *)frame->rdi, ktag, 96) <= 0) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            if (!is_user_pointer((const void *)frame->rsi, sizeof(grahafs_search_results_t))) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            int max = (int)frame->rdx;
            if (max <= 0 || max > 16) max = 16;

            grahafs_search_results_t kresults;
            int ret = grahafs_search_by_tag(ktag, &kresults, max);

            if (ret >= 0) {
                // Copy results to user space
                uint8_t *udst = (uint8_t *)frame->rsi;
                const uint8_t *ksrc = (const uint8_t *)&kresults;
                for (size_t i = 0; i < sizeof(grahafs_search_results_t); i++)
                    udst[i] = ksrc[i];
            }

            frame->rax = (uint64_t)(long)ret;
            break;
        }

        // Phase 8d: CAN Event Propagation
        case SYS_CAP_WATCH: {
            // RDI = capability name to watch
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NOT_FOUND;
                break;
            }
            int id = cap_find(kname);
            if (id < 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NOT_FOUND;
                break;
            }
            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }
            frame->rax = (uint64_t)(long)cap_watch(id, current->id);
            break;
        }

        case SYS_CAP_UNWATCH: {
            // RDI = capability name to unwatch
            char kname[32];
            if (copy_string_from_user((const char *)frame->rdi, kname, 32) <= 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NOT_FOUND;
                break;
            }
            int id = cap_find(kname);
            if (id < 0) {
                frame->rax = (uint64_t)(long)CAP_ERR_NOT_FOUND;
                break;
            }
            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }
            frame->rax = (uint64_t)(long)cap_unwatch(id, current->id);
            break;
        }

        case SYS_CAP_POLL: {
            // RDI = event buffer pointer, RSI = max events
            void *user_buf = (void *)frame->rdi;
            int max_events = (int)frame->rsi;

            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            if (max_events <= 0) max_events = 1;
            if (max_events > STATE_CAP_EVENT_QUEUE_SIZE) max_events = STATE_CAP_EVENT_QUEUE_SIZE;

            int pending = sched_pending_event_count(current->id);

            if (pending == 0) {
                if (user_buf == NULL) {
                    // Non-blocking query: just return 0 pending
                    frame->rax = 0;
                } else {
                    // Blocking mode: block and retry
                    current->event_waiting = 1;
                    current->state = TASK_STATE_BLOCKED;
                    frame->rax = (uint64_t)(long)-99;  // Retry signal
                }
                break;
            }

            if (!user_buf || !is_user_pointer(user_buf, max_events * sizeof(state_cap_event_t))) {
                // Just return the count if buffer is invalid
                frame->rax = (uint64_t)(long)pending;
                break;
            }

            // Dequeue up to max_events
            state_cap_event_t *out = (state_cap_event_t *)user_buf;
            int count = 0;
            state_cap_event_t tmp;
            while (count < max_events && sched_dequeue_cap_event(current->id, &tmp)) {
                // Copy event to user buffer
                uint8_t *dst = (uint8_t *)&out[count];
                const uint8_t *src = (const uint8_t *)&tmp;
                for (size_t i = 0; i < sizeof(state_cap_event_t); i++)
                    dst[i] = src[i];
                count++;
            }
            frame->rax = (uint64_t)(long)count;
            break;
        }

        // Phase 9a: Network ifconfig
        case SYS_NET_IFCONFIG: {
            // RDI = user buffer pointer (at least 7 bytes: 6 MAC + 1 link_up)
            void *user_buf = (void *)frame->rdi;
            if (!user_buf || !is_user_pointer(user_buf, 7)) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            if (!e1000_is_present()) {
                frame->rax = (uint64_t)(long)-2;  // No NIC
                break;
            }

            uint8_t *out = (uint8_t *)user_buf;
            uint8_t mac[6];
            e1000_get_mac(mac);
            for (int i = 0; i < 6; i++) out[i] = mac[i];
            out[6] = e1000_link_up() ? 1 : 0;

            frame->rax = 0;
            break;
        }

        // Phase 9b: Network stack status
        case SYS_NET_STATUS: {
            void *user_buf = (void *)frame->rdi;
            if (!user_buf || !is_user_pointer(user_buf, sizeof(net_status_t))) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }
            net_status_t status;
            net_get_status(&status);
            // Copy to user buffer
            uint8_t *dst = (uint8_t *)user_buf;
            uint8_t *src = (uint8_t *)&status;
            for (size_t i = 0; i < sizeof(net_status_t); i++) {
                dst[i] = src[i];
            }
            frame->rax = 0;
            break;
        }

        case SYS_HTTP_GET: {
            const char *url_user = (const char *)frame->rdi;
            char *resp_buf = (char *)frame->rsi;
            int max_len = (int)frame->rdx;

            task_t *current = sched_get_current_task();
            if (!current) { frame->rax = (uint64_t)(long)-1; break; }

            if (!resp_buf || max_len <= 0 || !is_user_pointer(resp_buf, max_len)) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            // Try to collect a completed result first
            int http_result = net_http_get_check(current->id, resp_buf, max_len);
            if (http_result != -99) {
                // Got a final result (success or error)
                frame->rax = (uint64_t)(long)http_result;
                break;
            }

            // No result yet — either start a new request or one is already in flight
            char url_kernel[512];
            if (copy_string_from_user(url_user, url_kernel, sizeof(url_kernel)) <= 0) {
                frame->rax = (uint64_t)(long)NET_ERR_BAD_URL;
                break;
            }

            int start_ret = net_http_get_start(current->id, url_kernel);
            if (start_ret == 0 || start_ret == NET_ERR_BUSY) {
                // Request in flight — block and retry
                current->state = TASK_STATE_BLOCKED;
                frame->rax = (uint64_t)(long)-99;
            } else {
                // Couldn't start (no network, OOM, etc.)
                frame->rax = (uint64_t)(long)start_ret;
            }
            break;
        }

        case SYS_DNS_RESOLVE: {
            const char *hostname_user = (const char *)frame->rdi;
            uint8_t *ip_buf = (uint8_t *)frame->rsi;

            task_t *current = sched_get_current_task();
            if (!current) { frame->rax = (uint64_t)(long)-1; break; }

            if (!ip_buf || !is_user_pointer(ip_buf, 4)) {
                frame->rax = (uint64_t)(long)-1;
                break;
            }

            // Try to collect a completed result first
            int dns_result = net_dns_check(current->id, ip_buf);
            if (dns_result != -99) {
                frame->rax = (uint64_t)(long)dns_result;
                break;
            }

            // No result yet — start or continue
            char hostname_kernel[256];
            if (copy_string_from_user(hostname_user, hostname_kernel, sizeof(hostname_kernel)) <= 0) {
                frame->rax = (uint64_t)(long)NET_ERR_BAD_URL;
                break;
            }

            int dns_start_ret = net_dns_start(current->id, hostname_kernel);
            if (dns_start_ret == 0 || dns_start_ret == NET_ERR_BUSY) {
                current->state = TASK_STATE_BLOCKED;
                frame->rax = (uint64_t)(long)-99;
            } else {
                frame->rax = (uint64_t)(long)dns_start_ret;
            }
            break;
        }

        default:
            frame->rax = (uint64_t)-1;
            break;
    }
}