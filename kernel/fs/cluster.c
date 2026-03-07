// kernel/fs/cluster.c
// Phase 11b: Sequential Leader Clustering engine
// Pure algorithm + in-memory table. No disk I/O here — grahafs.c handles persistence.

#include "cluster.h"
#include "simhash.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/drivers/serial/serial.h"

// String helpers (kernel freestanding — no libc)
static void *cl_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static void cl_strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Global cluster table
static cluster_t g_clusters[CLUSTER_MAX];
static int       g_cluster_count = 0;
static uint32_t  g_next_cluster_id = 1;
static spinlock_t cluster_lock = SPINLOCK_INITIALIZER("cluster");

void cluster_init(void) {
    spinlock_acquire(&cluster_lock);
    cl_memset(g_clusters, 0, sizeof(g_clusters));
    g_cluster_count = 0;
    g_next_cluster_id = 1;
    spinlock_release(&cluster_lock);
}

// Sequential Leader algorithm:
// 1. Scan all active clusters, compute Hamming distance to each leader
// 2. If nearest <= threshold and cluster not full -> add member, return cluster_id
// 3. Else create new cluster with this file as leader
// 4. Returns 0 if max clusters reached
uint32_t cluster_assign(uint32_t inode_num, uint64_t simhash, const char *filename) {
    spinlock_acquire(&cluster_lock);

    int best_idx = -1;
    int best_dist = CLUSTER_THRESHOLD + 1;  // Start above threshold

    // Find nearest cluster leader within threshold
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == 0) continue;  // Empty slot

        int dist = simhash_hamming_distance(simhash, g_clusters[i].leader_hash);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }

    // Check if this file is already a member of any cluster (avoid duplicates)
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == 0) continue;
        for (uint32_t m = 0; m < g_clusters[i].member_count; m++) {
            if (g_clusters[i].members[m].inode_num == inode_num) {
                // Already in a cluster — update simhash and return existing cluster
                g_clusters[i].members[m].simhash = simhash;
                uint32_t existing_id = g_clusters[i].id;
                spinlock_release(&cluster_lock);
                return existing_id;
            }
        }
    }

    // Assign to nearest cluster if within threshold and not full
    if (best_idx >= 0 && best_dist <= CLUSTER_THRESHOLD &&
        g_clusters[best_idx].member_count < CLUSTER_MAX_MEMBERS) {
        uint32_t mc = g_clusters[best_idx].member_count;
        g_clusters[best_idx].members[mc].inode_num = inode_num;
        g_clusters[best_idx].members[mc].simhash = simhash;
        cl_strncpy(g_clusters[best_idx].members[mc].name, filename, 28);
        g_clusters[best_idx].member_count++;
        uint32_t cid = g_clusters[best_idx].id;
        spinlock_release(&cluster_lock);
        return cid;
    }

    // Create a new cluster if room available
    if (g_cluster_count >= CLUSTER_MAX) {
        spinlock_release(&cluster_lock);
        return 0;  // Max clusters reached
    }

    // Find first empty slot
    int slot = -1;
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&cluster_lock);
        return 0;
    }

    // Initialize new cluster with this file as leader
    uint32_t new_id = g_next_cluster_id++;
    g_clusters[slot].id = new_id;
    g_clusters[slot].leader_hash = simhash;
    g_clusters[slot].leader_inode = inode_num;
    cl_strncpy(g_clusters[slot].leader_name, filename, 28);
    g_clusters[slot].member_count = 1;
    g_clusters[slot].members[0].inode_num = inode_num;
    g_clusters[slot].members[0].simhash = simhash;
    cl_strncpy(g_clusters[slot].members[0].name, filename, 28);
    g_cluster_count++;

    serial_write("[Cluster] New cluster ");
    serial_write_dec(new_id);
    serial_write(" created, leader=");
    serial_write(filename);
    serial_write("\n");

    spinlock_release(&cluster_lock);
    return new_id;
}

