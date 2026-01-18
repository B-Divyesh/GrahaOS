// user/sbrk_test.c
// Minimal test to isolate sbrk/malloc issue

#include "syscalls.h"

void putc(char c) {
    syscall_putc(c);
}

void print(const char *s) {
    while (*s) putc(*s++);
}

void _start(void) {
    print("Starting sbrk test...\n");

    // Test 1: Get initial brk
    print("Test 1: Getting initial brk...\n");
    long brk1;
    asm volatile("syscall" : "=a"(brk1) : "a"(1016), "D"(0) : "rcx", "r11", "memory");
    print("Initial brk: ");
    // Print hex
    for (int i = 60; i >= 0; i -= 4) {
        char hex = "0123456789ABCDEF"[(brk1 >> i) & 0xF];
        putc(hex);
    }
    print("\n");

    // Test 2: Grow heap by 1 page (4096 bytes)
    print("Test 2: Growing heap by 4096 bytes...\n");
    long new_addr = brk1 + 4096;
    long brk2;
    asm volatile("syscall" : "=a"(brk2) : "a"(1016), "D"(new_addr) : "rcx", "r11", "memory");

    if (brk2 == (long)-1) {
        print("ERROR: sbrk failed!\n");
        syscall_exit(1);
    }

    print("New brk: ");
    for (int i = 60; i >= 0; i -= 4) {
        char hex = "0123456789ABCDEF"[(brk2 >> i) & 0xF];
        putc(hex);
    }
    print("\n");

    // Test 3: Try to write to the new memory
    print("Test 3: Writing to new heap memory...\n");
    char *test_ptr = (char *)brk1;

    print("About to write byte at: ");
    for (int i = 60; i >= 0; i -= 4) {
        char hex = "0123456789ABCDEF"[((long)test_ptr >> i) & 0xF];
        putc(hex);
    }
    print("\n");

    // This is where it might crash
    test_ptr[0] = 'A';
    test_ptr[1] = 'B';
    test_ptr[2] = 'C';
    test_ptr[3] = '\0';

    print("Successfully wrote to heap!\n");
    print("Data: ");
    print(test_ptr);
    print("\n");

    print("All tests passed!\n");
    syscall_exit(0);
}
