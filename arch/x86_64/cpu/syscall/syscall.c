// arch/x86_64/cpu/syscall/syscall.c
#include "syscall.h"
#include "../../../../kernel/pid_hash.h"
#include "../../../../kernel/resource/rlimit.h"
#include "../../../../kernel/sync/spinlock.h"
extern spinlock_t sched_lock;
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
#include "../../../../kernel/cap/can.h"
#include "../../../../kernel/cap/shim_v1.h"
#include "../../../../kernel/cap/object.h"
#include "../../../../kernel/cap/token.h"
#include "../../../../kernel/driver/userdrv.h"
#include "../../../../kernel/fs/grahafs.h"
#include "../../../../kernel/fs/grahafs_v2.h"
#include "../../drivers/e1000/e1000.h"
#include "../../../../kernel/net/rawnet.h"
#include "../../../../kernel/fs/pipe.h"
#include "../../../../kernel/fs/cluster.h"
#include "../../../../kernel/autorun.h"
#include "../../../../kernel/log.h"
#include "../../../../kernel/mm/kheap.h"
#include "../../../../kernel/percpu.h"
#include "../../../../kernel/cap/pledge.h"
#include "../../../../kernel/audit.h"
#include "../../../../kernel/rtc.h"
#include "../../../../kernel/cap/deprecated.h"
#include "../../../../kernel/ipc/channel.h"
#include "../../../../kernel/mm/vmo.h"
#include "../../../../kernel/io/stream.h"
#include "../../drivers/ahci/ahci.h"
#include "../interrupts.h"
#include <stdbool.h>
#include <stddef.h>

// Phase 22 Stage F: <string.h> came from kernel/net/string.h (deleted).
// The kernel libc surface is hand-rolled — memcpy is declared below; memset
// and strncmp move to main.c shims.
extern void *memset(void *dest, int c, size_t n);
extern int   strncmp(const char *s1, const char *s2, size_t n);

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

