// user/grahai.c
#include <stdint.h>

// Syscall numbers
#define SYS_PUTC 1001

// Simple syscall wrapper.
// With the kernel properly saving/restoring registers, we only need to
// declare the registers clobbered by the `syscall` instruction itself
// (rcx, r11) and memory.
static inline long syscall(long n, long arg1) {
    long ret;
    
    asm volatile (
        "syscall"
        : "=a" (ret)    // Output: return value in RAX.
        : "a" (n),      // Input 0: syscall number 'n' in RAX.
          "D" (arg1)    // Input 1: argument 'arg1' in RDI.
        : "rcx", "r11", "memory" // Clobber list.
    );
    return ret;
}

// Simple print function using the new syscall.
// The 'volatile' keyword is still good practice to ensure the compiler
// doesn't make assumptions about the pointer 'p' across the syscall.
void print(const char *str) {
    for (volatile const char *p = str; *p; p++) {
        syscall(SYS_PUTC, (long)*p);
    }
}

// Entry point for the user program
void _start(void) {
    // Now we know the user program executes, let's make syscalls


    syscall(SYS_PUTC, (long)'H');
    syscall(SYS_PUTC, (long)'e');
    syscall(SYS_PUTC, (long)'l');
    syscall(SYS_PUTC, (long)'l');
    syscall(SYS_PUTC, (long)'o');
    syscall(SYS_PUTC, (long)' ');
    syscall(SYS_PUTC, (long)'w');
    syscall(SYS_PUTC, (long)'o');
    syscall(SYS_PUTC, (long)'r');
    syscall(SYS_PUTC, (long)'l');
    syscall(SYS_PUTC, (long)'d');
    syscall(SYS_PUTC, (long)'!');
    
    // Loop forever
    for (;;) {
        asm("hlt");
    }
}