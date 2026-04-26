// kernel/fs/cluster_v2.h
//
// Phase 19 — Sequential-Leader clustering adapted to the v2 inode layout.
//
// v2 inodes carry SimHash directly at `ai_embedding[0]` and cluster_id in
// `ai_reserved[0..3]` (4 bytes, little-endian). The Phase 11b cluster engine
// already handles the leader/member logic — this file just offers v2 helpers
// that read/write those fields consistently and call through to the engine.
//
// This header is intentionally thin. The heavy lifting lives in cluster.c.
#pragma once

#include <stdint.h>

// Rebuild one inode's cluster_id in the in-memory table at mount time.
// Called from grahafs_v2_mount after successful journal replay for every
// inode whose ai_embedding[0] is non-zero. `name` is best-effort (the
// first few characters of the inode table key); it feeds the diagnostic
// `clusters` CLI without adding a cross-reference lookup.
void cluster_v2_rebuild_add(uint32_t inode_num, uint32_t cluster_id,
                            uint64_t simhash, const char *name);

// Assign / reassign the cluster for this inode. Reads the SimHash from the
// inode's cached `ai_embedding[0]`, runs the Sequential Leader algorithm,
// writes the chosen cluster_id back into `ai_reserved[0..3]`, and journals
// the inode update. Safe to call from a dedicated recluster worker thread.
//
// Returns the cluster_id (1..CLUSTER_MAX) or 0 if the in-memory table is
// full.
uint32_t cluster_v2_assign_inode(uint32_t inode_num);

// Helpers for reading/writing cluster_id out of an in-memory inode copy.
// Inlined here because they're trivial but error-prone.
static inline uint32_t v2_inode_read_cluster_id(const uint8_t *ai_reserved32) {
    return ((uint32_t)ai_reserved32[0])        |
           ((uint32_t)ai_reserved32[1] << 8)   |
           ((uint32_t)ai_reserved32[2] << 16)  |
           ((uint32_t)ai_reserved32[3] << 24);
}

static inline void v2_inode_write_cluster_id(uint8_t *ai_reserved32, uint32_t id) {
    ai_reserved32[0] = (uint8_t)(id & 0xFF);
    ai_reserved32[1] = (uint8_t)((id >> 8)  & 0xFF);
    ai_reserved32[2] = (uint8_t)((id >> 16) & 0xFF);
    ai_reserved32[3] = (uint8_t)((id >> 24) & 0xFF);
}
