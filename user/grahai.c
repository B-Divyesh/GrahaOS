// user/grahai.c - Phase 6a Test Program
#include <stdint.h>
#include <stddef.h> // For size_t
#include "../kernel/fs/vfs.h" // For ssize_t

// Syscall numbers from kernel's syscall.h
#define SYS_PUTC  1001
#define SYS_OPEN  1002
#define SYS_READ  1003
#define SYS_CLOSE 1004

// Syscall wrapper for putc
void syscall_putc(char c) {
    asm volatile(
        "syscall"
        :
        : "a"(SYS_PUTC), "D"(c)
        : "rcx", "r11", "memory"
    );
}

// Syscall wrapper for open
int syscall_open(const char *pathname) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_OPEN), "D"(pathname)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

// Syscall wrapper for read
ssize_t syscall_read(int fd, void *buf, size_t count) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_READ), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return (ssize_t)ret;
}

// Syscall wrapper for close
int syscall_close(int fd) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_CLOSE), "D"(fd)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

// Helper to print a null-terminated string
void print(const char *str) {
    while (*str) {
        syscall_putc(*str++);
    }
}

void _start(void) {
    print("grahai: Starting filesystem test...\n");

    const char *filepath = "etc/motd.txt";
    int fd = syscall_open(filepath);

    if (fd < 0) {
        print("grahai: FAILED to open /etc/motd.txt\n");
    } else {
        print("grahai: Successfully opened /etc/motd.txt (fd=");
        // Simple int to char for debugging fd
        if (fd >= 0 && fd <= 9) {
            syscall_putc('0' + fd);
        } else {
            syscall_putc('?');
        }
        print(")\n");
        print("---\n");

        char buffer[128];
        ssize_t bytes_read;

        while ((bytes_read = syscall_read(fd, buffer, sizeof(buffer) - 1)) > 0) {
            // Null-terminate the buffer to print it safely
            buffer[bytes_read] = '\0';
            print(buffer);
        }
        
        print("\n---\n");
        print("grahai: Finished reading file.\n");

        syscall_close(fd);
    }

    print("grahai: Test complete. Halting.\n");

    // Infinite loop to stop the process
    while (1) {
        //asm volatile("hlt");
    }
}