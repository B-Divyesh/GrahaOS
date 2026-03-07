// kernel/fs/cluster.h
// Phase 11b: Sequential Leader Clustering for SimHash-based file grouping
#pragma once

#include <stdint.h>
#include <stddef.h>

#define CLUSTER_MAX          32
#define CLUSTER_MAX_MEMBERS  64
#define CLUSTER_THRESHOLD    10   // Default Hamming distance threshold (tau)
#define CLUSTER_ID_NONE      0    // 0 = unassigned

// In-memory entry for a cluster member
typedef struct {
    uint32_t inode_num;
    char     name[28];
    uint64_t simhash;
} cluster_member_entry_t;  // 40 bytes

// In-memory cluster structure
typedef struct {
    uint32_t id;                  // 1-based cluster ID, 0 = empty slot
    uint64_t leader_hash;        // Leader's SimHash (centroid)
    uint32_t leader_inode;
    char     leader_name[28];
    uint32_t member_count;
    cluster_member_entry_t members[CLUSTER_MAX_MEMBERS];
} cluster_t;

// User-space result: info about one cluster
typedef struct {
    uint32_t id;
    char     name[28];           // Leader name
    uint32_t member_count;
    uint64_t centroid;           // Leader SimHash
} cluster_info_t;

// User-space result: list of all clusters
typedef struct {
    uint32_t count;
    uint32_t _pad;
    cluster_info_t clusters[CLUSTER_MAX];
} cluster_list_t;

// User-space result: info about one cluster member
typedef struct {
    uint32_t inode_num;
    char     name[28];
    int      distance;           // Hamming distance from leader
} cluster_member_info_t;

// User-space result: members of a specific cluster
typedef struct {
    uint32_t cluster_id;
    char     leader_name[28];
    uint32_t count;
    cluster_member_info_t members[CLUSTER_MAX_MEMBERS];
} cluster_members_t;

// Initialize cluster table (zero everything)
void cluster_init(void);

// Sequential Leader assignment: assign file to nearest cluster or create new one
// Returns cluster_id (1-based) on success, 0 if max clusters reached
uint32_t cluster_assign(uint32_t inode_num, uint64_t simhash, const char *filename);

// Rebuild: add a file with known cluster_id (for mount-time reconstruction)
void cluster_rebuild_add(uint32_t inode_num, uint32_t cluster_id,
                         uint64_t simhash, const char *filename);

// Finalize rebuild: set next_id, count active clusters
void cluster_rebuild_finalize(void);

// Get list of all active clusters
// Returns number of active clusters
int cluster_get_list(cluster_list_t *out);

// Get members of a specific cluster
// Returns member count on success, -1 if cluster not found
int cluster_get_members(uint32_t cluster_id, cluster_members_t *out);

// Get number of active clusters
int cluster_get_count(void);
