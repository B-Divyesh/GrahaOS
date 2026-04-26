// kernel/fs/segment.h
//
// Phase 19 — segment subsystem for GrahaFS v2.
//
// A "segment" is a 128 MB (32768-block) logical container on disk. Each
// segment owns an on-disk segment_header at its first block. File data
// blocks + version_record entries live inside the segment. Version records
// carry a segment_id; when the last version record referencing a segment
// is pruned by GC, the segment becomes reclaimable.
//
// Lifecycle invariants (Phase 19 plan Tricky Bit #6):
//
//    FREE  --[allocate_for_write]-->  ACTIVE  (refcount bumped pre-return)
//    ACTIVE --[seal]-->               SEALED  (no more writes allowed)
//    SEALED + refcount==0 -->         RECLAIMABLE --[reclaim]--> FREE
//
//    *** ACTIVE segments are NEVER reclaimable regardless of refcount. ***
//
// That rule prevents a writer-vs-GC race where GC observes a fresh segment
// with refcount==0 before the writer has committed its first version record.
//
// Locks:
//    g_segment_table_lock guards the in-memory table. Individual
//    segment_t structs carry their own lock for concurrent refcount
//    mutations against other fields. Acquire order: inode_lock →
//    segment_table_lock (see plan §Tricky Bit #3).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grahafs_v2.h"

// Initialise the segment subsystem. Called from grahafs_v2_mount (U17).
// Reads the on-disk segment table (one block per spec) plus each segment's
// header. Rebuilds the in-memory g_segments[] array. Returns 0 or -errno.
int segment_subsystem_init(int device_id,
                           uint64_t segment_table_lba,
                           uint32_t segment_count_max,
                           uint64_t data_blocks_start_lba);

// Shutdown: flushes all dirty segment headers back to disk, zero g_segments.
void segment_subsystem_shutdown(void);

// Look up segment by id (returns NULL if out-of-range).
grahafs_v2_segment_t *segment_get(uint32_t segment_id);

// Allocate a block-range inside the currently active segment. On success,
// bumps the segment's refcount BEFORE returning (Plan §Tricky Bit #6). If
// the active segment doesn't have enough free bytes to satisfy the request,
// seals it and allocates a fresh ACTIVE segment. Returns segment_id on
// success, or 0xFFFFFFFF (UINT32_MAX) on exhaustion.
//
// `bytes_needed` may span multiple blocks; caller then allocates individual
// LBAs inside the segment (e.g., via block tree walker).
uint32_t segment_allocate_for_write(uint32_t bytes_needed);

// Seal a segment (no new writes). Usually invoked when active-segment space
// runs low, or explicitly during GC.
int segment_seal(uint32_t segment_id);

// Refcount mutators. Required for every version_record that points into the
// segment. Writer calls inc pre-commit; GC prune calls dec post-commit.
void segment_ref_inc(uint32_t segment_id);
void segment_ref_dec(uint32_t segment_id);

// GC hook. Returns true and marks RECLAIMABLE if the segment is both SEALED
// and has refcount==0. Returns false in any other state (including ACTIVE,
// per the invariant).
bool segment_reclaim_if_eligible(uint32_t segment_id);

// Return the segment to FREE after its data blocks have been reclaimed by
// the bitmap allocator. Caller must ensure no readers. Called from gc.c.
void segment_return_to_free(uint32_t segment_id);

// Diagnostics: snapshot refcounts for /bin/memstat-style output.
uint32_t segment_count_active(void);
uint32_t segment_count_sealed(void);
uint32_t segment_count_reclaimable(void);
