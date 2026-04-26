// kernel/fs/segment.c
//
// Phase 19 — segment lifecycle. See segment.h for invariants.
//
// MVP scope: segments live in a small in-memory array (up to 32 per mount).
// Each entry mirrors an on-disk segment_header read at mount. Allocations
// go linearly through the currently-active segment; when it fills, it is
// sealed and the next FREE slot is promoted to ACTIVE.
//
// What we skip for MVP (documented in problems/phase19 at end of phase):
//   * Deferred checkpointing (inline only).
//   * Age-based retention (count-only by default; gc_set_max_age_ns
//     enables a 7-day retention window if set non-zero).
//   * Per-segment read-ahead / prefetch.
//   * On-disk segment-table checksum verification (relies on header magic).

#define __GRAHAFS_V2_INTERNAL__
#include "segment.h"

#include <stddef.h>
#include <string.h>

#include "../log.h"
#include "../sync/spinlock.h"
#include "blk_client.h"
#include "../lib/crc32.h"

#define SEGMENT_TABLE_MAX_ENTRIES 32u

static grahafs_v2_segment_t g_segments[SEGMENT_TABLE_MAX_ENTRIES];
static uint32_t             g_segment_count = 0;
static uint32_t             g_active_segment_id = 0xFFFFFFFFu;
static int                  g_segment_device_id = -1;
static uint64_t             g_segment_table_lba = 0;
static spinlock_t           g_segment_table_lock = SPINLOCK_INITIALIZER("seg_table");

// ---------------------------------------------------------------------------
// On-disk helpers.
// ---------------------------------------------------------------------------
static uint32_t segment_header_crc(const grahafs_v2_segment_header_t *hdr) {
    return crc32_buf(hdr, offsetof(grahafs_v2_segment_header_t, checksum_header));
}

static int segment_write_header(uint32_t seg_idx) {
    if (seg_idx >= g_segment_count) return -5;  // -EIO.
    grahafs_v2_segment_t *s = &g_segments[seg_idx];

    grahafs_v2_segment_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic                  = GRAHAFS_V2_SEGMENT_MAGIC;
    hdr.segment_id             = s->segment_id;
    hdr.creation_txn           = s->creation_txn;
    hdr.size_blocks            = s->size_blocks;
    hdr.state                  = s->state;
    hdr.refcount               = s->refcount;
    hdr.first_version_offset   = 0;
    hdr.version_record_count   = s->version_record_count;
    hdr.free_bytes_in_segment  = s->free_bytes_in_segment;
    hdr.next_free_block_offset = s->next_free_block_offset;
    hdr.checksum_header        = segment_header_crc(&hdr);

    if (grahafs_block_write((uint8_t)g_segment_device_id, s->first_block, 1, &hdr) != 1) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "segment_write_header: grahafs_block_write seg=%u lba=%llu failed",
             seg_idx, (unsigned long long)s->first_block);
        return -5;
    }
    return 0;
}

static int segment_read_header(uint32_t seg_idx) {
    grahafs_v2_segment_t *s = &g_segments[seg_idx];
    uint8_t buf[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_block_read((uint8_t)g_segment_device_id, s->first_block, 1, buf) != 1) {
        return -5;
    }
    grahafs_v2_segment_header_t *hdr = (grahafs_v2_segment_header_t *)buf;
    if (hdr->magic != GRAHAFS_V2_SEGMENT_MAGIC) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "segment_read_header: bad magic seg=%u got=0x%08x",
             seg_idx, hdr->magic);
        return -126;  // -EBADFS.
    }
    // Verify the header CRC. The write path computes
    // crc32_buf(hdr, offsetof(checksum_header)) and writes the result into
    // checksum_header before persisting. We re-zero it, recompute, and
    // compare. On mismatch we return -EBADFS — the segment is corrupt and
    // any caller that needs durable data should bail rather than trust
    // stale fields. (Mismatch is treated as fatal because the write path is
    // the sole writer; a divergence indicates either a torn write or
    // bit-rot.)
    uint32_t expected = hdr->checksum_header;
    hdr->checksum_header = 0;
    uint32_t got = crc32_buf(hdr,
                             offsetof(grahafs_v2_segment_header_t, checksum_header));
    if (expected != got) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "segment_read_header: CRC mismatch seg=%u expected=0x%08x got=0x%08x",
             seg_idx, expected, got);
        return -126;  // -EBADFS
    }

    s->segment_id             = hdr->segment_id;
    s->size_blocks            = hdr->size_blocks;
    s->refcount               = hdr->refcount;
    s->creation_txn           = hdr->creation_txn;
    s->state                  = hdr->state;
    s->next_free_block_offset = hdr->next_free_block_offset;
    s->free_bytes_in_segment  = hdr->free_bytes_in_segment;
    s->version_record_count   = hdr->version_record_count;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

