// user/grahai.c - ORIGINAL WORKING VERSION
#include <stdint.h>

#define SYS_PUTC 1001

long syscall_putc(char c) {
    long ret;
    register long rdi_reg asm("rdi") = (long)c;
    register long rax_reg asm("rax") = SYS_PUTC;

    asm volatile(
        "syscall"
        : "=a" (ret)
        : "r" (rdi_reg), "r" (rax_reg)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void _start(void) {
    syscall_putc('[');
    syscall_putc('O');
    syscall_putc('K');
    syscall_putc(']');
    syscall_putc(' ');
    syscall_putc('H');
    syscall_putc('e');
    syscall_putc('l');
    syscall_putc('l');
    syscall_putc('o');
    syscall_putc(' ');
    syscall_putc('w');
    syscall_putc('o');
    syscall_putc('r');
    syscall_putc('l');
    syscall_putc('d');
    syscall_putc('!');
    syscall_putc('\n');

    while (1) {
        asm volatile("hlt");
    }
}