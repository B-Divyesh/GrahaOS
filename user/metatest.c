// user/metatest.c
// Phase 8c: AI Metadata test suite
// 14 automated tests covering basic metadata, extended metadata, search, error cases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/fs/grahafs.h"

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

// Helper to zero memory
static void zero_mem(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

// =====================================================
// Group 1: Basic Metadata (5 tests)
// =====================================================
void test_basic_metadata(void) {
    printf("\n=== Group 1: Basic Metadata ===\n");

    // Create test file first
    int ret = syscall_create("/metatest1", 0644);
    // OK if it already exists

    // Test 1: Set tags, then get and verify
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "hello,world");

        ret = syscall_set_ai_metadata("/metatest1", &meta);
        TEST("1. Set tags 'hello,world'", ret == 0);
    }

    // Test 2: Get tags back and verify
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));

        ret = syscall_get_ai_metadata("/metatest1", &meta);
        TEST("2. Get tags matches 'hello,world'",
             ret == 0 && strcmp(meta.tags, "hello,world") == 0);
    }

    // Test 3: Set importance=75, get it back
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
        meta.importance = 75;

        ret = syscall_set_ai_metadata("/metatest1", &meta);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        syscall_get_ai_metadata("/metatest1", &got);
        TEST("3. Importance=75 round-trips", ret == 0 && got.importance == 75);
    }

    // Test 4: Importance=0 is valid
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
        meta.importance = 0;

        ret = syscall_set_ai_metadata("/metatest1", &meta);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        syscall_get_ai_metadata("/metatest1", &got);
        TEST("4. Importance=0 is valid", ret == 0 && got.importance == 0);
    }

    // Test 5: Get metadata on file with no AI metadata set → defaults
    {
        syscall_create("/metatest_empty", 0644);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        ret = syscall_get_ai_metadata("/metatest_empty", &got);
        TEST("5. Default metadata (importance=0, empty tags)",
             ret == 0 && got.importance == 0 && got.tags[0] == '\0');
    }
}

// =====================================================
// Group 2: Extended Metadata (3 tests)
// =====================================================
void test_extended_metadata(void) {
    printf("\n=== Group 2: Extended Metadata ===\n");
    int ret;

    // Test 6: Set summary (triggers extended block)
    {
        syscall_create("/metatest2", 0644);

        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_SUMMARY;
        strcpy(meta.summary, "This is a test summary for AI metadata");

        ret = syscall_set_ai_metadata("/metatest2", &meta);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        syscall_get_ai_metadata("/metatest2", &got);
        TEST("6. Summary stored and retrieved",
             ret == 0 && strcmp(got.summary, "This is a test summary for AI metadata") == 0);
    }

    // Test 7: Overwrite tags
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "a,b");
        syscall_set_ai_metadata("/metatest1", &meta);

        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "c,d");
        ret = syscall_set_ai_metadata("/metatest1", &meta);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        syscall_get_ai_metadata("/metatest1", &got);
        TEST("7. Overwrite tags a,b -> c,d", ret == 0 && strcmp(got.tags, "c,d") == 0);
    }

    // Test 8: Set importance=100 is valid
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
        meta.importance = 100;

        ret = syscall_set_ai_metadata("/metatest1", &meta);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        syscall_get_ai_metadata("/metatest1", &got);
        TEST("8. Importance=100 is valid", ret == 0 && got.importance == 100);
    }
}

// =====================================================
// Group 3: Search (4 tests)
// =====================================================
void test_search(void) {
    printf("\n=== Group 3: Search ===\n");
    int ret;

    // Setup: tag metatest1 with "findme"
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "findme,unique");
        syscall_set_ai_metadata("/metatest1", &meta);
    }

    // Test 9: Search "findme" → found
    {
        grahafs_search_results_t results;
        zero_mem(&results, sizeof(results));

        ret = syscall_search_by_tag("findme", &results, 16);
        TEST("9. Search 'findme' finds tagged file", ret >= 0 && results.count >= 1);
    }

    // Test 10: Untagged file not in search results for "findme"
    {
        // metatest_empty has no tags
        grahafs_search_results_t results;
        zero_mem(&results, sizeof(results));

        ret = syscall_search_by_tag("findme", &results, 16);

        // Verify metatest_empty is NOT in results
        int found_empty = 0;
        for (uint32_t i = 0; i < results.count; i++) {
            if (strcmp(results.results[i].path, "/metatest_empty") == 0) {
                found_empty = 1;
            }
        }
        TEST("10. Untagged file not in search results", found_empty == 0);
    }

    // Test 11: Tag multiple files with "shared", search → count matches
    {
        syscall_create("/metatest3", 0644);

        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;

        strcpy(meta.tags, "shared,alpha");
        syscall_set_ai_metadata("/metatest2", &meta);

        strcpy(meta.tags, "shared,beta");
        syscall_set_ai_metadata("/metatest3", &meta);

        grahafs_search_results_t results;
        zero_mem(&results, sizeof(results));

        ret = syscall_search_by_tag("shared", &results, 16);
        TEST("11. Multiple files with 'shared' tag found", ret >= 0 && results.count >= 2);
    }

    // Test 12: Search nonexistent tag → count=0
    {
        grahafs_search_results_t results;
        zero_mem(&results, sizeof(results));

        ret = syscall_search_by_tag("zzz_nonexistent_zzz", &results, 16);
        TEST("12. Search nonexistent tag returns 0", ret >= 0 && results.count == 0);
    }
}

// =====================================================
// Group 4: Error Cases (2 tests)
// =====================================================
void test_errors(void) {
    printf("\n=== Group 4: Error Cases ===\n");
    int ret;

    // Test 13: Set metadata on nonexistent file → error
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "test");

        ret = syscall_set_ai_metadata("/nonexistent_file_xyz", &meta);
        TEST("13. Set metadata on nonexistent file fails", ret < 0);
    }

    // Test 14: Set metadata on directory "/" → succeeds
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
        meta.importance = 99;

        ret = syscall_set_ai_metadata("/", &meta);
        TEST("14. Set metadata on directory '/' succeeds", ret == 0);
    }
}

// =====================================================
// Main
// =====================================================
void _start(void) {
    printf("=== AI Metadata Test Suite (Phase 8c) ===\n");
    printf("Testing GrahaFS AI metadata operations...\n");
    printf("PID: %d\n", syscall_getpid());

    test_basic_metadata();
    test_extended_metadata();
    test_search();
    test_errors();

    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d/14\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("\nAll tests PASSED!\n");
    } else {
        printf("\nSome tests FAILED.\n");
    }

    syscall_exit(tests_failed);
}
