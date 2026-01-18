// user/printf_test.c
// Minimal test to debug printf %p issue

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void _start(void) {
    printf("printf_test starting...\n");

    // Test 1: Simple string
    printf("Test 1: Hello World\n");

    // Test 2: Integer
    printf("Test 2: %d\n", 42);

    // Test 3: Hex (small)
    printf("Test 3: %x\n", 0xDEAD);

    // Test 4: Hex (large)
    printf("Test 4: %lx\n", 0xDEADBEEFUL);

    // Test 5: Long long hex
    printf("Test 5: %llx\n", 0x123456789ABCDEFULL);

    // Test 6: Hex with # prefix (same as %p but for integer)
    printf("Test 6: %#x (should be 0x1000)\n", 0x1000);

    // Test 7: Long hex with # prefix
    printf("Test 7: %#lx (should be 0xdeadbeef)\n", 0xdeadbeefUL);

    // Test 8: Pointer (small)
    void *ptr1 = (void *)0x1000;
    printf("Test 8: %p\n", ptr1);

    // Test 9: Pointer (large - 4GB)
    void *ptr2 = (void *)0x100000000ULL;
    printf("Test 9: %p\n", ptr2);

    // Test 9: After pointer
    printf("Test 9: Done!\n");

    // Exit
    exit(0);
}
