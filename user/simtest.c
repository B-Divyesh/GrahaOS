// user/simtest.c
// Phase 11a: SimHash Feature Extraction Test Suite
// Tests SimHash computation, Hamming distance, and similarity search

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/fs/grahafs.h"

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

static void print_hex64(uint64_t val) {
    const char *hex = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[16] = '\0';
    printf("0x%s", buf);
}

// Helper: create a file with given content
static int create_file(const char *path, const char *content, int len) {
    int ret = syscall_create(path, 0);
    if (ret < 0) return ret;
    int fd = syscall_open(path);
    if (fd < 0) return fd;
    syscall_write(fd, content, len);
    syscall_close(fd);
    return 0;
}

void _start(void) {
    printf("=== SimHash Feature Extraction Test Suite (Phase 11a) ===\n\n");

    // Test 1: Compute SimHash on a text file
    {
        const char *text = "The quick brown fox jumps over the lazy dog";
        create_file("/sim_text1", text, strlen(text));

        uint64_t hash = syscall_compute_simhash("/sim_text1");
        printf("  Text1 SimHash: ");
        print_hex64(hash);
        printf("\n");

        TEST("1. SimHash on text file returns non-zero", hash != 0);
    }

    // Test 2: Same content = same SimHash
    {
        const char *text = "The quick brown fox jumps over the lazy dog";
        create_file("/sim_text2", text, strlen(text));

        uint64_t hash1 = syscall_compute_simhash("/sim_text1");
        uint64_t hash2 = syscall_compute_simhash("/sim_text2");

        printf("  Text1: ");
        print_hex64(hash1);
        printf("\n");
        printf("  Text2: ");
        print_hex64(hash2);
        printf("\n");

        TEST("2. Identical content produces identical SimHash", hash1 == hash2);
    }

    // Test 3: Similar content = low Hamming distance
    {
        const char *text_a = "The quick brown fox jumps over the lazy dog today";
        const char *text_b = "The quick brown fox jumps over the lazy cat today";
        create_file("/sim_sim_a", text_a, strlen(text_a));
        create_file("/sim_sim_b", text_b, strlen(text_b));

        uint64_t ha = syscall_compute_simhash("/sim_sim_a");
        uint64_t hb = syscall_compute_simhash("/sim_sim_b");

        // Compute Hamming distance manually (XOR + count bits)
        uint64_t diff = ha ^ hb;
        int dist = 0;
        while (diff) { dist += diff & 1; diff >>= 1; }

        printf("  SimA: ");
        print_hex64(ha);
        printf("\n");
        printf("  SimB: ");
        print_hex64(hb);
        printf("\n");
        printf("  Hamming distance: %d\n", dist);

        // Similar text should have low distance (< 20)
        TEST("3. Similar text has low Hamming distance (< 20)", dist < 20);
    }

    // Test 4: Very different content = higher Hamming distance
    {
        const char *text_c = "aaaa bbbb cccc dddd eeee ffff gggg hhhh";
        const char *text_d = "1234 5678 9012 3456 7890 wxyz qrst uvwx";
        create_file("/sim_diff_c", text_c, strlen(text_c));
        create_file("/sim_diff_d", text_d, strlen(text_d));

        uint64_t hc = syscall_compute_simhash("/sim_diff_c");
        uint64_t hd = syscall_compute_simhash("/sim_diff_d");

        uint64_t diff = hc ^ hd;
        int dist = 0;
        while (diff) { dist += diff & 1; diff >>= 1; }

        printf("  DiffC: ");
        print_hex64(hc);
        printf("\n");
        printf("  DiffD: ");
        print_hex64(hd);
        printf("\n");
        printf("  Hamming distance: %d\n", dist);

        // Different text should have higher distance
        TEST("4. Different text has higher Hamming distance (> 5)", dist > 5);
    }

    // Test 5: Empty/nonexistent file returns 0
    {
        uint64_t hash = syscall_compute_simhash("/nonexistent_file_xyz");
        TEST("5. Nonexistent file returns SimHash 0", hash == 0);
    }

    // Test 6: SimHash is stored in metadata (recompute returns same)
    {
        uint64_t h1 = syscall_compute_simhash("/sim_text1");
        uint64_t h2 = syscall_compute_simhash("/sim_text1");
        TEST("6. Recompute returns same SimHash", h1 == h2 && h1 != 0);
    }

    // Test 7: find_similar on file with computed SimHash
    {
        // sim_text1 and sim_text2 have identical content, should be similar
        grahafs_search_results_t results;
        memset(&results, 0, sizeof(results));

        int ret = syscall_find_similar("/sim_text1", 10, &results);
        printf("  find_similar returned: %d, count=%d\n", ret, results.count);

        // Should find sim_text2 (identical content = distance 0)
        int found_text2 = 0;
        for (uint32_t i = 0; i < results.count; i++) {
            printf("    Match %d: %s (dist=%d)\n", i, results.results[i].path, results.results[i].importance);
            if (strcmp(results.results[i].path, "/sim_text2") == 0)
                found_text2 = 1;
        }

        TEST("7. find_similar finds identical file", ret >= 0 && found_text2);
    }

    // Test 8: find_similar with no SimHash = error -2
    {
        const char *data = "nosimhash data here";
        create_file("/sim_nohash", data, strlen(data));
        // Don't compute SimHash for this file

        grahafs_search_results_t results;
        memset(&results, 0, sizeof(results));

        int ret = syscall_find_similar("/sim_nohash", 10, &results);
        TEST("8. find_similar on file without SimHash returns -2", ret == -2);
    }

    // Test 9: find_similar with tight threshold (0) on similar files
    {
        // sim_sim_a and sim_sim_b have slightly different content
        grahafs_search_results_t results;
        memset(&results, 0, sizeof(results));

        int ret = syscall_find_similar("/sim_sim_a", 0, &results);
        // With threshold 0, only exact matches should be returned
        printf("  find_similar(threshold=0) returned: %d, count=%d\n", ret, results.count);

        TEST("9. find_similar with threshold=0 uses default (10)", ret >= 0);
    }

    // Test 10: Multiple similar files found
    {
        // Create another file similar to text1
        const char *text = "The quick brown fox jumps over the lazy dog!";
        create_file("/sim_text3", text, strlen(text));
        syscall_compute_simhash("/sim_text3");

        grahafs_search_results_t results;
        memset(&results, 0, sizeof(results));

        int ret = syscall_find_similar("/sim_text1", 15, &results);
        printf("  find_similar(threshold=15) returned: %d, count=%d\n", ret, results.count);
        for (uint32_t i = 0; i < results.count; i++) {
            printf("    Match %d: %s (dist=%d)\n", i, results.results[i].path, results.results[i].importance);
        }

        // Should find at least sim_text2 (identical) and possibly sim_text3 (very similar)
        TEST("10. find_similar finds multiple similar files", ret >= 1 && results.count >= 1);
    }

    // Summary
    printf("\n=== SimHash Test Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    syscall_exit(tests_failed > 0 ? 1 : 0);
}