int segment_subsystem_init(int device_id,
                           uint64_t segment_table_lba,
                           uint32_t segment_count_max,
                           uint64_t data_blocks_start_lba) {
    if (segment_count_max == 0) return -5;
    if (segment_count_max > SEGMENT_TABLE_MAX_ENTRIES) {
        segment_count_max = SEGMENT_TABLE_MAX_ENTRIES;
    }

    g_segment_device_id  = device_id;
    g_segment_table_lba  = segment_table_lba;
    g_segment_count      = segment_count_max;
    g_active_segment_id  = 0xFFFFFFFFu;

    // Read segment table block. Entry layout (16 bytes each, per mkfs):
    //    [0..7]  : first_block (u64)
    //    [8..15] : reserved
    uint8_t tbl[GRAHAFS_V2_BLOCK_SIZE];
    if (grahafs_block_read((uint8_t)device_id, segment_table_lba, 1, tbl) != 1) {
        klog(KLOG_ERROR, SUBSYS_FS,
             "segment_subsystem_init: failed to read segment table at lba=%llu",
             (unsigned long long)segment_table_lba);
        return -5;
    }
    const uint64_t *entries = (const uint64_t *)tbl;

    for (uint32_t i = 0; i < g_segment_count; ++i) {
        grahafs_v2_segment_t *s = &g_segments[i];
        memset(s, 0, sizeof(*s));
        spinlock_init(&s->lock, "seg");
        uint64_t first_block = entries[i * 2];  // Stride matches mkfs (16 B).
        if (first_block == 0) {
            // Unpopulated slot: synthesize a FREE segment at the implied LBA.
            s->segment_id = i;
            s->first_block = data_blocks_start_lba +
                             (uint64_t)i * GRAHAFS_V2_SEGMENT_BLOCKS;
            s->size_blocks = GRAHAFS_V2_SEGMENT_BLOCKS;
            s->state = GRAHAFS_V2_SEG_FREE;
            continue;
        }
        s->segment_id = i;
        s->first_block = first_block;
        s->size_blocks = GRAHAFS_V2_SEGMENT_BLOCKS;
        int rc = segment_read_header(i);
        if (rc != 0) {
            // Header unreadable — mark FREE and continue. Could have been
            // an abandoned segment. Real corruption triggers -EBADFS at the
            // caller-boundary in grahafs_v2_mount.
            s->state = GRAHAFS_V2_SEG_FREE;
            klog(KLOG_WARN, SUBSYS_FS,
                 "segment_subsystem_init: seg=%u header unreadable, marked FREE",
                 i);
            continue;
        }
        if (s->state == GRAHAFS_V2_SEG_ACTIVE) {
            if (g_active_segment_id == 0xFFFFFFFFu) {
                g_active_segment_id = i;
            } else {
                // Two ACTIVE segments: demote the older to SEALED.
                s->state = GRAHAFS_V2_SEG_SEALED;
                segment_write_header(i);
                klog(KLOG_WARN, SUBSYS_FS,
                     "segment_subsystem_init: extra ACTIVE seg=%u demoted to SEALED",
                     i);
            }
        }
    }

    // If no ACTIVE found, promote the first FREE.
    if (g_active_segment_id == 0xFFFFFFFFu) {
        for (uint32_t i = 0; i < g_segment_count; ++i) {
            if (g_segments[i].state == GRAHAFS_V2_SEG_FREE) {
                g_segments[i].state = GRAHAFS_V2_SEG_ACTIVE;
                g_segments[i].refcount = 0;
                g_segments[i].next_free_block_offset =
                    GRAHAFS_V2_BLOCK_SIZE;  // block 0 = header.
                g_segments[i].free_bytes_in_segment =
                    (GRAHAFS_V2_SEGMENT_BLOCKS - 1) * GRAHAFS_V2_BLOCK_SIZE;
                segment_write_header(i);
                g_active_segment_id = i;
                klog(KLOG_INFO, SUBSYS_FS,
                     "segment_subsystem_init: promoted seg=%u to ACTIVE", i);
                break;
            }
        }
    }

    klog(KLOG_INFO, SUBSYS_FS,
         "segment_subsystem_init: count=%u active=%u",
         g_segment_count, g_active_segment_id);
    return 0;
}

void segment_subsystem_shutdown(void) {
    spinlock_acquire(&g_segment_table_lock);
    for (uint32_t i = 0; i < g_segment_count; ++i) {
        if (g_segments[i].state != GRAHAFS_V2_SEG_FREE) {
            (void)segment_write_header(i);
        }
    }
    g_segment_count = 0;
    g_active_segment_id = 0xFFFFFFFFu;
    g_segment_device_id = -1;
    spinlock_release(&g_segment_table_lock);
}

grahafs_v2_segment_t *segment_get(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return NULL;
    return &g_segments[segment_id];
}

