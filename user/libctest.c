// user/libctest.c
// Phase 7c: Comprehensive test program for libc

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, condition) do { \
    if (condition) { \
        printf("[PASS] %s\n", name); \
        tests_passed++; \
    } else { \
        printf("[FAIL] %s\n", name); \
        tests_failed++; \
    } \
} while(0)

// Test sys_brk/sbrk
void test_sbrk(void) {
    printf("\n=== Testing sys_brk/sbrk ===\n");

    // Get initial break
    void *initial_brk = sbrk(0);
    TEST("sbrk(0) returns non-null", initial_brk != (void *)-1);
    printf("Initial brk: %p\n", initial_brk);

    // Grow heap by 4KB
    void *new_brk = sbrk(4096);
    TEST("sbrk(4096) succeeds", new_brk != (void *)-1);
    TEST("sbrk(4096) returns old brk", new_brk == initial_brk);

    // Verify new break
    void *current_brk = sbrk(0);
    TEST("Heap grew by 4KB", (char *)current_brk == (char *)initial_brk + 4096);

    // Shrink heap
    void *shrink_brk = sbrk(-2048);
    TEST("sbrk(-2048) succeeds", shrink_brk != (void *)-1);

    current_brk = sbrk(0);
    TEST("Heap shrunk correctly", (char *)current_brk == (char *)initial_brk + 2048);
}

// Test malloc/free
void test_malloc_free(void) {
    printf("\n=== Testing malloc/free ===\n");

    // Test basic allocation
    void *ptr1 = malloc(100);
    TEST("malloc(100) succeeds", ptr1 != NULL);

    // Write to allocated memory
    memset(ptr1, 0xAB, 100);
    TEST("Can write to allocated memory", ((uint8_t *)ptr1)[50] == 0xAB);

    // Test multiple allocations
    void *ptr2 = malloc(200);
    void *ptr3 = malloc(300);
    TEST("Multiple mallocs succeed", ptr2 != NULL && ptr3 != NULL);
    TEST("Allocations don't overlap",
         ((char *)ptr2 >= (char *)ptr1 + 100) &&
         ((char *)ptr3 >= (char *)ptr2 + 200));

    // Free and reallocate
    free(ptr2);
    void *ptr4 = malloc(150);
    TEST("Can allocate after free", ptr4 != NULL);

    // Test calloc
    void *ptr5 = calloc(50, 4);
    TEST("calloc succeeds", ptr5 != NULL);
    TEST("calloc zeroes memory", ((uint8_t *)ptr5)[0] == 0 && ((uint8_t *)ptr5)[199] == 0);

    // Test realloc
    void *ptr6 = malloc(100);
    memset(ptr6, 0xCC, 100);
    void *ptr7 = realloc(ptr6, 200);
    TEST("realloc succeeds", ptr7 != NULL);
    TEST("realloc preserves data", ((uint8_t *)ptr7)[50] == 0xCC);

    // Cleanup
    free(ptr1);
    free(ptr3);
    free(ptr4);
    free(ptr5);
    free(ptr7);

    // Test edge cases
    void *ptr_zero = malloc(0);
    TEST("malloc(0) returns NULL", ptr_zero == NULL);

    free(NULL);
    TEST("free(NULL) doesn't crash", 1);
}

// Test string functions
void test_string_functions(void) {
    printf("\n=== Testing string functions ===\n");

    char buf1[100];
    char buf2[100];

    // Test strlen
    TEST("strlen(\"hello\") == 5", strlen("hello") == 5);
    TEST("strlen(\"\") == 0", strlen("") == 0);

    // Test strcpy
    strcpy(buf1, "test");
    TEST("strcpy works", strcmp(buf1, "test") == 0);

    // Test strncpy
    strncpy(buf2, "abcdefgh", 5);
    buf2[5] = '\0';
    TEST("strncpy works", strcmp(buf2, "abcde") == 0);

    // Test strcmp
    TEST("strcmp equal", strcmp("abc", "abc") == 0);
    TEST("strcmp less", strcmp("abc", "abd") < 0);
    TEST("strcmp greater", strcmp("abd", "abc") > 0);

    // Test strcat
    strcpy(buf1, "Hello");
    strcat(buf1, " World");
    TEST("strcat works", strcmp(buf1, "Hello World") == 0);

    // Test strchr
    char *p = strchr("hello", 'e');
    TEST("strchr finds character", p != NULL && *p == 'e');
    TEST("strchr returns NULL for missing", strchr("hello", 'z') == NULL);

    // Test memcpy
    char src[] = "12345";
    char dst[10];
    memcpy(dst, src, 5);
    dst[5] = '\0';
    TEST("memcpy works", strcmp(dst, "12345") == 0);

    // Test memset
    memset(buf1, 'X', 10);
    buf1[10] = '\0';
    TEST("memset works", buf1[0] == 'X' && buf1[9] == 'X');

    // Test memcmp
    TEST("memcmp equal", memcmp("abc", "abc", 3) == 0);
    TEST("memcmp different", memcmp("abc", "abd", 3) != 0);
}