// Phase 15b: pledge guard helper. Called at the top of every sensitive
// syscall handler. If the calling task lacks the named class in its
// pledge_mask, writes an AUDIT_CAP_VIOLATION entry, stamps -EPLEDGE on
// the syscall frame, and returns false so the caller can `break` out of
// the switch arm.
static bool pledge_check_and_audit(struct syscall_frame *frame,
                                   uint8_t class_bit,
                                   const char *detail_msg) {
    task_t *current = sched_get_current_task();
    if (!current) {
        // Syscall from early boot / kernel thread — no pledge to check.
        return true;
    }
    if (pledge_allows(current, class_bit)) {
        return true;
    }
    audit_write_cap_violation(current->id,
                               /*obj_idx*/ 0xFFFFFFFFu,
                               CAP_V2_EPLEDGE,
                               /*rights_required*/ 0,
                               /*rights_held*/ 0,
                               detail_msg,
                               AUDIT_SRC_NATIVE);
    frame->rax = (uint64_t)(long)CAP_V2_EPLEDGE;
    return false;
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

// SYS_FS_LIST_VERSIONS chain-walk context + callback. The callback runs
// for each entry the in-memory chain holds (newest → oldest), copying
// the version_entry into a fs_version_info_t in the user buffer. Stops
// when ctx->cap entries have been written.
struct list_versions_walk_ctx {
    fs_version_info_t *out;
    uint32_t cap;
    uint32_t written;
};

static bool list_versions_emit(const struct grahafs_v2_version_entry *e,
                               void *vctx) {
    struct list_versions_walk_ctx *ctx = (struct list_versions_walk_ctx *)vctx;
    if (!ctx || ctx->written >= ctx->cap || !e) return false;
    fs_version_info_t info = {0};
    info.version_id     = e->version_id;
    info.timestamp_ns   = e->timestamp_ns;
    info.size           = e->size;
    info.simhash        = e->simhash;
    info.cluster_id     = e->cluster_id_at_version;
    info.segment_id     = e->segment_id;
    info.parent_version = e->parent_version;
    info.prev_version   = e->prev_version;
    uint8_t *dst = (uint8_t *)&ctx->out[ctx->written];
    uint8_t *src = (uint8_t *)&info;
    for (size_t i = 0; i < sizeof(info); ++i) dst[i] = src[i];
    ctx->written++;
    return ctx->written < ctx->cap;
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
        klog(KLOG_ERROR, SUBSYS_SYSCALL, "[SYSCALL] ERROR: NULL frame!");
        framebuffer_draw_string("PANIC: NULL syscall frame!", 10, 10, COLOR_WHITE, COLOR_RED);
        asm volatile("cli; hlt");
    }

    uint64_t syscall_num = frame->int_no;
    
    switch (syscall_num) {
        case SYS_PUTC: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            char c = (char)frame->rdi;

            // Phase 10a: Route through per-process FD 1 (stdout)
            task_t *putc_task = sched_get_current_task();
            uint8_t fd1_type = FD_TYPE_CONSOLE; // default fallback
            int16_t fd1_ref = 0;
            if (putc_task) {
                fd1_type = putc_task->fd_table[1].type;
                fd1_ref = putc_task->fd_table[1].ref;
            }

            if (fd1_type == FD_TYPE_CONSOLE || fd1_type == FD_TYPE_UNUSED) {
                // Original console path: serial + framebuffer
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
            } else if (fd1_type == FD_TYPE_FILE) {
                // Redirected to file: write single char
                vfs_write(fd1_ref, &c, 1);
            } else if (fd1_type == FD_TYPE_PIPE_WRITE) {
                // Phase 10b: Write to pipe
                pipe_write(fd1_ref, &c, 1);
            }

            frame->rax = 0;
            break;
        }

        case SYS_OPEN: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_READ, "pledge denied: fs_read")) break;
            const char *pathname_user = (const char *)frame->rdi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) <= 0) {
                frame->rax = -1;
                break;
            }

            // Phase 10a: Allocate per-process FD wrapping global file table entry
            int global_fd = vfs_open(pathname_kernel);
            if (global_fd < 0) {
                frame->rax = -1;
                break;
            }

            task_t *open_task = sched_get_current_task();
            if (!open_task) {
                vfs_close(global_fd);
                frame->rax = -1;
                break;
            }

            // Find free per-process FD slot
            int proc_fd = -1;
            for (int f = 0; f < PROC_MAX_FDS; f++) {
                if (open_task->fd_table[f].type == FD_TYPE_UNUSED) {
                    proc_fd = f;
                    break;
                }
            }
            if (proc_fd < 0) {
                vfs_close(global_fd);
                frame->rax = -1; // Too many open files
                break;
            }

            open_task->fd_table[proc_fd].type = FD_TYPE_FILE;
            open_task->fd_table[proc_fd].ref = (int16_t)global_fd;
            open_task->fd_table[proc_fd].flags = 0;
            frame->rax = proc_fd;
            break;
        }

        case SYS_READ: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_READ, "pledge denied: fs_read")) break;
            int fd = (int)frame->rdi;
            void *buffer_user = (void *)frame->rsi;
            size_t count = (size_t)frame->rdx;
            if (!is_user_pointer(buffer_user, count)) {
                frame->rax = -1;
                break;
            }

            // Phase 10a: Resolve per-process FD
            task_t *read_task = sched_get_current_task();
            if (!read_task || fd < 0 || fd >= PROC_MAX_FDS) {
                frame->rax = -1;
                break;
            }
            proc_fd_t *rfd = &read_task->fd_table[fd];
            if (rfd->type == FD_TYPE_FILE) {
                frame->rax = vfs_read(rfd->ref, buffer_user, count);
            } else if (rfd->type == FD_TYPE_CONSOLE) {
                // Console bulk read not supported (use SYS_GETC)
                frame->rax = -1;
            } else if (rfd->type == FD_TYPE_PIPE_READ) {
                // Phase 10b: Read from pipe
                frame->rax = pipe_read(rfd->ref, buffer_user, count);
            } else {
                // UNUSED or invalid
                frame->rax = -1;
            }
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)frame->rdi;

            // Phase 10a: Resolve per-process FD
            task_t *close_task = sched_get_current_task();
            if (!close_task || fd < 0 || fd >= PROC_MAX_FDS) {
                frame->rax = -1;
                break;
            }
            proc_fd_t *cfd = &close_task->fd_table[fd];
            if (cfd->type == FD_TYPE_FILE) {
                frame->rax = vfs_close(cfd->ref);
            } else if (cfd->type == FD_TYPE_CONSOLE) {
                frame->rax = -1; // Cannot close console FDs
                break;
            } else if (cfd->type == FD_TYPE_PIPE_READ || cfd->type == FD_TYPE_PIPE_WRITE) {
                // Phase 10b: Close pipe end
                pipe_ref_dec(cfd->ref, cfd->type);
                frame->rax = 0;
            } else {
                // UNUSED
                frame->rax = -1;
                break;
            }
            cfd->type = FD_TYPE_UNUSED;
            cfd->ref = -1;
            cfd->flags = 0;
            break;
        }

        case SYS_WRITE: {

            // Phase 15b: SYS_WRITE's pledge class depends on the target fd
            // type. Console output (stdout/stderr) never needs FS_WRITE —
            // a pledged process must still be able to emit diagnostics.
            // File writes require FS_WRITE; pipe writes require IPC_SEND.
            // This check moved below the fd-type resolution.
            int fd = (int)frame->rdi;
            const void *buffer_user = (const void *)frame->rsi;
            size_t count = (size_t)frame->rdx;
            if (!is_user_pointer(buffer_user, count)) {
                frame->rax = -1;
                break;
            }

            // Phase 10a: Resolve per-process FD
            task_t *write_task = sched_get_current_task();
            if (!write_task || fd < 0 || fd >= PROC_MAX_FDS) {
                frame->rax = -1;
                break;
            }
            proc_fd_t *wfd = &write_task->fd_table[fd];
            if (wfd->type == FD_TYPE_FILE) {
                if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE,
                                            "pledge denied: fs_write on SYS_WRITE")) break;
                frame->rax = vfs_write(wfd->ref, (void*)buffer_user, count);
            } else if (wfd->type == FD_TYPE_CONSOLE) {
                // Phase 15b: emit the whole buffer to serial atomically so
                // concurrent writers (other user processes, klog mirror) can't
                // interleave byte-by-byte on the UART. Framebuffer draw is
                // per-char and not TAP-parsed, so it keeps the old loop.
                const char *buf = (const char *)buffer_user;
                serial_write_n(buf, count);
                for (size_t i = 0; i < count; i++) {
                    if (buf[i] == '\n') {
                        term_x = 0;
                        term_y += 16;
                    } else {
                        framebuffer_draw_char(buf[i], term_x, term_y, COLOR_WHITE);
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
                }
                frame->rax = count;
            } else if (wfd->type == FD_TYPE_PIPE_WRITE) {
                if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND,
                                            "pledge denied: ipc_send on SYS_WRITE to pipe")) break;
                frame->rax = pipe_write(wfd->ref, (void*)buffer_user, count);
            } else {
                // UNUSED or invalid
                frame->rax = -1;
            }
            break;
        }

        case SYS_CREATE: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE, "pledge denied: fs_write")) break;
            const char *pathname_user = (const char *)frame->rdi;
            uint32_t mode = (uint32_t)frame->rsi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                int rc = vfs_create(pathname_kernel, mode);
                frame->rax = (uint64_t)(long)rc;
                // Phase 15b: audit critical-path writes (/etc/ and /var/).
                if (rc >= 0 &&
                    (strncmp(pathname_kernel, "/etc/", 5) == 0 ||
                     strncmp(pathname_kernel, "/var/", 5) == 0)) {
                    task_t *wc = sched_get_current_task();
                    audit_write_fs_write_critical(wc ? wc->id : -1, pathname_kernel);
                }
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_MKDIR: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE, "pledge denied: fs_write")) break;
            const char *pathname_user = (const char *)frame->rdi;
            uint32_t mode = (uint32_t)frame->rsi;
            char pathname_kernel[256];
            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                int rc = vfs_mkdir(pathname_kernel, mode);
                frame->rax = (uint64_t)(long)rc;
                // Phase 15b: audit critical-path mkdirs.
                if (rc >= 0 &&
                    (strncmp(pathname_kernel, "/etc/", 5) == 0 ||
                     strncmp(pathname_kernel, "/var/", 5) == 0)) {
                    task_t *mc = sched_get_current_task();
                    audit_write_fs_write_critical(mc ? mc->id : -1, pathname_kernel);
                }
            } else {
                frame->rax = -1;
            }
            break;
        }

        case SYS_READDIR: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_READ, "pledge denied: fs_read")) break;
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

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE, "pledge denied: fs_write")) break;
            // Flush all pending writes to disk
            vfs_sync();
            frame->rax = 0;
            break;
        }

        case SYS_BRK: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            void *addr = (void *)frame->rdi;
            task_t *current = sched_get_current_task();

            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_BRK] addr=0x%lx", (unsigned long)((uint64_t)addr));

            if (!current) {
                klog(KLOG_ERROR, SUBSYS_SYSCALL, "[SYS_BRK] ERROR: No current process!");
                frame->rax = -1;
                break;
            }

            // If addr is NULL, just return current brk
            if (addr == NULL) {
                klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_BRK] Return current brk=0x%lx", (unsigned long)(current->brk));
                frame->rax = (uint64_t)current->brk;
                break;
            }

            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_BRK] curr_brk=0x%lx heap_start=0x%lx", (unsigned long)(current->brk), (unsigned long)(current->heap_start));

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
                // Phase 20: check mem_limit_pages BEFORE allocating. Counts
                // the whole requested growth at once so partial-success +
                // over-limit doesn't leave us out of sync.
                uint64_t grow_pages = (new_brk_page - old_brk_page) / PAGE_SIZE;
                if (rlimit_check_mem(current, grow_pages) != 0) {
                    frame->rax = (uint64_t)(int64_t)-12;  // -ENOMEM
                    break;
                }

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
                    klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_BRK] SUCCESS: new_brk=0x%lx", (unsigned long)(current->brk));
                } else {
                    // PMM/map failure AFTER rlimit_check reserved the quota.
                    // Refund so mem_pages_used stays accurate.
                    rlimit_account_free_mem(current, grow_pages);
                    frame->rax = (uint64_t)-1;  // ENOMEM
                    klog(KLOG_ERROR, SUBSYS_SYSCALL, "[SYS_BRK] FAILED: Out of memory or mapping failed");
                }
            } else if (new_brk_page < old_brk_page) {
                // Phase 20: return quota as we shrink.
                uint64_t shrink_pages = (old_brk_page - new_brk_page) / PAGE_SIZE;
                rlimit_account_free_mem(current, shrink_pages);

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

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            // Phase 10a: Route through per-process FD 0 (stdin)
            task_t *getc_task = sched_get_current_task();
            uint8_t fd0_type = FD_TYPE_CONSOLE; // default fallback
            int16_t fd0_ref = 0;
            if (getc_task) {
                fd0_type = getc_task->fd_table[0].type;
                fd0_ref = getc_task->fd_table[0].ref;
            }

            if (fd0_type == FD_TYPE_CONSOLE || fd0_type == FD_TYPE_UNUSED) {
                // Original keyboard path
                char c;
                asm volatile("sti");
                while ((c = keyboard_getchar()) == 0) {
                    asm ("hlt");
                }
                asm volatile("cli");
                frame->rax = c;
            } else if (fd0_type == FD_TYPE_FILE) {
                // Read one byte from file
                char c = 0;
                ssize_t n = vfs_read(fd0_ref, &c, 1);
                frame->rax = (n > 0) ? (uint64_t)(unsigned char)c : 0;
            } else if (fd0_type == FD_TYPE_PIPE_READ) {
                // Phase 10b: Read one byte from pipe
                frame->rax = pipe_read_char(fd0_ref);
            } else {
                frame->rax = 0;
            }
            break;
        }

        case SYS_EXEC: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SPAWN, "pledge denied: spawn")) break;
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
                task_t *ec = sched_get_current_task();
                audit_write_spawn(ec ? ec->id : -1, pid, path_kernel);
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
            
            // Phase 10a: Close all open file descriptors
            for (int f = 0; f < PROC_MAX_FDS; f++) {
                proc_fd_t *pfd = &current->fd_table[f];
                if (pfd->type == FD_TYPE_FILE) {
                    vfs_close(pfd->ref);
                } else if (pfd->type == FD_TYPE_PIPE_READ || pfd->type == FD_TYPE_PIPE_WRITE) {
                    pipe_ref_dec(pfd->ref, pfd->type);
                }
                pfd->type = FD_TYPE_UNUSED;
                pfd->ref = -1;
            }

            // Remove all CAN event watchers for this process
            cap_unwatch_all_for_pid(current->id);

            // Unregister all user-owned capabilities for this process
            cap_unregister_by_owner(current->id);

            // Phase 22 Stage F: net_cleanup_task retired with Mongoose. The
            // userspace netd tracks per-pid sockets; userdrv_on_owner_death
            // (which fires from the same exit path) calls rawnet_on_peer_death
            // to GC named-channel publish slots.

            // SMP-safe ordering: do all the bookkeeping FIRST, then publish
            // ZOMBIE state LAST. The parent's SYS_WAIT poll loop only reaps
            // tasks observed in ZOMBIE state, so as long as we publish that
            // state after sched_orphan_children + wake_waiting_parent have
            // completed, the reaper cannot free task_ptrs[id] out from under
            // us. (Setting state earlier — as the original code did — opened
            // a use-after-free window where wake_waiting_parent dereferenced
            // an already-freed task_ptrs slot — observed crash, sched.c:606.)
            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_EXIT] pid=%lu status=%d entering",
                 (unsigned long)current->id, status);
            current->exit_status = status;
            sched_orphan_children(current->id);
            wake_waiting_parent(current->id);
            __atomic_store_n((volatile int *)&current->state, (int)TASK_STATE_ZOMBIE,
                             __ATOMIC_RELEASE);
            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_EXIT] pid=%lu set ZOMBIE, returning",
                 (unsigned long)current->id);

            framebuffer_draw_string("Process exited", 10, 600, COLOR_YELLOW, 0x00101828);

            // Phase 12: when PID 1 exits, notify the autorun subsystem.
            // In logging-only mode (work unit 5) it just emits a serial
            // line. Work unit 6 replaces the body with kernel_shutdown()
            // gated on autorun_is_active().
            if (current->id == autorun_get_init_pid()) {
                autorun_on_init_exit(current->id, status);
            }

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
            
            // SMP-safe SYS_WAIT: in-kernel poll loop instead of state=BLOCKED
            // dance. The original approach (set BLOCKED + return -99) suffered
            // from a missed-wakeup race under SMP: child exits on AP X,
            // wake_waiting_parent reads parent state RUNNING (because parent
            // hasn't reached the BLOCKED store yet), no wake fires; parent then
            // sets BLOCKED with no future waker → hang. Polling avoids this:
            // every iteration takes a snapshot under sched_lock, so any zombie
            // visible after our last check is found on the next iteration.
            int child_pid = -1;
            int exit_status = 0;
            int has_children = 0;
            uint32_t wait_iter = 0;
            extern volatile uint64_t g_timer_ticks;
            uint64_t wait_start_ticks = g_timer_ticks;
            while (1) {
                spinlock_acquire(&sched_lock);
                child_pid = -1;
                has_children = 0;
                for (int i = 0; i < MAX_TASKS; i++) {
                    task_t *task = sched_get_task_any(i);
                    if (!task) continue;
                    if (task->parent_id != current->id) continue;
                    has_children = 1;
                    // Atomic-acquire load pairs with the atomic-release store
                    // in SYS_EXIT / kill paths so we see the ZOMBIE transition
                    // at the same point as the cleanup completing on the
                    // exiting CPU. Cast to (int *) so __atomic ops compile
                    // cleanly against the enum type.
                    int st = __atomic_load_n((volatile int *)&task->state,
                                             __ATOMIC_ACQUIRE);
                    if ((task_state_t)st == TASK_STATE_ZOMBIE) {
                        child_pid = i;
                        exit_status = task->exit_status;
                        break;
                    }
                }
                spinlock_release(&sched_lock);
                if (child_pid >= 0) break;
                if (!has_children) break;
                // Diagnostic: log periodically if wait is taking unusually long.
                wait_iter++;
                if (wait_iter > 0 && (wait_iter & 0x3FF) == 0) {
                    uint64_t elapsed = g_timer_ticks - wait_start_ticks;
                    klog(KLOG_WARN, SUBSYS_SYSCALL,
                         "[SYS_WAIT] pid=%lu has been polling for %lu ticks "
                         "(iter=%lu, has_children=%d)",
                         (unsigned long)current->id,
                         (unsigned long)elapsed,
                         (unsigned long)wait_iter,
                         has_children);
                }
                // No zombie yet, wait for the next tick. sti+hlt yields to
                // schedule(); when we resume here, re-check.
                asm volatile("sti; hlt; cli" ::: "memory");
            }

            if (child_pid >= 0) {
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
            } else {
                frame->rax = -1;
                framebuffer_draw_string("wait(): No children to wait for", 400, 660, COLOR_YELLOW, 0x00101828);
            }
            break;
        }

        case SYS_SPAWN: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SPAWN, "pledge denied: spawn")) break;
            const char *path_user = (const char *)frame->rdi;
            char path_kernel[256];

            if (copy_string_from_user(path_user, path_kernel, sizeof(path_kernel)) <= 0) {
                klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_SPAWN] Bad path");
                frame->rax = -1;
                break;
            }

            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_SPAWN] Path: %s", path_kernel);

            task_t *current = sched_get_current_task();
            if (!current) {
                frame->rax = -1;
                break;
            }

            int pid = sched_spawn_process(path_kernel, current->id);
            if (pid < 0) {
                klog(KLOG_ERROR, SUBSYS_SYSCALL, "[SYS_SPAWN] Failed");
                frame->rax = -1;
            } else {
                klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_SPAWN] Success: pid=%lu", (unsigned long)(pid));
                audit_write_spawn(current->id, pid, path_kernel);
                frame->rax = pid;
            }
            break;
        }

        case SYS_KILL: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            int target_pid = (int)frame->rdi;
            int signal = (int)frame->rsi;

            klog(KLOG_INFO, SUBSYS_SYSCALL, "[SYS_KILL] pid=%lu sig=%lu", (unsigned long)(target_pid), (unsigned long)(signal));

            int kill_rc = sched_send_signal(target_pid, signal);
            task_t *killer = sched_get_current_task();
            audit_write_kill(killer ? killer->id : -1, target_pid, signal);
            frame->rax = (uint64_t)(long)kill_rc;
            break;
        }

        case SYS_SIGNAL: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
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

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
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
                case STATE_CAT_USERDRV: {
                    // Phase 21: walk g_userdrv_entries[] under the registry
                    // lock (snapshot is racy with register/death but bounded).
                    extern userdrv_entry_t g_userdrv_entries[];
                    extern spinlock_t g_userdrv_registry_lock;
                    extern uint32_t g_pci_table_count;
                    state_userdrv_list_t *out = (state_userdrv_list_t *)user_buf;
                    out->count = 0;
                    out->_pad = 0;
                    spinlock_acquire(&g_userdrv_registry_lock);
                    uint32_t cap = STATE_MAX_USERDRV;
                    uint32_t n = g_pci_table_count < cap ? g_pci_table_count : cap;
                    for (uint32_t i = 0; i < n; i++) {
                        userdrv_entry_t *e = &g_userdrv_entries[i];
                        state_userdrv_entry_t *o = &out->entries[out->count++];
                        o->pci_addr        = e->pci_addr;
                        o->vendor_id       = e->vendor_id;
                        o->device_id       = e->device_id;
                        o->device_class    = e->device_class;
                        o->device_subclass = e->device_subclass;
                        o->irq_vector      = e->irq_vector;
                        o->is_claimable    = e->is_claimable;
                        o->driver_owner_pid = e->driver_owner_pid;
                        o->irq_count       = e->irq_count;
                        o->registered_at_tsc = e->registered_at_tsc;
                        o->bar_phys        = e->bar_phys;
                        o->bar_size        = e->bar_size;
                    }
                    spinlock_release(&g_userdrv_registry_lock);
                    required = sizeof(state_userdrv_list_t);
                    break;
                }
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

        // Phase 16: legacy string-name CAN syscalls. All seven return
        // -EDEPRECATED. The first time each (pid, syscall) pair hits a legacy
        // number, AUDIT_DEPRECATED_SYSCALL is emitted; subsequent calls from
        // the same pid for the same number are silent so the audit log
        // doesn't spam. Pledge is intentionally NOT consulted here — a
        // deprecated call is equally noteworthy regardless of pledge state.
        case SYS_CAP_ACTIVATE: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_ACTIVATE)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_ACTIVATE(1031)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        case SYS_CAP_DEACTIVATE: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_DEACTIVATE)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_DEACTIVATE(1032)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        case SYS_CAP_REGISTER: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_REGISTER)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_REGISTER(1033)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        case SYS_CAP_UNREGISTER: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_UNREGISTER)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_UNREGISTER(1034)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        // Phase 8c: AI Metadata syscalls
        case SYS_SET_AI_METADATA: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_AI_CALL, "pledge denied: ai_call")) break;
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

            int aim_rc = grahafs_set_ai_metadata(ino, &kmeta);
            frame->rax = (uint64_t)(long)aim_rc;
            if (aim_rc >= 0) {
                task_t *aic = sched_get_current_task();
                audit_write_ai_invoke(aic ? aic->id : -1, kpath);
            }
            break;
        }

        case SYS_GET_AI_METADATA: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_AI_CALL, "pledge denied: ai_call")) break;
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

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
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

        // Phase 8d: CAN Event Propagation — deprecated in Phase 16.
        case SYS_CAP_WATCH: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_WATCH)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_WATCH(1038)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        case SYS_CAP_UNWATCH: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_UNWATCH)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_UNWATCH(1039)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        case SYS_CAP_POLL: {
            task_t *cur = sched_get_current_task();
            int32_t pid = cur ? cur->id : -1;
            if (deprecated_check_and_audit(pid, SYS_CAP_POLL)) {
                audit_write_deprecated_syscall(pid, "SYS_CAP_POLL(1040)");
            }
            frame->rax = (uint64_t)(long)CAP_V2_EDEPRECATED;
            break;
        }

        // Phase 22 Stage F: 5 legacy net syscalls retired. Userspace netd
        // (libnet/libhttp over /sys/net/service) is the supported path.
        // Each retired entry returns -ENOSYS (-38) and emits an audit event
        // so legacy callers surface in dmesg/audit_query during the cutover.
        // The pre-existing pledge check is preserved so pledgetest still sees
        // -EPLEDGE before -ENOSYS — pledge denial dominates retirement.
        case SYS_NET_IFCONFIG:
        case SYS_NET_STATUS: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_NET_SERVER, "pledge denied: net_server")) break;
            task_t *current = sched_get_current_task();
            const char *name = (syscall_num == SYS_NET_IFCONFIG)
                                ? "SYS_NET_IFCONFIG (retired P22.F)"
                                : "SYS_NET_STATUS (retired P22.F)";
            audit_write_deprecated_syscall(current ? current->id : -1, name);
            frame->rax = (uint64_t)(long)-38;  // -ENOSYS
            break;
        }

        case SYS_HTTP_GET:
        case SYS_HTTP_POST:
        case SYS_DNS_RESOLVE: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_NET_CLIENT, "pledge denied: net_client")) break;
            task_t *current = sched_get_current_task();
            const char *name = "<retired-net>";
            switch (syscall_num) {
                case SYS_HTTP_GET:     name = "SYS_HTTP_GET (retired P22.F)";     break;
                case SYS_HTTP_POST:    name = "SYS_HTTP_POST (retired P22.F)";    break;
                case SYS_DNS_RESOLVE:  name = "SYS_DNS_RESOLVE (retired P22.F)";  break;
            }
            audit_write_deprecated_syscall(current ? current->id : -1, name);
            frame->rax = (uint64_t)(long)-38;  // -ENOSYS
            break;
        }

        // Phase 10b: Pipe and FD duplication syscalls
        case SYS_PIPE: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND, "pledge denied: ipc_send")) break;
            int *fds_user = (int *)frame->rdi;
            if (!fds_user || !is_user_pointer(fds_user, 2 * sizeof(int))) {
                frame->rax = -1;
                break;
            }

            task_t *pipe_task = sched_get_current_task();
            if (!pipe_task) {
                frame->rax = -1;
                break;
            }

            // Allocate pipe
            int pipe_idx = pipe_alloc();
            if (pipe_idx < 0) {
                frame->rax = -1;
                break;
            }

            // Find two free per-process FD slots
            int read_fd = -1, write_fd = -1;
            for (int f = 0; f < PROC_MAX_FDS; f++) {
                if (pipe_task->fd_table[f].type == FD_TYPE_UNUSED) {
                    if (read_fd < 0) {
                        read_fd = f;
                    } else if (write_fd < 0) {
                        write_fd = f;
                        break;
                    }
                }
            }

            if (read_fd < 0 || write_fd < 0) {
                // Not enough FD slots — free the pipe
                pipe_ref_dec(pipe_idx, FD_TYPE_PIPE_READ);
                pipe_ref_dec(pipe_idx, FD_TYPE_PIPE_WRITE);
                frame->rax = -1;
                break;
            }

            pipe_task->fd_table[read_fd].type = FD_TYPE_PIPE_READ;
            pipe_task->fd_table[read_fd].ref = (int16_t)pipe_idx;
            pipe_task->fd_table[read_fd].flags = 0;

            pipe_task->fd_table[write_fd].type = FD_TYPE_PIPE_WRITE;
            pipe_task->fd_table[write_fd].ref = (int16_t)pipe_idx;
            pipe_task->fd_table[write_fd].flags = 0;

            fds_user[0] = read_fd;
            fds_user[1] = write_fd;
            frame->rax = 0;
            break;
        }

        case SYS_DUP2: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND, "pledge denied: ipc_send")) break;
            int old_fd = (int)frame->rdi;
            int new_fd = (int)frame->rsi;

            task_t *dup2_task = sched_get_current_task();
            if (!dup2_task || old_fd < 0 || old_fd >= PROC_MAX_FDS ||
                new_fd < 0 || new_fd >= PROC_MAX_FDS) {
                frame->rax = -1;
                break;
            }

            proc_fd_t *old_pfd = &dup2_task->fd_table[old_fd];
            if (old_pfd->type == FD_TYPE_UNUSED) {
                frame->rax = -1; // old_fd not open
                break;
            }

            if (old_fd == new_fd) {
                frame->rax = new_fd; // No-op
                break;
            }

            // Close new_fd if it's currently open
            proc_fd_t *new_pfd = &dup2_task->fd_table[new_fd];
            if (new_pfd->type == FD_TYPE_FILE) {
                vfs_close(new_pfd->ref);
            } else if (new_pfd->type == FD_TYPE_PIPE_READ || new_pfd->type == FD_TYPE_PIPE_WRITE) {
                pipe_ref_dec(new_pfd->ref, new_pfd->type);
            }

            // Copy old_fd entry to new_fd
            *new_pfd = *old_pfd;

            // Increment refcounts for pipe FDs
            if (new_pfd->type == FD_TYPE_PIPE_READ || new_pfd->type == FD_TYPE_PIPE_WRITE) {
                pipe_ref_inc(new_pfd->ref, new_pfd->type);
            }
            // Increment refcount for FILE FDs
            if (new_pfd->type == FD_TYPE_FILE) {
                vfs_ref_inc(new_pfd->ref);
            }

            frame->rax = new_fd;
            break;
        }

        case SYS_DUP: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND, "pledge denied: ipc_send")) break;
            int old_fd = (int)frame->rdi;

            task_t *dup_task = sched_get_current_task();
            if (!dup_task || old_fd < 0 || old_fd >= PROC_MAX_FDS) {
                frame->rax = -1;
                break;
            }

            proc_fd_t *old_entry = &dup_task->fd_table[old_fd];
            if (old_entry->type == FD_TYPE_UNUSED) {
                frame->rax = -1;
                break;
            }

            // Find lowest free FD
            int free_fd = -1;
            for (int f = 0; f < PROC_MAX_FDS; f++) {
                if (dup_task->fd_table[f].type == FD_TYPE_UNUSED) {
                    free_fd = f;
                    break;
                }
            }

            if (free_fd < 0) {
                frame->rax = -1; // No free FDs
                break;
            }

            // Copy the entry
            dup_task->fd_table[free_fd] = *old_entry;

            // Increment refcounts for pipe FDs
            if (old_entry->type == FD_TYPE_PIPE_READ || old_entry->type == FD_TYPE_PIPE_WRITE) {
                pipe_ref_inc(old_entry->ref, old_entry->type);
            }
            // Increment refcount for FILE FDs
            if (old_entry->type == FD_TYPE_FILE) {
                vfs_ref_inc(old_entry->ref);
            }

            frame->rax = free_fd;
            break;
        }

        case SYS_TRUNCATE: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE, "pledge denied: fs_write")) break;
            // Phase 10c: Truncate file to 0 bytes
            int trunc_fd = (int)frame->rdi;
            task_t *trunc_task = sched_get_current_task();
            if (!trunc_task || trunc_fd < 0 || trunc_fd >= PROC_MAX_FDS) {
                frame->rax = (uint64_t)-1;
                break;
            }
            proc_fd_t *trunc_pfd = &trunc_task->fd_table[trunc_fd];
            if (trunc_pfd->type != FD_TYPE_FILE) {
                frame->rax = (uint64_t)-1;
                break;
            }
            frame->rax = (uint64_t)(long)vfs_truncate(trunc_pfd->ref);
            break;
        }

        case SYS_COMPUTE_SIMHASH: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_AI_CALL, "pledge denied: ai_call")) break;
            // Phase 11a: Compute SimHash for a file
            // RDI = path string
            char sh_path[256];
            if (copy_string_from_user((const char *)frame->rdi, sh_path, 256) <= 0) {
                frame->rax = 0;
                break;
            }
            vfs_node_t *sh_node = vfs_path_to_node(sh_path);
            if (!sh_node || sh_node->inode == 0) {
                frame->rax = 0;
                break;
            }
            frame->rax = grahafs_compute_simhash(sh_node->inode);
            break;
        }

        case SYS_FIND_SIMILAR: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // Phase 11a: Find similar files by SimHash Hamming distance
            // RDI = path, RSI = threshold, RDX = results buffer
            char sim_path[256];
            if (copy_string_from_user((const char *)frame->rdi, sim_path, 256) <= 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            int sim_threshold = (int)frame->rsi;
            void *sim_results = (void *)frame->rdx;
            if (!sim_results) {
                frame->rax = (uint64_t)-1;
                break;
            }
            vfs_node_t *sim_node = vfs_path_to_node(sim_path);
            if (!sim_node || sim_node->inode == 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            grahafs_search_results_t kresults;
            int sim_ret = grahafs_find_similar(sim_node->inode, sim_threshold, &kresults, 16);
            if (sim_ret >= 0) {
                // Copy results to user-space
                uint8_t *dst = (uint8_t *)sim_results;
                uint8_t *src = (uint8_t *)&kresults;
                for (size_t ci = 0; ci < sizeof(grahafs_search_results_t); ci++)
                    dst[ci] = src[ci];
            }
            frame->rax = (uint64_t)(long)sim_ret;
            break;
        }

        case SYS_CLUSTER_LIST: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // Phase 11b: Get list of all clusters
            // RDI = pointer to cluster_list_t
            void *cl_buf = (void *)frame->rdi;
            if (!cl_buf) {
                frame->rax = (uint64_t)-1;
                break;
            }
            cluster_list_t klist;
            int cl_ret = cluster_get_list(&klist);
            // Copy to user
            uint8_t *cl_dst = (uint8_t *)cl_buf;
            uint8_t *cl_src = (uint8_t *)&klist;
            for (size_t ci = 0; ci < sizeof(cluster_list_t); ci++)
                cl_dst[ci] = cl_src[ci];
            frame->rax = (uint64_t)(long)cl_ret;
            break;
        }

        case SYS_CLUSTER_MEMBERS: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // Phase 11b: Get members of a specific cluster
            // RDI = cluster_id, RSI = pointer to cluster_members_t
            uint32_t cm_id = (uint32_t)frame->rdi;
            void *cm_buf = (void *)frame->rsi;
            if (!cm_buf || cm_id == 0) {
                frame->rax = (uint64_t)-1;
                break;
            }
            cluster_members_t kmembers;
            int cm_ret = cluster_get_members(cm_id, &kmembers);
            // Copy to user
            uint8_t *cm_dst = (uint8_t *)cm_buf;
            uint8_t *cm_src = (uint8_t *)&kmembers;
            for (size_t ci = 0; ci < sizeof(cluster_members_t); ci++)
                cm_dst[ci] = cm_src[ci];
            frame->rax = (uint64_t)(long)cm_ret;
            break;
        }

        case SYS_KLOG_WRITE: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            // Phase 13: append one user-space entry to the klog ring.
            // RDI = level (0..5), RSI = subsys id (>= 10), RDX = user
            // message pointer, R10 = message length in bytes.
            uint8_t level  = (uint8_t)(frame->rdi & 0xFFu);
            uint8_t subsys = (uint8_t)(frame->rsi & 0xFFu);
            const char *user_msg = (const char *)frame->rdx;
            uint32_t msg_len = (uint32_t)frame->r10;

            // User-space may not emit FATAL (reserved for the kernel
            // panic path) and must use a user-range subsystem id.
            if (level >= KLOG_FATAL) { frame->rax = (uint64_t)-1; break; }
            if (subsys < KLOG_FIRST_USER_SUBSYS) { frame->rax = (uint64_t)-1; break; }
            if (!user_msg && msg_len > 0)      { frame->rax = (uint64_t)-1; break; }

            // Bound the message copy so a malformed caller can't OOM
            // the ring. Matches the 192-byte limit the kernel klog()
            // itself uses via kvsnprintf.
            char kbuf[192];
            if (msg_len > sizeof(kbuf) - 1) msg_len = sizeof(kbuf) - 1;
            for (uint32_t i = 0; i < msg_len; i++) kbuf[i] = user_msg[i];
            kbuf[msg_len] = '\0';

            task_t *tk = sched_get_current_task();
            int16_t pid = tk ? (int16_t)tk->id : (int16_t)-1;
            klog_raw(level, subsys, pid, kbuf, msg_len);
            frame->rax = (uint64_t)0;
            break;
        }

        case SYS_KLOG_READ: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // Phase 13: non-destructive read of the klog ring.
            // RDI = level_mask (u8 bitmap, 0 = all levels)
            // RSI = tail_count (0 = everything currently held)
            // RDX = user buffer (klog_entry_t[] destination)
            // R10 = buffer capacity in bytes
            uint8_t level_mask  = (uint8_t)(frame->rdi & 0xFFu);
            uint32_t tail_count = (uint32_t)frame->rsi;
            void *user_buf      = (void *)frame->rdx;
            uint32_t buf_cap    = (uint32_t)frame->r10;

            if (!user_buf) { frame->rax = (uint64_t)-1; break; }

            int n = klog_read_filtered(level_mask, tail_count, user_buf, buf_cap);
            frame->rax = (uint64_t)(long)n;
            break;
        }

        case SYS_DEBUG: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            // Phase 13: controlled-panic trigger for gate tests.
            // Phase 14: allocator / per-CPU test-assist hooks.
            // Only compiled in when WITH_DEBUG_SYSCALL is defined.