uint32_t segment_allocate_for_write(uint32_t bytes_needed) {
    if (bytes_needed == 0) return 0xFFFFFFFFu;

    spinlock_acquire(&g_segment_table_lock);

    // Try the currently active segment first.
    for (;;) {
        uint32_t id = g_active_segment_id;
        if (id != 0xFFFFFFFFu) {
            grahafs_v2_segment_t *s = &g_segments[id];
            if (s->state == GRAHAFS_V2_SEG_ACTIVE &&
                s->free_bytes_in_segment >= bytes_needed) {
                // Bump refcount BEFORE returning — Plan §Tricky Bit #6 race fix.
                s->refcount++;
                s->free_bytes_in_segment -= bytes_needed;
                s->next_free_block_offset += bytes_needed;
                (void)segment_write_header(id);
                spinlock_release(&g_segment_table_lock);
                return id;
            }
            // Active segment full — seal it.
            s->state = GRAHAFS_V2_SEG_SEALED;
            (void)segment_write_header(id);
            klog(KLOG_INFO, SUBSYS_FS,
                 "segment_allocate: seg=%u full, sealed", id);
            g_active_segment_id = 0xFFFFFFFFu;
        }

        // Promote first FREE to ACTIVE.
        uint32_t new_active = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < g_segment_count; ++i) {
            if (g_segments[i].state == GRAHAFS_V2_SEG_FREE) {
                new_active = i;
                break;
            }
        }
        if (new_active == 0xFFFFFFFFu) {
            spinlock_release(&g_segment_table_lock);
            klog(KLOG_ERROR, SUBSYS_FS,
                 "segment_allocate: no FREE segments available");
            return 0xFFFFFFFFu;  // Exhausted.
        }
        grahafs_v2_segment_t *s = &g_segments[new_active];
        s->state = GRAHAFS_V2_SEG_ACTIVE;
        s->refcount = 0;
        s->next_free_block_offset = GRAHAFS_V2_BLOCK_SIZE;  // After header.
        s->free_bytes_in_segment =
            (GRAHAFS_V2_SEGMENT_BLOCKS - 1) * GRAHAFS_V2_BLOCK_SIZE;
        (void)segment_write_header(new_active);
        g_active_segment_id = new_active;
        // Loop back to retry the allocation against the new ACTIVE segment.
    }
}

int segment_seal(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return -22;  // -EINVAL.
    spinlock_acquire(&g_segment_table_lock);
    grahafs_v2_segment_t *s = &g_segments[segment_id];
    if (s->state == GRAHAFS_V2_SEG_ACTIVE) {
        s->state = GRAHAFS_V2_SEG_SEALED;
        if (g_active_segment_id == segment_id) {
            g_active_segment_id = 0xFFFFFFFFu;
        }
        (void)segment_write_header(segment_id);
    }
    spinlock_release(&g_segment_table_lock);
    return 0;
}

void segment_ref_inc(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return;
    spinlock_acquire(&g_segment_table_lock);
    g_segments[segment_id].refcount++;
    (void)segment_write_header(segment_id);
    spinlock_release(&g_segment_table_lock);
}

void segment_ref_dec(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return;
    spinlock_acquire(&g_segment_table_lock);
    if (g_segments[segment_id].refcount > 0) {
        g_segments[segment_id].refcount--;
        (void)segment_write_header(segment_id);
    }
    spinlock_release(&g_segment_table_lock);
}

bool segment_reclaim_if_eligible(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return false;
    spinlock_acquire(&g_segment_table_lock);
    grahafs_v2_segment_t *s = &g_segments[segment_id];
    // INVARIANT: ACTIVE segments are NEVER reclaimable (Plan §Tricky Bit #6).
    if (s->state == GRAHAFS_V2_SEG_SEALED && s->refcount == 0) {
        s->state = GRAHAFS_V2_SEG_RECLAIMABLE;
        (void)segment_write_header(segment_id);
        spinlock_release(&g_segment_table_lock);
        klog(KLOG_INFO, SUBSYS_FS,
             "segment_reclaim: seg=%u marked RECLAIMABLE", segment_id);
        return true;
    }
    spinlock_release(&g_segment_table_lock);
    return false;
}

void segment_return_to_free(uint32_t segment_id) {
    if (segment_id >= g_segment_count) return;
    spinlock_acquire(&g_segment_table_lock);
    grahafs_v2_segment_t *s = &g_segments[segment_id];
    if (s->state == GRAHAFS_V2_SEG_RECLAIMABLE) {
        s->state = GRAHAFS_V2_SEG_FREE;
        s->refcount = 0;
        s->version_record_count = 0;
        s->next_free_block_offset = 0;
        s->free_bytes_in_segment = 0;
        (void)segment_write_header(segment_id);
    }
    spinlock_release(&g_segment_table_lock);
}

static uint32_t segment_count_by_state(uint8_t state) {
    uint32_t n = 0;
    spinlock_acquire(&g_segment_table_lock);
    for (uint32_t i = 0; i < g_segment_count; ++i) {
        if (g_segments[i].state == state) n++;
    }
    spinlock_release(&g_segment_table_lock);
    return n;
}

uint32_t segment_count_active(void)      { return segment_count_by_state(GRAHAFS_V2_SEG_ACTIVE); }
uint32_t segment_count_sealed(void)      { return segment_count_by_state(GRAHAFS_V2_SEG_SEALED); }
uint32_t segment_count_reclaimable(void) { return segment_count_by_state(GRAHAFS_V2_SEG_RECLAIMABLE); }