// Mount-time reconstruction: add a file with known cluster_id
void cluster_rebuild_add(uint32_t inode_num, uint32_t cluster_id,
                         uint64_t simhash, const char *filename) {
    if (cluster_id == 0) return;

    spinlock_acquire(&cluster_lock);

    // Find existing cluster with this id
    int found = -1;
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == cluster_id) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        // Add as member to existing cluster
        if (g_clusters[found].member_count < CLUSTER_MAX_MEMBERS) {
            uint32_t mc = g_clusters[found].member_count;
            g_clusters[found].members[mc].inode_num = inode_num;
            g_clusters[found].members[mc].simhash = simhash;
            cl_strncpy(g_clusters[found].members[mc].name, filename, 28);
            g_clusters[found].member_count++;
        }
    } else {
        // Create new cluster with this file as leader
        int slot = -1;
        for (int i = 0; i < CLUSTER_MAX; i++) {
            if (g_clusters[i].id == 0) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            g_clusters[slot].id = cluster_id;
            g_clusters[slot].leader_hash = simhash;
            g_clusters[slot].leader_inode = inode_num;
            cl_strncpy(g_clusters[slot].leader_name, filename, 28);
            g_clusters[slot].member_count = 1;
            g_clusters[slot].members[0].inode_num = inode_num;
            g_clusters[slot].members[0].simhash = simhash;
            cl_strncpy(g_clusters[slot].members[0].name, filename, 28);
            g_cluster_count++;
        }
    }

    // Track max id for g_next_cluster_id
    if (cluster_id >= g_next_cluster_id) {
        g_next_cluster_id = cluster_id + 1;
    }

    spinlock_release(&cluster_lock);
}

// Finalize rebuild: count active clusters, ensure next_id is correct
void cluster_rebuild_finalize(void) {
    spinlock_acquire(&cluster_lock);

    g_cluster_count = 0;
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id != 0) {
            g_cluster_count++;
        }
    }

    serial_write("[Cluster] Rebuild complete: ");
    serial_write_dec(g_cluster_count);
    serial_write(" clusters, next_id=");
    serial_write_dec(g_next_cluster_id);
    serial_write("\n");

    spinlock_release(&cluster_lock);
}

// Get list of all active clusters
int cluster_get_list(cluster_list_t *out) {
    if (!out) return -1;

    spinlock_acquire(&cluster_lock);

    cl_memset(out, 0, sizeof(cluster_list_t));
    uint32_t count = 0;

    for (int i = 0; i < CLUSTER_MAX && count < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == 0) continue;

        out->clusters[count].id = g_clusters[i].id;
        cl_strncpy(out->clusters[count].name, g_clusters[i].leader_name, 28);
        out->clusters[count].member_count = g_clusters[i].member_count;
        out->clusters[count].centroid = g_clusters[i].leader_hash;
        count++;
    }
    out->count = count;

    spinlock_release(&cluster_lock);
    return (int)count;
}

// Get members of a specific cluster
int cluster_get_members(uint32_t cluster_id, cluster_members_t *out) {
    if (!out || cluster_id == 0) return -1;

    spinlock_acquire(&cluster_lock);

    cl_memset(out, 0, sizeof(cluster_members_t));

    // Find cluster
    int found = -1;
    for (int i = 0; i < CLUSTER_MAX; i++) {
        if (g_clusters[i].id == cluster_id) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        spinlock_release(&cluster_lock);
        return -1;
    }

    out->cluster_id = g_clusters[found].id;
    cl_strncpy(out->leader_name, g_clusters[found].leader_name, 28);
    out->count = g_clusters[found].member_count;

    for (uint32_t m = 0; m < g_clusters[found].member_count; m++) {
        out->members[m].inode_num = g_clusters[found].members[m].inode_num;
        cl_strncpy(out->members[m].name, g_clusters[found].members[m].name, 28);
        out->members[m].distance = simhash_hamming_distance(
            g_clusters[found].leader_hash,
            g_clusters[found].members[m].simhash
        );
    }

    spinlock_release(&cluster_lock);
    return (int)out->count;
}

// Get number of active clusters
int cluster_get_count(void) {
    spinlock_acquire(&cluster_lock);
    int c = g_cluster_count;
    spinlock_release(&cluster_lock);
    return c;
}