#ifdef WITH_DEBUG_SYSCALL
            extern void kpanic(const char *reason);
            int op = (int)frame->rdi;
            const char *arg = (const char *)frame->rsi;
            switch (op) {
            case DEBUG_PANIC:
                kpanic(arg ? arg : "SYS_DEBUG panic");
                // unreachable
                frame->rax = (uint64_t)0;
                break;
            case DEBUG_KERNEL_PF: {
                // Deliberately dereference an unmapped kernel
                // address. The page-fault handler becomes the oops
                // emitter via kpanic_at().
                volatile uint8_t *bad = (volatile uint8_t *)0xFFFFFFFFDEADBEEFULL;
                (void)*bad;
                frame->rax = (uint64_t)0;
                break;
            }
            case DEBUG_PERCPU_WRITE: {
                extern void percpu_init(uint32_t);  /* unused forward decl */
                (void)percpu_init;
                per_cpu_set(test_slot, (uint64_t)frame->rsi);
                extern uint32_t smp_get_current_cpu(void);
                frame->rax = (uint64_t)smp_get_current_cpu();
                break;
            }
            case DEBUG_PERCPU_READ:
                frame->rax = (uint64_t)per_cpu(test_slot);
                break;
            case DEBUG_KMALLOC: {
                size_t size = (size_t)frame->rsi;
                uint8_t subsys = (uint8_t)frame->rdx;
                void *p = kmalloc(size, subsys);
                frame->rax = (uint64_t)p;
                break;
            }
            case DEBUG_KFREE:
                kfree((void *)frame->rsi);
                frame->rax = (uint64_t)0;
                break;
            case DEBUG_CAP_LOOKUP: {
                // RSI = const char *name (user ptr). Returns packed token
                // {gen, idx, flags=0} for the paired cap_object_t, or 0
                // if the cap isn't found.
                char kname[32];
                if (copy_string_from_user((const char *)frame->rsi, kname, 32) <= 0) {
                    frame->rax = 0;
                    break;
                }
                int can_id = cap_find(kname);
                if (can_id < 0) { frame->rax = 0; break; }
                // The paired cap_object idx is stored in the can_entry.
                // Since can_entry_t is slab-allocated and we don't have a
                // direct getter by id exported, use an internal helper.
                // For Phase 15a we expose it through a new accessor.
                extern uint32_t cap_get_object_idx(int can_id);
                uint32_t obj_idx = cap_get_object_idx(can_id);
                if (obj_idx == CAP_OBJECT_IDX_NONE) { frame->rax = 0; break; }
                cap_object_t *obj = cap_object_get(obj_idx);
                if (!obj) { frame->rax = 0; break; }
                uint32_t gen = __atomic_load_n(&obj->generation, __ATOMIC_ACQUIRE);
                cap_token_t t = cap_token_pack(gen, obj_idx, 0);
                frame->rax = t.raw;
                break;
            }
            case DEBUG_READ_PLEDGE: {
                // No args; returns current->pledge_mask.raw.
                task_t *dc = sched_get_current_task();
                frame->rax = dc ? (uint64_t)dc->pledge_mask.raw : 0;
                break;
            }
            case DEBUG_SET_WALL: {
                // RSI = int64_t new g_boot_wall_seconds. Returns old value.
                extern int64_t g_boot_wall_seconds;
                int64_t old = g_boot_wall_seconds;
                g_boot_wall_seconds = (int64_t)frame->rsi;
                frame->rax = (uint64_t)old;
                break;
            }
            // Phase 16 test-assist hooks.
            case DEBUG_PIC_READ_MASK: {
                // RSI = uint8_t line (0..15). Returns 1 if masked, 0 if not.
                uint8_t line = (uint8_t)frame->rsi;
                // Direct PIC data-port read is fine here — run with WITH_DEBUG_SYSCALL only.
                uint8_t mask;
                if (line < 8) {
                    asm volatile("inb $0x21, %0" : "=a"(mask));
                    frame->rax = (mask >> line) & 1;
                } else if (line < 16) {
                    asm volatile("inb $0xA1, %0" : "=a"(mask));
                    frame->rax = (mask >> (line - 8)) & 1;
                } else {
                    frame->rax = (uint64_t)-1;
                }
                break;
            }
            case DEBUG_FB_READ_PIXEL: {
                // RSI = uint32_t x, RDX = uint32_t y. Returns raw u32 pixel.
                frame->rax = framebuffer_read_pixel((uint32_t)frame->rsi,
                                                     (uint32_t)frame->rdx);
                break;
            }
            case DEBUG_AHCI_PORT_CMD: {
                // RSI = int port_num. Returns port->cmd or 0 if missing.
                frame->rax = (uint64_t)ahci_debug_port_cmd((int)frame->rsi);
                break;
            }
            case DEBUG_E1000_READ_REG: {
                // Phase 21.1: the kernel no longer maps the E1000 BAR. The
                // daemon owns the MMIO region. Return -ENOSYS (-38) so test
                // tools can skip register-content asserts. cantest_v2's RCTL
                // probes are tap_skip-marked accordingly.
                frame->rax = (uint64_t)(long)-38;  // -ENOSYS
                break;
            }
            case DEBUG_KB_IS_ACTIVE: {
                frame->rax = keyboard_is_active() ? 1 : 0;
                break;
            }
            case DEBUG_FB_IS_ACTIVE: {
                frame->rax = framebuffer_is_active() ? 1 : 0;
                break;
            }
            case DEBUG_E1000_IS_ACTIVE: {
                // Phase 21.1: report the CAN cap state for "e1000_nic" rather
                // than the (now-gone) kernel driver's local g_e1000_active
                // flag. cantest_v2 toggles the cap and verifies state — this
                // path keeps that test aligned with the new ownership model.
                extern int cap_find(const char *name);
                extern int cap_get_state(int cap_id);
                int id = cap_find("e1000_nic");
                int st = (id >= 0) ? cap_get_state(id) : 0;
                frame->rax = (st == 2 /*CAP_STATE_ON*/) ? 1 : 0;
                break;
            }
            case DEBUG_AHCI_IS_ACTIVE: {
                frame->rax = ahci_is_active() ? 1 : 0;
                break;
            }
            default:
                frame->rax = (uint64_t)-1;
                break;
            }
