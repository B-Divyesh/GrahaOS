// user/clustertest.c
// Phase 11b: Sequential Leader Clustering Test Suite

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/fs/grahafs.h"
#include "../kernel/fs/cluster.h"

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
    printf("=== Cluster Test Suite (Phase 11b) ===\n\n");

    // Test 1: Cluster list initially (should be >= 0, possibly with existing clusters)
    {
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        int ret = syscall_cluster_list(&list);
        printf("  Initial cluster count: %d\n", ret);
        TEST("1. cluster_list returns >= 0", ret >= 0);
    }

    int initial_count;
    {
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);
        initial_count = (int)list.count;
    }

    // Test 2: SimHash triggers auto-clustering
    {
        const char *text = "clustering test file alpha data";
        create_file("/cl_alpha", text, strlen(text));
        uint64_t hash = syscall_compute_simhash("/cl_alpha");
        printf("  cl_alpha hash: 0x%lx\n", hash);

        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        int ret = syscall_cluster_list(&list);
        printf("  Cluster count after first simhash: %d\n", ret);

        TEST("2. SimHash triggers auto-clustering (new cluster created)",
             ret >= 0 && (int)list.count > initial_count);
    }

    // Test 3: Identical content -> same cluster
    {
        const char *text = "clustering test file alpha data";
        create_file("/cl_alpha2", text, strlen(text));
        syscall_compute_simhash("/cl_alpha2");

        // Both should be in the same cluster (distance 0 = identical)
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);

        // Find the cluster containing cl_alpha
        int found_together = 0;
        for (uint32_t i = 0; i < list.count; i++) {
            cluster_members_t members;
            memset(&members, 0, sizeof(members));
            int mc = syscall_cluster_members(list.clusters[i].id, &members);
            if (mc < 0) continue;

            int has_alpha = 0, has_alpha2 = 0;
            for (uint32_t m = 0; m < members.count; m++) {
                if (strcmp(members.members[m].name, "cl_alpha") == 0) has_alpha = 1;
                if (strcmp(members.members[m].name, "cl_alpha2") == 0) has_alpha2 = 1;
            }
            if (has_alpha && has_alpha2) {
                found_together = 1;
                printf("  cl_alpha and cl_alpha2 in cluster %d\n", list.clusters[i].id);
            }
        }

        TEST("3. Identical content -> same cluster", found_together);
    }

    // Test 4: Similar content -> same cluster (Hamming distance <= 10)
    {
        const char *text_a = "clustering test file alpha data here today";
        const char *text_b = "clustering test file beta data here today";
        create_file("/cl_sima", text_a, strlen(text_a));
        create_file("/cl_simb", text_b, strlen(text_b));
        syscall_compute_simhash("/cl_sima");
        syscall_compute_simhash("/cl_simb");

        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);

        int same_cluster = 0;
        for (uint32_t i = 0; i < list.count; i++) {
            cluster_members_t members;
            memset(&members, 0, sizeof(members));
            syscall_cluster_members(list.clusters[i].id, &members);
            int has_a = 0, has_b = 0;
            for (uint32_t m = 0; m < members.count; m++) {
                if (strcmp(members.members[m].name, "cl_sima") == 0) has_a = 1;
                if (strcmp(members.members[m].name, "cl_simb") == 0) has_b = 1;
            }
            if (has_a && has_b) same_cluster = 1;
        }

        TEST("4. Similar content -> same cluster", same_cluster);
    }

    // Test 5: Very different content -> different clusters
    {
        const char *text_x = "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj";
        const char *text_y = "1234 5678 9012 3456 7890 wxyz qrst uvwx lmno pqrs";
        create_file("/cl_diffx", text_x, strlen(text_x));
        create_file("/cl_diffy", text_y, strlen(text_y));
        syscall_compute_simhash("/cl_diffx");
        syscall_compute_simhash("/cl_diffy");

        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);

        int same_cluster = 0;
        for (uint32_t i = 0; i < list.count; i++) {
            cluster_members_t members;
            memset(&members, 0, sizeof(members));
            syscall_cluster_members(list.clusters[i].id, &members);
            int has_x = 0, has_y = 0;
            for (uint32_t m = 0; m < members.count; m++) {
                if (strcmp(members.members[m].name, "cl_diffx") == 0) has_x = 1;
                if (strcmp(members.members[m].name, "cl_diffy") == 0) has_y = 1;
            }
            if (has_x && has_y) same_cluster = 1;
        }

        TEST("5. Very different content -> different clusters", !same_cluster);
    }

    // Test 6: cluster_list returns correct count
    {
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        int ret = syscall_cluster_list(&list);
        printf("  Total clusters now: %d (list.count=%d)\n", ret, list.count);
        TEST("6. cluster_list count matches return value", ret >= 0 && (uint32_t)ret == list.count);
    }

    // Test 7: cluster_members returns correct files for a known cluster
    {
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);

        int found_valid = 0;
        if (list.count > 0) {
            cluster_members_t members;
            memset(&members, 0, sizeof(members));
            int mc = syscall_cluster_members(list.clusters[0].id, &members);
            printf("  Cluster %d has %d members\n", list.clusters[0].id, mc);
            if (mc > 0 && members.count > 0 && members.members[0].name[0] != '\0') {
                found_valid = 1;
            }
            for (uint32_t m = 0; m < members.count; m++) {
                printf("    %s (dist=%d)\n", members.members[m].name, members.members[m].distance);
            }
        }
        TEST("7. cluster_members returns valid data", found_valid);
    }

    // Test 8: Leader is the first file simhashed in its cluster
    {
        cluster_list_t list;
        memset(&list, 0, sizeof(list));
        syscall_cluster_list(&list);

        int leader_is_first = 0;
        for (uint32_t i = 0; i < list.count; i++) {
            cluster_members_t members;
            memset(&members, 0, sizeof(members));
            int mc = syscall_cluster_members(list.clusters[i].id, &members);
            if (mc > 0 && members.count > 0) {
                // Leader should have distance 0
                if (members.members[0].distance == 0) {
                    leader_is_first = 1;
                    break;
                }
            }
        }
        TEST("8. Leader has distance 0 from centroid", leader_is_first);
    }

    // Test 9: Recompute simhash -> file stays clustered (not duplicated)
    {
        // cl_alpha was already simhashed and clustered
        cluster_list_t list_before;
        memset(&list_before, 0, sizeof(list_before));
        syscall_cluster_list(&list_before);
        int count_before = (int)list_before.count;

        // Recompute
        syscall_compute_simhash("/cl_alpha");

        cluster_list_t list_after;
        memset(&list_after, 0, sizeof(list_after));
        syscall_cluster_list(&list_after);
        int count_after = (int)list_after.count;

        printf("  Clusters before recompute: %d, after: %d\n", count_before, count_after);
        TEST("9. Recompute simhash doesn't create duplicate cluster", count_after == count_before);
    }

    // Test 10: Non-existent cluster returns error
    {
        cluster_members_t members;
        memset(&members, 0, sizeof(members));
        int ret = syscall_cluster_members(9999, &members);
        TEST("10. Non-existent cluster ID returns -1", ret == -1);
    }

    // Summary
    printf("\n=== Cluster Test Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    syscall_exit(tests_failed > 0 ? 1 : 0);
}