// Test printf format specifiers
void test_printf(void) {
    printf("\n=== Testing printf ===\n");

    // Integer formats
    printf("%%d: %d\n", 42);
    printf("%%i: %i\n", -42);
    printf("%%u: %u\n", 4294967295U);
    printf("%%x: %x\n", 0xDEADBEEF);
    printf("%%X: %X\n", 0xDEADBEEF);
    printf("%%o: %o\n", 0755);

    // Pointer format
    printf("%%p: %p\n", (void *)0x12345678);

    // String and char formats
    printf("%%s: %s\n", "Hello World");
    printf("%%c: %c\n", 'A');

    // Width and precision
    printf("%%10d: %10d\n", 42);
    printf("%%-10d: %-10d\n", 42);
    printf("%%05d: %05d\n", 42);
    printf("%%.5s: %.5s\n", "Hello World");

    // Long formats
    printf("%%ld: %ld\n", 9223372036854775807L);
    printf("%%lld: %lld\n", 9223372036854775807LL);
    printf("%%lx: %lx\n", 0x123456789ABCDEFULL);

    // Special cases
    printf("%%%%: %%\n");

    TEST("printf doesn't crash", 1);

    // Test sprintf
    char buf[100];
    sprintf(buf, "test %d %s", 42, "hello");
    TEST("sprintf works", strcmp(buf, "test 42 hello") == 0);

    // Test snprintf
    snprintf(buf, 10, "This is a long string");
    TEST("snprintf truncates", strlen(buf) < 10);
}

// Stress test malloc/free
void test_malloc_stress(void) {
    printf("\n=== Stress testing malloc/free ===\n");

    #define NUM_PTRS 20
    void *ptrs[NUM_PTRS];

    // Allocate many blocks
    for (int i = 0; i < NUM_PTRS; i++) {
        ptrs[i] = malloc((i + 1) * 50);
        if (!ptrs[i]) {
            printf("malloc failed at iteration %d\n", i);
            break;
        }
        memset(ptrs[i], i, (i + 1) * 50);
    }

    // Verify data integrity
    int integrity_ok = 1;
    for (int i = 0; i < NUM_PTRS; i++) {
        if (ptrs[i] && ((uint8_t *)ptrs[i])[0] != (uint8_t)i) {
            integrity_ok = 0;
            break;
        }
    }
    TEST("Data integrity after many allocations", integrity_ok);

    // Free every other block
    for (int i = 0; i < NUM_PTRS; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    // Reallocate in freed slots
    for (int i = 0; i < NUM_PTRS; i += 2) {
        ptrs[i] = malloc((i + 1) * 50);
    }
    TEST("Can reallocate after fragmentation", ptrs[0] != NULL);

    // Free all
    for (int i = 0; i < NUM_PTRS; i++) {
        free(ptrs[i]);
    }
    TEST("Mass free doesn't crash", 1);
}

// Main entry point
void _start(void) {
    printf("\n");
    printf("========================================\n");
    printf("   GrahaOS libc Test Suite (Phase 7c)  \n");
    printf("========================================\n");

    test_sbrk();
    test_malloc_free();
    test_string_functions();
    test_printf();
    test_malloc_stress();

    printf("\n========================================\n");
    printf("   Test Results                         \n");
    printf("========================================\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("\n*** ALL TESTS PASSED! ***\n");
    } else {
        printf("\n*** SOME TESTS FAILED ***\n");
    }

    printf("\nTest complete. Exiting.\n");
    exit(0);
}