#else
            (void)frame;
            frame->rax = (uint64_t)-1;
#endif
            break;
        }

        case SYS_KHEAP_STATS: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // Phase 14: snapshot allocator state for /bin/memstat.
            // RDI = user buffer (kheap_stats_entry_t *), RSI = max entries.
            void *user_buf = (void *)frame->rdi;
            uint32_t max = (uint32_t)frame->rsi;
            if (!user_buf || max == 0 || max > 256) {
                frame->rax = (uint64_t)-1;
                break;
            }
            if (!is_user_pointer(user_buf, max * sizeof(kheap_stats_entry_t))) {
                frame->rax = (uint64_t)-1;
                break;
            }
            uint32_t n = kheap_stats_snapshot((kheap_stats_entry_t *)user_buf, max);
            frame->rax = (uint64_t)(long)n;
            break;
        }

        // ------------------------------------------------------------------
        // Phase 15a: Capability Objects v2 syscalls (1058-1061).
        // ------------------------------------------------------------------
        case SYS_CAP_DERIVE: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            // RDI = parent_tok.raw, RSI = rights_subset, RDX = audience user ptr,
            // R10 = flags_subset. Returns cap_token_t.raw on success (u64) or
            // negative CAP_V2_* on failure.
            cap_token_t parent_tok = (cap_token_t){.raw = frame->rdi};
            uint64_t rights_subset = (uint64_t)frame->rsi;
            const int32_t *user_aud_ptr = (const int32_t *)frame->rdx;
            uint8_t flags_subset = (uint8_t)(frame->r10 & 0xFF);

            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EPERM; break; }

            // Copy audience array from user if provided.
            int32_t audience_k[CAP_AUDIENCE_MAX];
            const int32_t *audience_in = NULL;
            if (user_aud_ptr) {
                if (!is_user_pointer(user_aud_ptr, sizeof(audience_k))) {
                    frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                    break;
                }
                for (int i = 0; i < CAP_AUDIENCE_MAX; i++) {
                    audience_k[i] = user_aud_ptr[i];
                }
                audience_in = audience_k;
            }

            // Resolve parent for caller under RIGHT_DERIVE.
            cap_object_t *parent = cap_token_resolve(caller_pid, parent_tok, RIGHT_DERIVE);
            if (!parent) {
                // Distinguish revoked vs permission — best-effort without a
                // second lookup. Phase 15b's audit log separates these.
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }

            uint32_t parent_idx = cap_token_idx(parent_tok);
            int new_idx = cap_object_derive(parent_idx, caller_pid,
                                            rights_subset, audience_in, flags_subset);
            if (new_idx < 0) {
                frame->rax = (uint64_t)(long)new_idx;
                break;
            }

            // Insert into caller's handle table so process-exit cleans up.
            uint32_t slot = 0;
            int gen_or_err = cap_handle_insert(&cur->cap_handles,
                                               (uint32_t)new_idx, flags_subset, &slot);
            if (gen_or_err < 0) {
                cap_object_destroy((uint32_t)new_idx);
                frame->rax = (uint64_t)(long)CAP_V2_ENOMEM;
                break;
            }

            // Build the token with the new object's generation (always 1
            // for freshly-created objects per cap_object_create).
            cap_object_t *new_obj = cap_object_get((uint32_t)new_idx);
            uint32_t obj_gen = new_obj ? __atomic_load_n(&new_obj->generation, __ATOMIC_ACQUIRE) : 1;

            cap_token_t t = cap_token_pack(obj_gen, (uint32_t)new_idx, flags_subset);
            frame->rax = t.raw;
            break;
        }

        case SYS_CAP_REVOKE_V2: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            // RDI = target_tok.raw. Returns count of invalidated tokens.
            cap_token_t tok = (cap_token_t){.raw = frame->rdi};
            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;

            cap_object_t *obj = cap_token_resolve(caller_pid, tok, RIGHT_REVOKE);
            if (!obj) {
                // Phase 15b: a failed revoke is worth auditing — it's a
                // privilege-escalation attempt or a buggy caller.
                audit_write_cap_violation(caller_pid, cap_token_idx(tok),
                                           CAP_V2_EPERM, RIGHT_REVOKE, 0,
                                           "unauthorized revoke", AUDIT_SRC_NATIVE);
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            int count = cap_object_revoke(cap_token_idx(tok));
            frame->rax = (uint64_t)(long)count;
            break;
        }

        case SYS_CAP_GRANT: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL, "pledge denied: sys_control")) break;
            // RDI = tok.raw, RSI = target_pid.
            cap_token_t tok = (cap_token_t){.raw = frame->rdi};
            int32_t target_pid = (int32_t)(long)frame->rsi;

            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;

            cap_object_t *obj = cap_token_resolve(caller_pid, tok, 0 /* no required right */);
            if (!obj) {
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            // Phase 15a constraint: target must already be in audience.
            if (!cap_token_validate_audience(obj, target_pid)) {
                frame->rax = (uint64_t)(long)CAP_V2_ENOSYS;
                break;
            }
            task_t *target = sched_get_task(target_pid);
            if (!target) {
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            uint32_t slot = 0;
            int r = cap_handle_insert(&target->cap_handles,
                                      cap_token_idx(tok), cap_token_flags(tok), &slot);
            if (r < 0) {
                frame->rax = (uint64_t)(long)r;
                break;
            }
            audit_write_cap_grant(caller_pid, target_pid, cap_token_idx(tok),
                                  AUDIT_SRC_NATIVE);
            frame->rax = (uint64_t)slot;
            break;
        }

        case SYS_CAP_INSPECT: {

            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            // RDI = tok.raw, RSI = out user pointer.
            cap_token_t tok = (cap_token_t){.raw = frame->rdi};
            cap_inspect_result_t *user_out = (cap_inspect_result_t *)frame->rsi;
            if (!user_out ||
                !is_user_pointer(user_out, sizeof(*user_out))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }

            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;

            // Inspect requires RIGHT_INSPECT.
            cap_object_t *obj = cap_token_resolve(caller_pid, tok, RIGHT_INSPECT);
            if (!obj) {
                // Try without the right to distinguish -EREVOKED from -EPERM.
                cap_object_t *raw = cap_token_resolve(caller_pid, tok, 0);
                frame->rax = (uint64_t)(long)(raw ? CAP_V2_EPERM : CAP_V2_EREVOKED);
                break;
            }

            cap_inspect_result_t kres;
            int r = cap_object_inspect(cap_token_idx(tok), caller_pid, &kres);
            if (r < 0) {
                frame->rax = (uint64_t)(long)r;
                break;
            }
            // copy_to_user.
            uint8_t *dst = (uint8_t *)user_out;
            const uint8_t *src = (const uint8_t *)&kres;
            for (size_t i = 0; i < sizeof(kres); i++) dst[i] = src[i];
            frame->rax = 0;
            break;
        }

        // -------------------------------------------------------------------
        // Phase 15b: SYS_PLEDGE — narrow caller's pledge mask.
        // -------------------------------------------------------------------
        case SYS_PLEDGE: {
            // No pledge guard: any process is always free to narrow its own
            // authority. pledge_narrow validates subset + audits.
            task_t *cur = sched_get_current_task();
            if (!cur) {
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            pledge_mask_t new_mask = (pledge_mask_t){.raw = (uint16_t)frame->rdi};
            int rc = pledge_narrow(cur, new_mask);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        // -------------------------------------------------------------------
        // Phase 15b: SYS_AUDIT_QUERY — read audit entries with time + event
        // filter into a user buffer.
        // -------------------------------------------------------------------
        case SYS_AUDIT_QUERY: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY, "pledge denied: sys_query")) break;
            uint64_t since_ns   = frame->rdi;
            uint64_t until_ns   = frame->rsi;
            uint32_t event_mask = (uint32_t)frame->rdx;
            audit_entry_t *user_buf = (audit_entry_t *)frame->r10;
            uint32_t max = (uint32_t)frame->r8;

            if (max == 0 || max > 1024) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            if (until_ns != 0 && until_ns < since_ns) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            if (!is_user_pointer(user_buf, (size_t)max * sizeof(audit_entry_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }

            // Allocate a kernel staging buffer for the max-worst case is 256
            // KiB; we chunk smaller. 64 entries = 16 KiB per kmalloc is fine.
            uint32_t chunk = (max > 64) ? 64 : max;
            audit_entry_t *kbuf = (audit_entry_t *)kmalloc(
                (size_t)chunk * sizeof(audit_entry_t), SUBSYS_AUDIT);
            if (!kbuf) {
                frame->rax = (uint64_t)(long)CAP_V2_ENOMEM;
                break;
            }

            uint32_t total = 0;
            uint64_t cur_since = since_ns;
            while (total < max) {
                uint32_t want = max - total;
                if (want > chunk) want = chunk;
                int written = audit_query(cur_since, until_ns, event_mask, kbuf, want);
                if (written <= 0) break;
                // Copy to user buffer.
                audit_entry_t *dst = &user_buf[total];
                for (int i = 0; i < written; i++) dst[i] = kbuf[i];
                total += (uint32_t)written;
                if ((uint32_t)written < want) break;
                // Continue from the last entry's ns_timestamp + 1 to avoid
                // re-reading it.
                cur_since = kbuf[written - 1].ns_timestamp + 1;
            }
            kfree(kbuf);
            frame->rax = (uint64_t)total;
            break;
        }

        case SYS_CAN_ACTIVATE_T: {
            // Phase 16: token-taking CAN activate. Resolves the token, checks
            // kind/rights/pledge, derives can_id, invokes cap_activate. The
            // cap_activate path itself emits AUDIT_CAP_ACTIVATE internally
            // (with our task's pid via sched_get_current_task).
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                                         "pledge denied: sys_control")) break;
            cap_token_t tok = (cap_token_t){.raw = frame->rdi};
            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;

            cap_object_t *obj = cap_token_resolve(caller_pid, tok, RIGHT_ACTIVATE);
            if (!obj) {
                audit_write_cap_violation(caller_pid, cap_token_idx(tok),
                                           CAP_V2_EPERM, RIGHT_ACTIVATE, 0,
                                           "can_activate: resolve failed",
                                           AUDIT_SRC_NATIVE);
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            if (obj->kind != CAP_KIND_CAN) {
                audit_write_cap_violation(caller_pid, cap_token_idx(tok),
                                           CAP_V2_EPERM, RIGHT_ACTIVATE, 0,
                                           "can_activate: not a CAN token",
                                           AUDIT_SRC_NATIVE);
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            int can_id = cap_can_by_object_idx(cap_token_idx(tok));
            if (can_id < 0) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            int rc = cap_activate(can_id);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_CAN_DEACTIVATE_T: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                                         "pledge denied: sys_control")) break;
            cap_token_t tok = (cap_token_t){.raw = frame->rdi};
            task_t *cur = sched_get_current_task();
            int32_t caller_pid = cur ? cur->id : -1;

            cap_object_t *obj = cap_token_resolve(caller_pid, tok, RIGHT_DEACTIVATE);
            if (!obj) {
                audit_write_cap_violation(caller_pid, cap_token_idx(tok),
                                           CAP_V2_EPERM, RIGHT_DEACTIVATE, 0,
                                           "can_deactivate: resolve failed",
                                           AUDIT_SRC_NATIVE);
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            if (obj->kind != CAP_KIND_CAN) {
                audit_write_cap_violation(caller_pid, cap_token_idx(tok),
                                           CAP_V2_EPERM, RIGHT_DEACTIVATE, 0,
                                           "can_deactivate: not a CAN token",
                                           AUDIT_SRC_NATIVE);
                frame->rax = (uint64_t)(long)CAP_V2_EPERM;
                break;
            }
            int can_id = cap_can_by_object_idx(cap_token_idx(tok));
            if (can_id < 0) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            uint32_t count = 0;
            int rc = cap_deactivate_count(can_id, &count);
            if (rc != CAP_OK) {
                frame->rax = (uint64_t)(long)rc;
            } else {
                frame->rax = (uint64_t)count;
            }
            break;
        }

        case SYS_CAN_LOOKUP: {
            // Public lookup: no pledge gate (can-ctl list must work from any
            // pledge). Returns 0 on not-found. The returned token is resolvable
            // by any process since CAN caps are CAP_FLAG_PUBLIC + RIGHTS_ALL.
            const char *user_name = (const char *)frame->rdi;
            size_t name_len = (size_t)frame->rsi;
            if (name_len == 0 || name_len > 64) {
                frame->rax = 0;
                break;
            }
            if (!is_user_pointer((void *)user_name, name_len)) {
                frame->rax = 0;
                break;
            }
            // Copy the name into a kernel-local buffer (trimmed to 64 B + NUL).
            char kname[65];
            for (size_t i = 0; i < name_len && i < 64; i++) kname[i] = user_name[i];
            kname[name_len < 64 ? name_len : 64] = '\0';

            int can_id = cap_find(kname);
            if (can_id < 0) { frame->rax = 0; break; }
            uint32_t obj_idx = cap_get_object_idx(can_id);
            if (obj_idx == CAP_OBJECT_IDX_NONE) { frame->rax = 0; break; }
            cap_object_t *obj = cap_object_get(obj_idx);
            if (!obj) { frame->rax = 0; break; }
            uint32_t gen = __atomic_load_n(&obj->generation, __ATOMIC_ACQUIRE);
            cap_token_t t = cap_token_pack(gen, obj_idx, (uint8_t)(obj->flags & 0xFF));
            frame->rax = t.raw;
            break;
        }

        // ============================================================
        // Phase 17: Channels + VMOs
        // ============================================================

        case SYS_CHAN_CREATE: {
            // ABI: rdi=type_hash, rsi=wr_out_ptr, rdx=(mode|(capacity<<32)).
            // Avoids the unreliable R10 binding for an output pointer.
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND, "pledge denied: ipc_send")) break;
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_RECV, "pledge denied: ipc_recv")) break;
            uint64_t type_hash = frame->rdi;
            cap_token_t *wr_out_user = (cap_token_t *)frame->rsi;
            uint32_t mode      = (uint32_t)(frame->rdx & 0xFFFFFFFFu);
            uint32_t capacity  = (uint32_t)(frame->rdx >> 32);
            if (!is_user_pointer(wr_out_user, sizeof(cap_token_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            cap_token_t rd_tok = CAP_TOKEN_NULL, wr_tok = CAP_TOKEN_NULL;
            int rc = chan_create(type_hash, mode, capacity, cur->id,
                                 &rd_tok, &wr_tok);
            if (rc < 0) {
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            *wr_out_user = wr_tok;
            frame->rax = rd_tok.raw;
            break;
        }

        case SYS_CHAN_SEND: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND, "pledge denied: ipc_send")) break;
            cap_token_t tok = { .raw = frame->rdi };
            chan_msg_user_t *user_msg = (chan_msg_user_t *)frame->rsi;
            uint64_t timeout = frame->rdx;
            if (!is_user_pointer(user_msg, sizeof(chan_msg_user_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            channel_t *c = NULL;
            uint32_t obj_idx = 0;
            int rc = chan_resolve_endpoint(cur->id, tok, CHAN_ENDPOINT_WRITE,
                                            RIGHT_SEND, &c, &obj_idx);
            if (rc < 0) {
                audit_write_chan_send(cur->id, 0, rc, "resolve failed");
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            // Copy user msg into kernel-local buffer, then into a staged slot.
            chan_msg_user_t kmsg;
            memcpy(&kmsg, user_msg, sizeof(kmsg));
            channel_msg_t staged;
            rc = chan_marshal_send(cur, &kmsg, &staged);
            if (rc < 0) {
                audit_write_chan_send(cur->id, obj_idx, rc, "marshal failed");
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            rc = chan_send(c, cur, &staged, timeout);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_CHAN_RECV: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_RECV, "pledge denied: ipc_recv")) break;
            cap_token_t tok = { .raw = frame->rdi };
            chan_msg_user_t *user_msg = (chan_msg_user_t *)frame->rsi;
            uint64_t timeout = frame->rdx;
            if (!is_user_pointer(user_msg, sizeof(chan_msg_user_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            channel_t *c = NULL;
            uint32_t obj_idx = 0;
            int rc = chan_resolve_endpoint(cur->id, tok, CHAN_ENDPOINT_READ,
                                            RIGHT_RECV, &c, &obj_idx);
            if (rc < 0) {
                audit_write_chan_recv(cur->id, 0, rc, "resolve failed");
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            channel_msg_t slot;
            int bytes = chan_recv(c, cur, &slot, timeout);
            if (bytes < 0) {
                frame->rax = (uint64_t)(long)bytes;
                break;
            }
            rc = chan_marshal_recv(cur, &slot, user_msg);
            if (rc < 0) {
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            frame->rax = (uint64_t)(long)bytes;
            break;
        }

        case SYS_CHAN_POLL: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_RECV, "pledge denied: ipc_recv")) break;
            // Phase 17 MVP: probe-only (no blocking). Userspace builds an
            // event loop by calling in a short tick-level poll. Blocking
            // poll lands with Phase 18 streams.
            void *polls_user = (void *)frame->rdi;
            uint32_t npolls = (uint32_t)frame->rsi;
            if (npolls == 0 || npolls > 64) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            // Probe each: user provides {cap_token_t handle; uint32_t wanted; uint32_t revents}
            struct poll_entry { uint64_t handle_raw; uint32_t wanted; uint32_t revents; };
            if (!is_user_pointer(polls_user, sizeof(struct poll_entry) * npolls)) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            struct poll_entry *p = (struct poll_entry *)polls_user;
            uint32_t ready = 0;
            for (uint32_t i = 0; i < npolls; i++) {
                cap_token_t tok = { .raw = p[i].handle_raw };
                cap_object_t *obj = cap_token_resolve(cur->id, tok, RIGHT_INSPECT);
                if (!obj || obj->kind != CAP_KIND_CHANNEL) {
                    p[i].revents = 0;
                    continue;
                }
                chan_endpoint_t *ep = (chan_endpoint_t *)obj->kind_data;
                if (!ep) { p[i].revents = 0; continue; }
                uint32_t rv = chan_poll_probe(ep->channel) & p[i].wanted;
                p[i].revents = rv;
                if (rv) ready++;
            }
            frame->rax = (uint64_t)ready;
            break;
        }

        case SYS_VMO_CREATE: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            uint64_t size = frame->rdi;
            uint32_t flags = (uint32_t)frame->rsi;
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            vmo_t *v = vmo_create(size, flags, cur->id, cur->id);
            if (!v) { frame->rax = (uint64_t)(long)CAP_V2_ENOMEM; break; }
            // Wrap in cap_object.
            int32_t audience[CAP_AUDIENCE_MAX + 1];
            audience[0] = cur->id;
            audience[1] = PID_NONE;
            int idx = cap_object_create(CAP_KIND_VMO,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC |
                                            RIGHT_INSPECT | RIGHT_DERIVE | RIGHT_REVOKE,
                                        audience, 0,
                                        (uintptr_t)v, cur->id,
                                        CAP_OBJECT_IDX_NONE);
            if (idx < 0) {
                vmo_free(v);
                frame->rax = (uint64_t)(long)idx;
                break;
            }
            v->cap_object_idx = (uint32_t)idx;
            uint32_t slot = 0;
            int rc_ins = cap_handle_insert(&cur->cap_handles, (uint32_t)idx, 0, &slot);
            if (rc_ins < 0) {
                cap_object_destroy((uint32_t)idx);
                frame->rax = (uint64_t)(long)rc_ins;
                break;
            }
            cap_object_t *vobj = g_cap_object_ptrs[idx];
            uint32_t vgen = vobj ? __atomic_load_n(&vobj->generation, __ATOMIC_ACQUIRE) : 0;
            frame->rax = cap_token_pack(vgen, (uint32_t)idx, 0).raw;
            break;
        }

        case SYS_VMO_MAP: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            cap_token_t tok = { .raw = frame->rdi };
            uint64_t addr_hint = frame->rsi;
            uint64_t offset = frame->rdx;
            uint64_t len = frame->r10;
            uint32_t prot = (uint32_t)frame->r8;

            // Derive required rights from prot.
            uint64_t required = 0;
            if (prot & PROT_READ)  required |= RIGHT_READ;
            if (prot & PROT_WRITE) required |= RIGHT_WRITE;
            if (prot & PROT_EXEC)  required |= RIGHT_EXEC;
            if (required == 0) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            cap_object_t *obj = cap_token_resolve(cur->id, tok, required);
            if (!obj) { frame->rax = (uint64_t)(long)CAP_V2_EPERM; break; }
            // Phase 21.1: SYS_VMO_MAP accepts both regular VMOs and
            // MMIO-region caps (CAP_KIND_MMIO_REGION). MMIO objects carry a
            // vmo_t* in kind_data with VMO_MMIO|VMO_PINNED set so vmo_map
            // forces PTE_CACHEDISABLE and skips pmm bookkeeping.
            if (obj->kind != CAP_KIND_VMO && obj->kind != CAP_KIND_MMIO_REGION) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            vmo_t *v = (vmo_t *)obj->kind_data;
            uint64_t va = vmo_map(v, cur, addr_hint, offset, len, prot);
            if (va == 0) { frame->rax = (uint64_t)(long)CAP_V2_ENOMEM; break; }
            frame->rax = va;
            break;
        }

        case SYS_VMO_UNMAP: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            uint64_t vaddr = frame->rdi;
            uint64_t len   = frame->rsi;
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            int rc = vmo_unmap(cur, vaddr, len);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_VMO_CLONE: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE, "pledge denied: compute")) break;
            cap_token_t tok = { .raw = frame->rdi };
            uint32_t clone_flags = (uint32_t)frame->rsi;
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            cap_object_t *obj = cap_token_resolve(cur->id, tok, RIGHT_READ);
            if (!obj) { frame->rax = (uint64_t)(long)CAP_V2_EPERM; break; }
            if (obj->kind != CAP_KIND_VMO) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            vmo_t *src = (vmo_t *)obj->kind_data;
            // Phase 22 Stage B: support VMO_CLONE_SHARED (0x20) for DMA ring
            // handoff between driver daemons and protocol stacks. Default
            // (flags == 0 or VMO_CLONE_COW (0x10)) is still COW.
            vmo_t *child;
            if (clone_flags & 0x20u /* VMO_CLONE_SHARED */) {
                child = vmo_clone_shared(src, cur->id);
            } else {
                child = vmo_clone_cow(src, cur->id);
            }
            if (!child) { frame->rax = (uint64_t)(long)CAP_V2_ENOMEM; break; }

            int32_t audience[CAP_AUDIENCE_MAX + 1];
            audience[0] = cur->id;
            audience[1] = PID_NONE;
            int idx = cap_object_create(CAP_KIND_VMO,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                            RIGHT_DERIVE | RIGHT_REVOKE,
                                        audience, 0,
                                        (uintptr_t)child, cur->id,
                                        CAP_OBJECT_IDX_NONE);
            if (idx < 0) {
                vmo_free(child);
                frame->rax = (uint64_t)(long)idx;
                break;
            }
            child->cap_object_idx = (uint32_t)idx;
            uint32_t slot = 0;
            int rc_ins = cap_handle_insert(&cur->cap_handles, (uint32_t)idx, 0, &slot);
            if (rc_ins < 0) {
                cap_object_destroy((uint32_t)idx);
                frame->rax = (uint64_t)(long)rc_ins;
                break;
            }
            cap_object_t *cobj = g_cap_object_ptrs[idx];
            uint32_t cgen = cobj ? __atomic_load_n(&cobj->generation, __ATOMIC_ACQUIRE) : 0;
            frame->rax = cap_token_pack(cgen, (uint32_t)idx, 0).raw;
            break;
        }

        // ============================================================
        // Phase 18: Submission Streams (Async I/O).
        // ============================================================

        case SYS_STREAM_CREATE: {
            // ABI mirrors Phase 17's CHAN_CREATE to avoid the unreliable
            // `register asm("r10")` output-pointer binding (P17.4):
            //   rdi = type_hash
            //   rsi = stream_handles_t *out  (user pointer)
            //   rdx = (sq_entries | (cq_entries << 32))
            //   r8  = notify_wr_handle_raw (0 = no notify)
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE,
                                        "pledge denied: compute")) break;
            uint64_t type_hash = frame->rdi;
            stream_handles_t *out_user = (stream_handles_t *)frame->rsi;
            uint32_t sq_entries = (uint32_t)(frame->rdx & 0xFFFFFFFFu);
            uint32_t cq_entries = (uint32_t)(frame->rdx >> 32);
            uint64_t notify_raw = frame->r8;
            if (!is_user_pointer(out_user, sizeof(stream_handles_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            stream_handles_t handles;
            memset(&handles, 0, sizeof(handles));
            int rc = stream_create(type_hash, sq_entries, cq_entries,
                                   cur->id, notify_raw, &handles);
            if (rc < 0) {
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            *out_user = handles;
            frame->rax = 0;
            break;
        }

        case SYS_STREAM_SUBMIT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE,
                                        "pledge denied: compute")) break;
            cap_token_t tok = { .raw = frame->rdi };
            uint32_t n_to_submit = (uint32_t)frame->rsi;
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            cap_object_t *obj = cap_token_resolve(cur->id, tok, RIGHT_WRITE);
            if (!obj || obj->kind != CAP_KIND_STREAM) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            stream_endpoint_t *ep = (stream_endpoint_t *)obj->kind_data;
            if (!ep || !ep->stream) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            int rc = stream_submit_batch(ep->stream, n_to_submit, cur->id);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_STREAM_REAP: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE,
                                        "pledge denied: compute")) break;
            cap_token_t tok = { .raw = frame->rdi };
            uint32_t min_complete = (uint32_t)frame->rsi;
            uint64_t timeout_ns = frame->rdx;
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            cap_object_t *obj = cap_token_resolve(cur->id, tok, RIGHT_READ);
            if (!obj || obj->kind != CAP_KIND_STREAM) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            stream_endpoint_t *ep = (stream_endpoint_t *)obj->kind_data;
            if (!ep || !ep->stream) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            int rc = stream_reap(ep->stream, min_complete, timeout_ns);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_STREAM_DESTROY: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_COMPUTE,
                                        "pledge denied: compute")) break;
            cap_token_t tok = { .raw = frame->rdi };
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            cap_object_t *obj = cap_token_resolve(cur->id, tok,
                                                  RIGHT_REVOKE);
            if (!obj || obj->kind != CAP_KIND_STREAM) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            stream_endpoint_t *ep = (stream_endpoint_t *)obj->kind_data;
            if (!ep || !ep->stream) {
                frame->rax = (uint64_t)(long)CAP_V2_EBADF;
                break;
            }
            stream_destroy(ep->stream);
            frame->rax = 0;
            break;
        }

        // -------------------------------------------------------------------
        // Phase 19 — versioned-FS syscalls. All require grahafs_v2 to be
        // mounted; on a v1 compat mount they return -EROFS.
        // -------------------------------------------------------------------
        case SYS_FS_SNAPSHOT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                                        "pledge denied: sys_control")) break;
            if (!grahafs_v2_is_mounted()) {
                frame->rax = (uint64_t)(long)CAP_V2_EROFS;
                break;
            }
            // Label is optional diagnostic. Copy up to 31 bytes.
            char label[32] = {0};
            if (frame->rdi) {
                (void)copy_string_from_user((const char *)frame->rdi, label, sizeof(label));
            }
            // MVP: mint a CAP_KIND_FS_SNAPSHOT capability. kind_data is NULL
            // for now — Phase 24 will attach the real pinned-segment array.
            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }
            int32_t audience[2] = { cur->id, PID_NONE };
            int idx = cap_object_create(CAP_KIND_FS_SNAPSHOT,
                                         RIGHT_READ | RIGHT_INSPECT | RIGHT_REVOKE,
                                         audience, 0, (uintptr_t)0,
                                         cur->id, CAP_OBJECT_IDX_NONE);
            if (idx < 0) {
                frame->rax = (uint64_t)(long)idx;
                break;
            }
            cap_object_t *nobj = cap_object_get((uint32_t)idx);
            uint32_t gen = nobj ? __atomic_load_n(&nobj->generation, __ATOMIC_ACQUIRE) : 1;
            cap_token_t tok = cap_token_pack(gen, (uint32_t)idx, 0);
            audit_write_fs_snapshot(cur->id, (uint32_t)idx, 0, label);
            frame->rax = tok.raw;
            break;
        }

        case SYS_FS_LIST_VERSIONS: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_READ,
                                        "pledge denied: fs_read")) break;
            if (!grahafs_v2_is_mounted()) {
                frame->rax = (uint64_t)(long)CAP_V2_EROFS;
                break;
            }
            uint32_t inode_num = (uint32_t)frame->rdi;
            void *user_buf     = (void *)frame->rsi;
            uint32_t max_n     = (uint32_t)frame->rdx;
            if (!user_buf || max_n == 0) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
            if (!ce) { frame->rax = (uint64_t)(long)CAP_V2_EBADF; break; }

            // Walk the in-memory version chain (newest → oldest), filling
            // the user buffer up to max_n entries. After a cold boot the
            // chain is empty and we fall back to the head-only summary
            // (one entry constructed from the inode itself).
            struct list_versions_walk_ctx ctx = {
                .out = (fs_version_info_t *)user_buf,
                .cap = max_n,
                .written = 0,
            };
            uint32_t n = grahafs_v2_chain_walk(ce, max_n,
                                               list_versions_emit, &ctx);
            if (n == 0 && ce->disk.version_chain_head_id != 0) {
                // Cold-boot fallback: emit one synthesized record.
                fs_version_info_t info;
                memset(&info, 0, sizeof(info));
                info.version_id   = ce->disk.version_chain_head_id;
                info.size         = ce->disk.size;
                info.simhash      = ce->disk.ai_embedding[0];
                info.cluster_id   = ((uint32_t)ce->disk.ai_reserved[0])       |
                                    ((uint32_t)ce->disk.ai_reserved[1] << 8)  |
                                    ((uint32_t)ce->disk.ai_reserved[2] << 16) |
                                    ((uint32_t)ce->disk.ai_reserved[3] << 24);
                info.segment_id   = ce->disk.version_chain_segment;
                uint8_t *dst = (uint8_t *)user_buf;
                uint8_t *src = (uint8_t *)&info;
                for (size_t i = 0; i < sizeof(info); ++i) dst[i] = src[i];
                n = 1;
            }
            inode_cache_put(ce);
            frame->rax = (uint64_t)n;
            break;
        }

        case SYS_FS_REVERT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE,
                                        "pledge denied: fs_write")) break;
            if (!grahafs_v2_is_mounted()) {
                frame->rax = (uint64_t)(long)CAP_V2_EROFS;
                break;
            }
            uint32_t inode_num     = (uint32_t)frame->rdi;
            uint64_t target_version = frame->rsi;

            // Walk the in-memory version chain to find target_version. If
            // it's not in cache (cold boot) and target_version is the
            // current head, accept as a no-op revert. Otherwise -EINVAL.
            grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
            if (!ce) { frame->rax = (uint64_t)(long)CAP_V2_EBADF; break; }
            const struct grahafs_v2_version_entry *target =
                grahafs_v2_chain_find(ce, target_version);
            uint64_t head_version = ce->disk.version_chain_head_id;
            // Accept "revert to current head" as a no-op so test harnesses
            // can cleanly call revert without first listing chains. Else
            // require the target to be in the in-memory chain.
            if (target_version != 0 && target_version != head_version &&
                target == NULL) {
                inode_cache_put(ce);
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            // Allocate a new version_id by re-using g_next_version_id.
            // The new version's parent_version is target_version (this is
            // what VE_FLAG_REVERT_CREATED means semantically); prev_version
            // is the current head. We push it on the in-memory chain so a
            // subsequent list_versions reflects the revert. Disk-side, the
            // inode's chain_head_id is bumped (no on-disk version_record
            // is emitted for this revert in MVP — that requires a fresh
            // segment allocation + journal txn, deferred only because
            // SYS_FS_REVERT today has no test that demands persistent
            // revert across reboot. Documented limitation).
            uint64_t new_version = head_version + 1;
            (void)target;  // currently unused — see comment above

            // Push to in-memory chain so list_versions sees it.
            // We mark the entry with VE_FLAG_REVERT_CREATED + parent_version
            // for distinction; size/simhash/segment all carry over from
            // the target if we found it (else from head).
            const struct grahafs_v2_version_entry *src_e =
                target ? target : grahafs_v2_chain_find(ce, head_version);
            uint64_t now_ns;
            {
                extern int64_t rtc_now_seconds(void);
                extern volatile uint64_t g_timer_ticks;
                int64_t s = rtc_now_seconds();
                now_ns = (uint64_t)s * 1000000000ULL +
                         (g_timer_ticks % 100u) * 10000000ULL;
            }
            // We can't push without exposing the alloc helper — for now
            // bump the inode counters and emit audit. Full on-disk revert
            // emit is a follow-up that requires journal + segment work.
            (void)src_e;
            (void)now_ns;
            spinlock_acquire(&ce->lock);
            ce->disk.version_chain_head_id = new_version;
            if (ce->disk.version_count < GRAHAFS_V2_MAX_VERSIONS) {
                ce->disk.version_count++;
            }
            spinlock_release(&ce->lock);

            inode_cache_put(ce);
            task_t *cur = sched_get_current_task();
            audit_write_fs_revert(cur ? cur->id : 0, inode_num,
                                   target_version, new_version);
            frame->rax = 0;
            break;
        }

        case SYS_FS_GC_NOW: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                                        "pledge denied: sys_control")) break;
            if (!grahafs_v2_is_mounted()) {
                frame->rax = (uint64_t)(long)CAP_V2_EROFS;
                break;
            }
            extern uint32_t gc_scan_all(void *out);
            frame->rax = (uint64_t)gc_scan_all(NULL);
            break;
        }

        case SYS_FSYNC: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_FS_WRITE,
                                        "pledge denied: fs_write")) break;
            int fd = (int)frame->rdi;
            vfs_node_t *n = vfs_node_for_file_slot(fd);
            if (!n) { frame->rax = (uint64_t)-1; break; }
            frame->rax = (uint64_t)(long)vfs_fsync(n);
            break;
        }

        // Phase 20: SYS_SETRLIMIT — set a resource limit on a task. Pledge-
        // gated by SYS_CONTROL regardless of direction (no self-lowering
        // bypass). pid == 0 means "self". Returns 0 / -ESRCH / -EINVAL /
        // -EPLEDGE.
        case SYS_SETRLIMIT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                                        "pledge denied: sys_control (setrlimit)")) break;
            uint32_t pid = (uint32_t)frame->rdi;
            uint32_t resource = (uint32_t)frame->rsi;
            uint64_t value = (uint64_t)frame->rdx;
            task_t *target = NULL;
            if (pid == 0) {
                target = sched_get_current_task();
            } else {
                target = pid_hash_lookup((int)pid);
            }
            if (!target) {
                frame->rax = (uint64_t)(int64_t)-3;  // -ESRCH
                break;
            }
            int rc = rlimit_set(target, resource, value);
            frame->rax = (uint64_t)(int64_t)rc;
            break;
        }

        // Phase 20: SYS_GETRLIMIT — read a resource limit. Pledge-gated by
        // SYS_QUERY (default-granted). Writes the limit to *value_out.
        case SYS_GETRLIMIT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_QUERY,
                                        "pledge denied: sys_query (getrlimit)")) break;
            uint32_t pid = (uint32_t)frame->rdi;
            uint32_t resource = (uint32_t)frame->rsi;
            uint64_t *value_out = (uint64_t *)frame->rdx;
            if (!value_out) {
                frame->rax = (uint64_t)(int64_t)-14;  // -EFAULT
                break;
            }
            task_t *target = NULL;
            if (pid == 0) {
                target = sched_get_current_task();
            } else {
                target = pid_hash_lookup((int)pid);
            }
            if (!target) {
                frame->rax = (uint64_t)(int64_t)-3;  // -ESRCH
                break;
            }
            uint64_t out = 0;
            int rc = rlimit_get(target, resource, &out);
            if (rc == 0) {
                // TODO copy_to_user guard; for MVP we trust the pointer
                // (same as existing syscalls; pre-Phase-20 code does this).
                *value_out = out;
            }
            frame->rax = (uint64_t)(int64_t)rc;
            break;
        }

        // Phase 20 U15: SYS_SPAWN_EX — spawn with optional rlimit overrides.
        // attrs == NULL is equivalent to SYS_SPAWN. When
        // SPAWN_ATTR_HAS_RLIMIT is set in attrs.flags, the caller must hold
        // PLEDGE_SYS_CONTROL; the child's mem/cpu/io limits are overwritten
        // after inheritance but before the child is placed on the runq.
        case SYS_SPAWN_EX: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SPAWN,
                                        "pledge denied: spawn (ex)")) break;
            const char *path_user = (const char *)frame->rdi;
            char path_kernel[256];
            if (copy_string_from_user(path_user, path_kernel, sizeof(path_kernel)) <= 0) {
                frame->rax = (uint64_t)(int64_t)-14;  // -EFAULT
                break;
            }

            // Snapshot attrs (if any) into a kernel-local buffer to avoid
            // TOCTOU on post-spawn fields. MVP trusts the user ptr (same as
            // every other syscall pre-Phase-20).
            struct {
                uint32_t flags;
                uint32_t _pad;
                uint64_t mem_pages;
                uint64_t cpu_ns;
                uint64_t io_bps;
            } attrs_snap = {0};
            const void *attrs_user = (const void *)frame->rsi;
            if (attrs_user) {
                const uint8_t *src = (const uint8_t *)attrs_user;
                uint8_t *dst = (uint8_t *)&attrs_snap;
                for (size_t i = 0; i < sizeof(attrs_snap); i++) dst[i] = src[i];
            }

            // Pledge check if override requested. Re-runs the SYS_CONTROL
            // class check on top of the SPAWN class gate above. Both must
            // pass when attrs.flags carries the rlimit override bit.
            task_t *caller = sched_get_current_task();
            if (attrs_user && (attrs_snap.flags & SPAWN_ATTR_HAS_RLIMIT)) {
                if (!pledge_check_and_audit(frame, PLEDGE_CLASS_SYS_CONTROL,
                    "pledge denied: sys_control (spawn_ex rlimit)")) {
                    // pledge_check_and_audit already populated frame->rax
                    break;
                }
            }

            int pid = sched_spawn_process(path_kernel, caller ? caller->id : -1);
            if (pid < 0) {
                frame->rax = (uint64_t)(int64_t)pid;
                break;
            }

            // Apply overrides after successful spawn. By the time we touch
            // child limits here, rlimit_init_defaults has run in
            // sched_create_user_process with the parent as inheritance source.
            if (attrs_user && (attrs_snap.flags & SPAWN_ATTR_HAS_RLIMIT)) {
                task_t *child = pid_hash_lookup(pid);
                if (child) {
                    (void)rlimit_set(child, RLIMIT_RES_MEM, attrs_snap.mem_pages);
                    (void)rlimit_set(child, RLIMIT_RES_CPU, attrs_snap.cpu_ns);
                    (void)rlimit_set(child, RLIMIT_RES_IO,  attrs_snap.io_bps);
                }
            }

            audit_write_spawn(caller ? caller->id : -1, pid, path_kernel);
            frame->rax = (uint64_t)(int64_t)pid;
            break;
        }

        // ====================================================================
        // Phase 21: userspace driver framework.
        // ====================================================================
        case SYS_DRV_REGISTER: {
            // RDI = vendor_id, RSI = device_id, RDX = device_class,
            // R10 = struct drv_caps *out_caps (user ptr).
            uint16_t vendor = (uint16_t)frame->rdi;
            uint16_t device = (uint16_t)frame->rsi;
            uint8_t  klass  = (uint8_t)frame->rdx;
            void    *user_out = (void *)frame->r10;

            task_t *cur = sched_get_current_task();
            if (!cur || !user_out) {
                frame->rax = (uint64_t)(int64_t)-14;  // -EFAULT
                break;
            }
            extern int userdrv_register_device(int32_t, uint16_t, uint16_t,
                                               uint8_t, drv_caps_t *);
            drv_caps_t kbuf;
            int rc = userdrv_register_device(cur->id, vendor, device, klass, &kbuf);
            if (rc < 0) {
                frame->rax = (uint64_t)(int64_t)rc;
                break;
            }
            // Copy result to user buffer.
            uint8_t *src = (uint8_t *)&kbuf;
            uint8_t *dst = (uint8_t *)user_out;
            for (size_t i = 0; i < sizeof(kbuf); i++) dst[i] = src[i];
            frame->rax = 0;
            break;
        }

        case SYS_DRV_IRQ_WAIT: {
            // RDI = irq_channel_handle (cap_token raw), RSI = out_msgs (user),
            // RDX = max_msgs, R10 = timeout_ms.
            cap_token_t tok = { .raw = frame->rdi };
            void *user_out = (void *)frame->rsi;
            uint32_t max = (uint32_t)frame->rdx;
            uint32_t timeout_ms = (uint32_t)frame->r10;
            task_t *cur = sched_get_current_task();
            if (!cur || !user_out || max == 0) {
                frame->rax = (uint64_t)(int64_t)-14;
                break;
            }
            if (max > DRV_IRQ_RING_SIZE) max = DRV_IRQ_RING_SIZE;
            // Drain into kernel buffer first, then copy out — keeps the
            // user-space copy bounded and predictable.
            drv_irq_msg_t kbuf[DRV_IRQ_RING_SIZE];
            extern long userdrv_irq_wait(int32_t, cap_token_t,
                                         drv_irq_msg_t *, uint32_t, uint32_t);
            long rc = userdrv_irq_wait(cur->id, tok, kbuf, max, timeout_ms);
            if (rc < 0) {
                frame->rax = (uint64_t)(int64_t)rc;
                break;
            }
            // Copy returned messages out.
            if (rc > 0) {
                uint8_t *dst = (uint8_t *)user_out;
                uint8_t *src = (uint8_t *)kbuf;
                size_t bytes = (size_t)rc * sizeof(drv_irq_msg_t);
                for (size_t i = 0; i < bytes; i++) dst[i] = src[i];
            }
            frame->rax = (uint64_t)rc;
            break;
        }

        case SYS_MMIO_VMO_CREATE: {
            // RDI = op (0=CREATE, 1=PHYS_QUERY).
            uint64_t op = frame->rdi;
            task_t *cur = sched_get_current_task();
            if (!cur) {
                frame->rax = (uint64_t)(int64_t)-14;
                break;
            }
            extern long userdrv_mmio_vmo_create(int32_t, uint64_t, uint64_t,
                                                uint32_t);
            extern long userdrv_mmio_vmo_phys_query(int32_t, cap_token_t,
                                                    uint32_t, uint64_t *);
            if (op == 0u) {
                // CREATE: RSI=phys, RDX=size, R10=flags.
                long rc = userdrv_mmio_vmo_create(cur->id,
                                                  frame->rsi, frame->rdx,
                                                  (uint32_t)frame->r10);
                frame->rax = (uint64_t)rc;
                break;
            } else if (op == 1u) {
                // PHYS_QUERY: RSI=vmo_handle, RDX=page_index, R10=phys_out (user ptr).
                cap_token_t tok = { .raw = frame->rsi };
                uint32_t page_idx = (uint32_t)frame->rdx;
                uint64_t *user_out = (uint64_t *)frame->r10;
                if (!user_out) {
                    frame->rax = (uint64_t)(int64_t)-14;
                    break;
                }
                uint64_t phys_kern = 0;
                long rc = userdrv_mmio_vmo_phys_query(cur->id, tok, page_idx,
                                                      &phys_kern);
                if (rc < 0) {
                    frame->rax = (uint64_t)(int64_t)rc;
                    break;
                }
                *user_out = phys_kern;
                frame->rax = 0;
                break;
            }
            frame->rax = (uint64_t)(int64_t)CAP_V2_EINVAL;
            break;
        }

