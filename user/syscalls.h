// user/syscalls.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../kernel/gcp.h" // For gcp_command_t

// Standard C library types
typedef long ssize_t;

// --- System Call Numbers ---
#define SYS_PUTC        1001
#define SYS_OPEN        1002
#define SYS_READ        1003
#define SYS_CLOSE       1004
#define SYS_GCP_EXECUTE 1005
#define SYS_GETC        1006
#define SYS_EXEC        1007
#define SYS_EXIT        1008
#define SYS_WAIT        1009
#define SYS_WRITE  1010
#define SYS_CREATE 1011
#define SYS_MKDIR  1012
#define SYS_STAT   1013
#define SYS_READDIR 1014
#define SYS_SYNC  1015

// Directory entry structure for user space
typedef struct {
    uint32_t type;
    char name[28];
} user_dirent_t;

// --- Syscall Wrappers (Inline Assembly) ---

static inline void syscall_putc(char c) {
    asm volatile("syscall" : : "a"(SYS_PUTC), "D"(c) : "rcx", "r11", "memory");
}

static inline int syscall_open(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_OPEN), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline ssize_t syscall_read(int fd, void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READ), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_close(int fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CLOSE), "D"(fd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_gcp_execute(gcp_command_t *cmd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GCP_EXECUTE), "D"(cmd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline char syscall_getc(void) {
    long ret;
    // This is a blocking call; the kernel won't return until a key is pressed.
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GETC) : "rcx", "r11", "memory");
    return (char)ret;
}

static inline int syscall_exec(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_EXEC), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_exit(int status) {
    asm volatile("syscall" : : "a"(SYS_EXIT), "D"(status) : "rcx", "r11", "memory");
    // Should never return
    while(1);
}

// NEW: wait() - Wait for any child process to exit
// Returns: child PID on success, -1 on error
// If status is non-NULL, the exit status of the child is stored there
static inline int syscall_wait(int *status) {
    long ret;
    
    // Keep retrying if we get the special retry value
    do {
        asm volatile("syscall" : "=a"(ret) : "a"(SYS_WAIT), "D"(status) : "rcx", "r11", "memory");
        
        // If we got -99, it means we were blocked and need to retry
        if (ret == -99) {
            // The kernel has woken us up, retry the syscall
            continue;
        }
        
        break;
    } while (1);
    
    return (int)ret;
}

// Convenience wrapper that ignores exit status
static inline int wait(void) {
    return syscall_wait(NULL);
}

static inline ssize_t syscall_write(int fd, const void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_WRITE), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_create(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CREATE), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_mkdir(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_MKDIR), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_readdir(const char *pathname, uint32_t index, user_dirent_t *dirent) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READDIR), "D"(pathname), "S"(index), "d"(dirent) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_sync(void) {
    asm volatile("syscall" : : "a"(SYS_SYNC) : "rcx", "r11", "memory");
}