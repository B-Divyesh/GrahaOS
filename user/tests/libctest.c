// user/tests/libctest.c
// Phase 12: TAP 1.4 port of user/libctest.c (Phase 7c libc test suite).
// Preserves every original assertion and test name exactly.

#include "../libtap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// =====================================================
// Test group: sys_brk/sbrk (6 TAP_ASSERTs)
// =====================================================
static void test_sbrk(void) {
    printf("\n=== Testing sys_brk/sbrk ===\n");

    // Get initial break
    void *initial_brk = sbrk(0);
    TAP_ASSERT(initial_brk != (void *)-1, "sbrk(0) returns non-null");
    printf("Initial brk: %p\n", initial_brk);

    // Grow heap by 4KB
    void *new_brk = sbrk(4096);
    TAP_ASSERT(new_brk != (void *)-1, "sbrk(4096) succeeds");
    TAP_ASSERT(new_brk == initial_brk, "sbrk(4096) returns old brk");

    // Verify new break
    void *current_brk = sbrk(0);
    TAP_ASSERT((char *)current_brk == (char *)initial_brk + 4096, "Heap grew by 4KB");

    // Shrink heap
    void *shrink_brk = sbrk(-2048);
    TAP_ASSERT(shrink_brk != (void *)-1, "sbrk(-2048) succeeds");

    current_brk = sbrk(0);
    TAP_ASSERT((char *)current_brk == (char *)initial_brk + 2048, "Heap shrunk correctly");
}

// =====================================================
// Test group: malloc/free (10 TAP_ASSERTs)
// =====================================================
static void test_malloc_free(void) {
    printf("\n=== Testing malloc/free ===\n");

    // Test basic allocation
    void *ptr1 = malloc(100);
    TAP_ASSERT(ptr1 != NULL, "malloc(100) succeeds");

    // Write to allocated memory
    memset(ptr1, 0xAB, 100);
    TAP_ASSERT(((uint8_t *)ptr1)[50] == 0xAB, "Can write to allocated memory");

    // Test multiple allocations
    void *ptr2 = malloc(200);
    void *ptr3 = malloc(300);
    TAP_ASSERT(ptr2 != NULL && ptr3 != NULL, "Multiple mallocs succeed");
    TAP_ASSERT(((char *)ptr2 >= (char *)ptr1 + 100) &&
               ((char *)ptr3 >= (char *)ptr2 + 200),
               "Allocations don't overlap");

    // Free and reallocate
    free(ptr2);
    void *ptr4 = malloc(150);
    TAP_ASSERT(ptr4 != NULL, "Can allocate after free");

    // Test calloc
    void *ptr5 = calloc(50, 4);
    TAP_ASSERT(ptr5 != NULL, "calloc succeeds");
    TAP_ASSERT(((uint8_t *)ptr5)[0] == 0 && ((uint8_t *)ptr5)[199] == 0,
               "calloc zeroes memory");

    // Test realloc
    void *ptr6 = malloc(100);
    memset(ptr6, 0xCC, 100);
    void *ptr7 = realloc(ptr6, 200);
    TAP_ASSERT(ptr7 != NULL, "realloc succeeds");
    TAP_ASSERT(((uint8_t *)ptr7)[50] == 0xCC, "realloc preserves data");

    // Cleanup
    free(ptr1);
    free(ptr3);
    free(ptr4);
    free(ptr5);
    free(ptr7);

    // Test edge cases
    void *ptr_zero = malloc(0);
    TAP_ASSERT(ptr_zero == NULL, "malloc(0) returns NULL");

    free(NULL);
    TAP_ASSERT(1, "free(NULL) doesn't crash");
}

// =====================================================
// Test group: string functions (14 TAP_ASSERTs)
// =====================================================
static void test_string_functions(void) {
    printf("\n=== Testing string functions ===\n");

    char buf1[100];
    char buf2[100];

    // Test strlen
    TAP_ASSERT(strlen("hello") == 5, "strlen(\"hello\") == 5");
    TAP_ASSERT(strlen("") == 0, "strlen(\"\") == 0");

    // Test strcpy
    strcpy(buf1, "test");
    TAP_ASSERT(strcmp(buf1, "test") == 0, "strcpy works");

    // Test strncpy
    strncpy(buf2, "abcdefgh", 5);
    buf2[5] = '\0';
    TAP_ASSERT(strcmp(buf2, "abcde") == 0, "strncpy works");

    // Test strcmp
    TAP_ASSERT(strcmp("abc", "abc") == 0, "strcmp equal");
    TAP_ASSERT(strcmp("abc", "abd") < 0, "strcmp less");
    TAP_ASSERT(strcmp("abd", "abc") > 0, "strcmp greater");

    // Test strcat
    strcpy(buf1, "Hello");
    strcat(buf1, " World");
    TAP_ASSERT(strcmp(buf1, "Hello World") == 0, "strcat works");

    // Test strchr
    char *p = strchr("hello", 'e');
    TAP_ASSERT(p != NULL && *p == 'e', "strchr finds character");
    TAP_ASSERT(strchr("hello", 'z') == NULL, "strchr returns NULL for missing");

    // Test memcpy
    char src[] = "12345";
    char dst[10];
    memcpy(dst, src, 5);
    dst[5] = '\0';
    TAP_ASSERT(strcmp(dst, "12345") == 0, "memcpy works");

    // Test memset
    memset(buf1, 'X', 10);
    buf1[10] = '\0';
    TAP_ASSERT(buf1[0] == 'X' && buf1[9] == 'X', "memset works");

    // Test memcmp
    TAP_ASSERT(memcmp("abc", "abc", 3) == 0, "memcmp equal");
    TAP_ASSERT(memcmp("abc", "abd", 3) != 0, "memcmp different");
}

// =====================================================
// Test group: printf (3 TAP_ASSERTs)
// =====================================================
static void test_printf(void) {
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

    TAP_ASSERT(1, "printf doesn't crash");

    // Test sprintf
    char buf[100];
    sprintf(buf, "test %d %s", 42, "hello");
    TAP_ASSERT(strcmp(buf, "test 42 hello") == 0, "sprintf works");

    // Test snprintf
    snprintf(buf, 10, "This is a long string");
    TAP_ASSERT(strlen(buf) < 10, "snprintf truncates");
}

// =====================================================
// Test group: malloc stress (3 TAP_ASSERTs)
// =====================================================
static void test_malloc_stress(void) {
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
    TAP_ASSERT(integrity_ok, "Data integrity after many allocations");

    // Free every other block
    for (int i = 0; i < NUM_PTRS; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    // Reallocate in freed slots
    for (int i = 0; i < NUM_PTRS; i += 2) {
        ptrs[i] = malloc((i + 1) * 50);
    }
    TAP_ASSERT(ptrs[0] != NULL, "Can reallocate after fragmentation");

    // Free all
    for (int i = 0; i < NUM_PTRS; i++) {
        free(ptrs[i]);
    }
    TAP_ASSERT(1, "Mass free doesn't crash");
}

void _start(void) {
    printf("\n");
    printf("========================================\n");
    printf("   GrahaOS libc Test Suite (Phase 7c)  \n");
    printf("========================================\n");

    // Total assertions: 6 + 11 + 14 + 3 + 3 = 37
    tap_plan(37);

    test_sbrk();
    test_malloc_free();
    test_string_functions();
    test_printf();
    test_malloc_stress();

    tap_done();
    exit(0);
}