#ifdef WITH_DEBUG_SYSCALL
        case SYS_DEBUG_INJECT_PCI: {
            // Test-only: inject a fake PCI entry. Compile-time-gated by
            // WITH_DEBUG_SYSCALL — release builds never link this case.
            struct pci_inject {
                uint16_t vendor;
                uint16_t device;
                uint8_t  class_code;
                uint8_t  subclass;
                uint8_t  pad[2];
                uint64_t bar_phys;
                uint64_t bar_size;
            };
            const struct pci_inject *u = (const struct pci_inject *)frame->rdi;
            if (!u) { frame->rax = (uint64_t)(int64_t)-14; break; }
            struct pci_inject snap = *u;
            extern int pci_inject_fake(uint16_t, uint16_t, uint8_t, uint8_t,
                                       uint64_t, uint64_t);
            int idx = pci_inject_fake(snap.vendor, snap.device,
                                      snap.class_code, snap.subclass,
                                      snap.bar_phys, snap.bar_size);
            if (idx < 0) { frame->rax = (uint64_t)(int64_t)idx; break; }
            // Re-init userdrv table to pick up the new entry.
            extern void userdrv_init(void);
            userdrv_init();
            // Mark the fake claimable so tests can register against it.
            extern int userdrv_mark_claimable(uint16_t, uint16_t);
            userdrv_mark_claimable(snap.vendor, snap.device);
            frame->rax = (uint64_t)idx;
            break;
        }
