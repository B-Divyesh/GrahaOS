// kernel/fs/cluster_v2.c
//
// Phase 19 — thin wrapper adapting the Sequential-Leader engine from
// cluster.c to v2 inodes. See cluster_v2.h for the contract.

#define __GRAHAFS_V2_INTERNAL__
#include "cluster_v2.h"
#include "cluster.h"
#include "grahafs_v2.h"
#include "journal.h"
#include "../log.h"
#include "../lib/crc32.h"

#include <stddef.h>
#include <string.h>

// inode_cache_get / inode_cache_put / journal_stage_inode are static inside
// grahafs_v2.c; re-declare the public ones we need here.
extern grahafs_v2_inode_cache_t *inode_cache_get(uint32_t inode_num);
extern void                      inode_cache_put(grahafs_v2_inode_cache_t *e);
extern int                       grahafs_v2_device_id(void);

void cluster_v2_rebuild_add(uint32_t inode_num, uint32_t cluster_id,
                            uint64_t simhash, const char *name) {
    cluster_rebuild_add(inode_num, cluster_id, simhash, name ? name : "");
}

uint32_t cluster_v2_assign_inode(uint32_t inode_num) {
    grahafs_v2_inode_cache_t *ce = inode_cache_get(inode_num);
    if (!ce) return 0;
    uint64_t simhash = ce->disk.ai_embedding[0];
    if (simhash == 0) {
        inode_cache_put(ce);
        return 0;
    }
    // Use the first 27 chars of the first tag as a display hint.
    char hint[28];
    memcpy(hint, ce->disk.ai_tags, 27);
    hint[27] = 0;

    uint32_t cid = cluster_assign(inode_num, simhash, hint);
    if (cid == 0) { inode_cache_put(ce); return 0; }

    // Persist cluster_id in ai_reserved[0..3] via a journaled metadata txn.
    journal_txn_t *txn = journal_txn_begin();
    if (!txn) { inode_cache_put(ce); return 0; }

    grahafs_v2_inode_t disk_copy = ce->disk;
    v2_inode_write_cluster_id(disk_copy.ai_reserved, cid);
    disk_copy.checksum_inode = 0;
    disk_copy.checksum_inode = crc32_buf(&disk_copy,
        offsetof(grahafs_v2_inode_t, checksum_inode));

    // Stage the inode-block holding this inode into the txn.
    extern int  journal_stage_inode_external(journal_txn_t *txn,
                                             uint32_t inode_num,
                                             const grahafs_v2_inode_t *ino);
    int rc = journal_stage_inode_external(txn, inode_num, &disk_copy);
    if (rc != 0) {
        journal_txn_abort(txn);
        inode_cache_put(ce);
        return 0;
    }
    if (journal_txn_commit(txn) != 0) {
        inode_cache_put(ce);
        return 0;
    }
    // Refresh cache.
    ce->disk = disk_copy;
    inode_cache_put(ce);
    return cid;
}
