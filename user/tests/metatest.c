// user/tests/metatest.c
// Phase 12: TAP 1.4 port of user/metatest.c (Phase 8c AI metadata tests).

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/fs/grahafs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Helper to zero memory
static void zero_mem(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

// =====================================================
// Group 1: Basic Metadata (5 tests)
// =====================================================
static void test_basic_metadata(void) {
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
        TAP_ASSERT(ret == 0, "1. Set tags 'hello,world'");
    }

    // Test 2: Get tags back and verify
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));

        ret = syscall_get_ai_metadata("/metatest1", &meta);
        TAP_ASSERT(ret == 0 && strcmp(meta.tags, "hello,world") == 0,
                   "2. Get tags matches 'hello,world'");
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
        TAP_ASSERT(ret == 0 && got.importance == 75,
                   "3. Importance=75 round-trips");
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
        TAP_ASSERT(ret == 0 && got.importance == 0,
                   "4. Importance=0 is valid");
    }

    // Test 5: Get metadata on file with no AI metadata set → defaults
    {
        syscall_create("/metatest_empty", 0644);

        grahafs_ai_metadata_t got;
        zero_mem(&got, sizeof(got));
        ret = syscall_get_ai_metadata("/metatest_empty", &got);
        TAP_ASSERT(ret == 0 && got.importance == 0 && got.tags[0] == '\0',
                   "5. Default metadata (importance=0, empty tags)");
    }
}

// =====================================================
// Group 2: Extended Metadata (3 tests)
// =====================================================
static void test_extended_metadata(void) {
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
        TAP_ASSERT(ret == 0 && strcmp(got.summary, "This is a test summary for AI metadata") == 0,
                   "6. Summary stored and retrieved");
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
        TAP_ASSERT(ret == 0 && strcmp(got.tags, "c,d") == 0,
                   "7. Overwrite tags a,b -> c,d");
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
        TAP_ASSERT(ret == 0 && got.importance == 100,
                   "8. Importance=100 is valid");
    }
}

// =====================================================
// Group 3: Search (4 tests)
// =====================================================
static void test_search(void) {
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
        TAP_ASSERT(ret >= 0 && results.count >= 1,
                   "9. Search 'findme' finds tagged file");
    }

    // Test 10: Untagged file not in search results for "findme"
    {
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
        TAP_ASSERT(found_empty == 0, "10. Untagged file not in search results");
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
        TAP_ASSERT(ret >= 0 && results.count >= 2,
                   "11. Multiple files with 'shared' tag found");
    }

    // Test 12: Search nonexistent tag → count=0
    {
        grahafs_search_results_t results;
        zero_mem(&results, sizeof(results));

        ret = syscall_search_by_tag("zzz_nonexistent_zzz", &results, 16);
        TAP_ASSERT(ret >= 0 && results.count == 0,
                   "12. Search nonexistent tag returns 0");
    }
}

// =====================================================
// Group 4: Error Cases (2 tests)
// =====================================================
static void test_errors(void) {
    printf("\n=== Group 4: Error Cases ===\n");
    int ret;

    // Test 13: Set metadata on nonexistent file → error
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_TAGS;
        strcpy(meta.tags, "test");

        ret = syscall_set_ai_metadata("/nonexistent_file_xyz", &meta);
        TAP_ASSERT(ret < 0, "13. Set metadata on nonexistent file fails");
    }

    // Test 14: Set metadata on directory "/" → succeeds
    {
        grahafs_ai_metadata_t meta;
        zero_mem(&meta, sizeof(meta));
        meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
        meta.importance = 99;

        ret = syscall_set_ai_metadata("/", &meta);
        TAP_ASSERT(ret == 0, "14. Set metadata on directory '/' succeeds");
    }
}

void _start(void) {
    printf("=== AI Metadata Test Suite (Phase 8c) ===\n");
    printf("Testing GrahaFS AI metadata operations...\n");
    printf("PID: %d\n", syscall_getpid());

    tap_plan(14);

    test_basic_metadata();
    test_extended_metadata();
    test_search();
    test_errors();

    tap_done();
    exit(0);
}