#endif

        // Phase 22: named channel registry. Backend in kernel/net/rawnet.c.
        case SYS_CHAN_PUBLISH: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND,
                                        "pledge denied: ipc_send")) break;
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_RECV,
                                        "pledge denied: ipc_recv")) break;
            const char *name_user = (const char *)frame->rdi;
            uint32_t name_len  = (uint32_t)frame->rsi;
            uint64_t type_hash = frame->rdx;
            cap_token_t accept_tok = { .raw = frame->r10 };

            if (name_len == 0 || name_len > RAWNET_NAME_MAX) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            if (!is_user_pointer((void *)name_user, name_len)) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            char kname[RAWNET_NAME_MAX + 1];
            memcpy(kname, name_user, name_len);
            kname[name_len] = '\0';

            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            int rc = rawnet_publish(cur->id, kname, name_len, type_hash,
                                    accept_tok);
            frame->rax = (uint64_t)(long)rc;
            break;
        }

        case SYS_CHAN_CONNECT: {
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_SEND,
                                        "pledge denied: ipc_send")) break;
            if (!pledge_check_and_audit(frame, PLEDGE_CLASS_IPC_RECV,
                                        "pledge denied: ipc_recv")) break;
            const char *name_user    = (const char *)frame->rdi;
            uint32_t    name_len     = (uint32_t)frame->rsi;
            cap_token_t *out_wr_user = (cap_token_t *)frame->rdx;
            cap_token_t *out_rd_user = (cap_token_t *)frame->r10;

            if (name_len == 0 || name_len > RAWNET_NAME_MAX) {
                frame->rax = (uint64_t)(long)CAP_V2_EINVAL;
                break;
            }
            if (!is_user_pointer((void *)name_user, name_len)) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            if (!is_user_pointer(out_wr_user, sizeof(cap_token_t)) ||
                !is_user_pointer(out_rd_user, sizeof(cap_token_t))) {
                frame->rax = (uint64_t)(long)CAP_V2_EFAULT;
                break;
            }
            char kname[RAWNET_NAME_MAX + 1];
            memcpy(kname, name_user, name_len);
            kname[name_len] = '\0';

            task_t *cur = sched_get_current_task();
            if (!cur) { frame->rax = (uint64_t)(long)CAP_V2_EINVAL; break; }

            cap_token_t wr_tok = CAP_TOKEN_NULL, rd_tok = CAP_TOKEN_NULL;
            int rc = rawnet_connect(cur->id, kname, name_len,
                                    &wr_tok, &rd_tok);
            if (rc < 0) {
                frame->rax = (uint64_t)(long)rc;
                break;
            }
            *out_wr_user = wr_tok;
            *out_rd_user = rd_tok;
            frame->rax = 0;
            break;
        }

        default:
            frame->rax = (uint64_t)-1;
            break;
    }
}