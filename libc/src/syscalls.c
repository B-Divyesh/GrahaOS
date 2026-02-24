// libc/src/syscalls.c
// Phase 7c: System call wrappers for user-space programs

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// System call numbers
#define SYS_TEST        0
#define SYS_PUTC        1001
#define SYS_OPEN        1002
#define SYS_READ        1003
#define SYS_CLOSE       1004
#define SYS_GCP_EXECUTE 1005
#define SYS_GETC        1006
#define SYS_EXEC        1007
#define SYS_EXIT        1008
#define SYS_WAIT        1009
#define SYS_WRITE       1010
#define SYS_CREATE      1011
#define SYS_MKDIR       1012
#define SYS_STAT        1013
#define SYS_READDIR     1014
#define SYS_SYNC        1015
#define SYS_BRK         1016
#define SYS_SPAWN       1017
#define SYS_KILL        1018
#define SYS_SIGNAL      1019
#define SYS_GETPID      1020
#define SYS_GET_SYSTEM_STATE 1021
#define SYS_DEBUG       9999

// Generic syscall functions
static inline long syscall0(long n) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

// ===== FILE OPERATIONS =====

int open(const char *pathname, int flags, ...) {
    return (int)syscall1(SYS_OPEN, (long)pathname);
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_READ, fd, (long)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_WRITE, fd, (long)buf, count);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

// ===== PROCESS CONTROL =====

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    // Should never return - task is now ZOMBIE, scheduler will skip it
    // NOTE: Do NOT use hlt here - it's privileged and causes #GP in Ring 3
    while (1) {
        asm volatile("pause");
    }
}

void exit(int status) {
    // TODO: Call atexit handlers and flush buffers
    _exit(status);
}

void abort(void) {
    _exit(128 + 6); // SIGABRT = 6
}

int exec(const char *path) {
    return (int)syscall1(SYS_EXEC, (long)path);
}

// Wait for any child process
int wait(int *status) {
    long ret;
    do {
        ret = syscall1(SYS_WAIT, (long)status);
        // If we got -99, kernel blocked us and we need to retry
        if (ret == -99) {
            continue;
        }
        break;
    } while (1);
    return (int)ret;
}

// Get process ID
int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

// Spawn a new process (modern replacement for fork+exec)
int spawn(const char *path) {
    return (int)syscall1(SYS_SPAWN, (long)path);
}

// Send a signal to a process
int kill(int pid, int sig) {
    return (int)syscall2(SYS_KILL, pid, sig);
}

// Register a signal handler
void (*signal(int sig, void (*handler)(int)))(int) {
    return (void (*)(int))syscall2(SYS_SIGNAL, sig, (long)handler);
}

// ===== MEMORY MANAGEMENT =====

// Program break syscall
int brk(void *addr) {
    long ret = syscall1(SYS_BRK, (long)addr);
    if (ret == (long)-1) {
        return -1;
    }
    return 0;
}

// Increment program break
void *sbrk(intptr_t increment) {
    // Get current break
    long current_brk = syscall1(SYS_BRK, 0);
    if (current_brk == (long)-1) {
        return (void *)-1;
    }

    if (increment == 0) {
        return (void *)current_brk;
    }

    // Set new break
    long new_brk = syscall1(SYS_BRK, current_brk + increment);
    if (new_brk == (long)-1) {
        return (void *)-1;
    }

    // Return old break
    return (void *)current_brk;
}

// ===== FILE SYSTEM OPERATIONS =====

// Create a file
int create(const char *pathname, uint32_t mode) {
    return (int)syscall2(SYS_CREATE, (long)pathname, mode);
}

// Create a directory
int mkdir(const char *pathname, uint32_t mode) {
    return (int)syscall2(SYS_MKDIR, (long)pathname, mode);
}

// Get file status (placeholder)
int stat(const char *pathname, void *statbuf) {
    return (int)syscall2(SYS_STAT, (long)pathname, (long)statbuf);
}

// Read directory entry
int readdir(const char *pathname, uint32_t index, void *dirent) {
    return (int)syscall3(SYS_READDIR, (long)pathname, index, (long)dirent);
}

// Sync filesystem to disk
void sync(void) {
    syscall0(SYS_SYNC);
}

// ===== WORKING DIRECTORY (Placeholders) =====

char *getcwd(char *buf, size_t size) {
    // TODO: Implement SYS_GETCWD in kernel
    if (buf && size > 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

int chdir(const char *path) {
    // TODO: Implement SYS_CHDIR in kernel
    return -1;
}

// ===== SLEEP (Placeholders) =====

unsigned int sleep(unsigned int seconds) {
    // TODO: Implement SYS_NANOSLEEP in kernel
    // For now, busy-wait (not recommended, but works)
    for (unsigned int i = 0; i < seconds; i++) {
        for (volatile unsigned long j = 0; j < 100000000; j++);
    }
    return 0;
}

int usleep(unsigned int usec) {
    // TODO: Implement SYS_NANOSLEEP in kernel
    for (volatile unsigned long j = 0; j < usec * 100; j++);
    return 0;
}

// ===== SYSTEM STATE (Phase 8a) =====

long get_system_state(uint32_t category, void *buf, size_t buf_size) {
    return syscall3(SYS_GET_SYSTEM_STATE, category, (long)buf, buf_size);
}

// ===== GCP (GrahaOS Control Protocol) =====

// Execute GCP command (requires kernel/gcp.h types)
int gcp_execute(void *cmd) {
    return (int)syscall1(SYS_GCP_EXECUTE, (long)cmd);
}
