// kernel/snap/restore.c — Phase 24 W16 snap_restore.
//
// Restores a previously-captured snapshot. The barrier window is held while
// page tables, FD tables, and FS chain heads are rolled back. Channel thaw
// (W18) is invoked on every channel that was frozen at capture time.
//
// v1 design choices.
//   - The captured task's live regs are NOT restored. The caller is in the
//     middle of executing snap_restore in kernel context; rewriting its
//     userspace return frame would break the kernel stack invariants.
//     Restoring regs needs a syscall-frame swizzle that is more delicate
//     than the snapshot machinery itself; left for a follow-up that grows
//     a syscall_set_user_frame helper.
//   - For non-caller tasks under SNAP_SCOPE_GLOBAL we DO replace their
//     task->regs because they are parked in TASK_STATE_BARRIER_WAIT and
//     dispatched from task->regs on resume.
//   - Page restore: for every captured (virt, phys, flags) we re-install
//     the page in the live task's CR3 with the writable bit cleared (so a
//     subsequent write triggers cow_fault and re-establishes RW + COW).
//     If the live PTE already points at `phys`, we just restore flags.
//     If it points at a different phys (because the parent COW'd), we
//     unmap + pmm_unref the new phys and install the captured one with a
//     fresh pmm_page_ref.
//   - FS revert: for every fs_pin we call grahafs_revert_to_version which
//     pushes a VE_FLAG_REVERT_CREATED entry making the pinned version the
//     new HEAD.
//   - Channel thaw: for now only the ones explicitly captured (none in
//     v1 since W14.6 freeze-all walk is stubbed).
//
// All page-restore loops are O(N_captured_pages) per task. Inside the
// barrier, the only concurrent state mutator is the snap_create caller
// itself — there is no preemption to worry about.

#include "snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../log.h"
#include "../mm/kheap.h"
#include "../fs/grahafs_v2.h"
#include "../ipc/channel.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"

#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/interrupts.h"
#include "../pid_hash.h"

#define R_EPERM    1
#define R_ENOMEM  12
#define R_EINVAL  22
#define R_EBUSY   16
#define R_ENOSYS  38

// PHYS_ADDR_MASK isolates the bits-12..51 physical-address payload
// from a PTE; see the explanation in capture.c.
#define PHYS_ADDR_MASK  0x000FFFFFFFFFF000ULL

// ---------------------------------------------------------------------------
// Restore captured pages into a live task's CR3.
// ---------------------------------------------------------------------------
static int restore_pages(snapshot_task_entry_t *te, task_t *live) {
    if (!te || !live) return -R_EINVAL;
    if (te->page_count == 0) return 0;

    uint64_t cr3 = live->cr3;
    uint64_t restored = 0;
    uint64_t replaced = 0;
    uint64_t reflags  = 0;
    uint64_t skipped  = 0;

    for (uint32_t i = 0; i < te->page_count; i++) {
        snap_captured_page_t *cap = &te->pages[i];
        // Sanity: skip entries pointing at clearly bogus addresses.
        if (cap->phys == 0) { skipped++; continue; }

        uint64_t live_pte = vmm_get_pte(cr3, cap->virt);
        uint64_t live_phys = live_pte & PHYS_ADDR_MASK;
        // Restore semantics: install captured phys at virt with the
        // captured flags but force the writable bit clear so subsequent
        // writes trigger cow_fault. The captured cow_page_tracker
        // refcount keeps the page alive (already bumped at snap_create).
        uint64_t restore_flags = (cap->flags & ~PTE_WRITABLE) | PTE_PRESENT;

        if (live_pte != 0 && live_phys == cap->phys) {
            // Already the same phys; just sync flags (might have been
            // promoted to writable by cow_fault). Force RO so the
            // snapshot-shared invariant is restored.
            (void)vmm_protect_page_by_cr3(cr3, cap->virt, restore_flags);
            reflags++;
            continue;
        }

        // Different physical page — undo the COW. Unmap the live mapping
        // (drops one pmm refcount on live_phys), bump pmm_page_ref on the
        // captured phys (we are about to add a second reference besides
        // the snapshot's), and map.
        if (live_pte != 0) {
            vmm_unmap_page_by_cr3(cr3, cap->virt);
            pmm_page_unref((void *)live_phys);
        }
        // Bump pmm refcount for the parent's restored mapping. The
        // snapshot already holds one ref; this adds the parent's ref
        // back, which will be dropped if the parent later COWs.
        pmm_page_ref((void *)cap->phys);
        // The cow_page_tracker for this phys may still be live (we
        // bumped it at snap_create). We re-bump so refcount reflects
        // both the snapshot and the freshly-restored parent mapping.
        cow_page_tracker_bump(cap->phys);

        if (!vmm_map_page_by_cr3(cr3, cap->virt, cap->phys, restore_flags)) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "snap_restore: vmm_map_page_by_cr3 failed va=0x%lx phys=0x%lx",
                 (unsigned long)cap->virt, (unsigned long)cap->phys);
            // Roll back the bumps to keep accounting balanced.
            cow_page_tracker_put(cap->phys);
            pmm_page_unref((void *)cap->phys);
            skipped++;
            continue;
        }
        replaced++;
        restored++;
    }

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_restore: pid=%d pages=%u replaced=%lu reflagged=%lu skipped=%lu",
         (int)live->id, (unsigned)te->page_count,
         (unsigned long)replaced, (unsigned long)reflags,
         (unsigned long)skipped);
    return 0;
}

// ---------------------------------------------------------------------------
// Restore a non-caller task's regs + FD table.
// ---------------------------------------------------------------------------
static void restore_regs_and_fds(snapshot_task_entry_t *te, task_t *live) {
    if (!te || !live) return;
    if (te->regs) {
        live->regs = *te->regs;
    }
    if (te->fd_table_copy) {
        memcpy(live->fd_table, te->fd_table_copy, sizeof(live->fd_table));
    }
    live->pledge_mask.raw = te->pledge_snapshot;
}

// ---------------------------------------------------------------------------
// snap_restore (declared in snapshot.h).
// ---------------------------------------------------------------------------
int snap_restore(uint32_t handle) {
    task_t *current = sched_get_current_task();
    if (!current) return -R_EPERM;

    cap_handle_entry_t *entry = cap_handle_lookup(&current->cap_handles, handle);
    if (!entry) return -R_EINVAL;

    cap_object_t *obj = cap_object_get(entry->object_idx);
    if (!obj || obj->kind != CAP_KIND_SNAPSHOT) return -R_EINVAL;

    snapshot_t *s = (snapshot_t *)obj->kind_data;
    if (!s) return -R_EINVAL;
    if (s->state != SNAP_STATE_ACTIVE) return -R_EBUSY;

    s->state = SNAP_STATE_RESTORING;

    int barrier_rc = snap_begin_barrier();
    if (barrier_rc != 0) {
        s->state = SNAP_STATE_ACTIVE;
        return barrier_rc;
    }

    // Walk every captured task; restore regs + FDs (non-caller) + pages.
    //
    // CRITICAL v1 limitation: the caller's user-half pages are NOT
    // restored. The caller is currently executing snap_restore via a
    // syscall; its userspace stack pages contain the live syscall return
    // frame (post snap_restore call site) and any local variables. If we
    // overwrite those pages with snap-time content, the userspace RSP
    // continues to point at the pre-restore physical address — but the
    // bytes there are now snap-time bytes, so the next `ret` pops a
    // bogus return address and the user task triple-faults.
    //
    // Restoring the caller requires either (a) a syscall_set_user_frame
    // helper that swizzles the in-kernel syscall_frame to match the
    // captured regs, or (b) treating SCOPE_SELF restore as a userspace
    // re-spawn. v1 documents this limitation: SCOPE_SELF restore is a
    // no-op for the caller; the FS revert + non-caller restores still
    // happen so the system-wide effect is real.
    for (uint32_t i = 0; i < s->task_count; i++) {
        snapshot_task_entry_t *te = &s->tasks[i];
        if (te->pid <= 0) continue;
        struct task_struct *live = pid_hash_lookup(te->pid);
        if (!live) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "snap_restore: captured pid=%d no longer alive — skipped",
                 (int)te->pid);
            continue;
        }
        if ((task_t *)live == current) {
            // Caller — see file-header note. Skip page + reg restore.
            continue;
        }
        (void)restore_pages(te, (task_t *)live);
        restore_regs_and_fds(te, (task_t *)live);
    }

    // FS revert: for each pinned (inode, version) push a revert entry so
    // the inode's HEAD is the captured version again. grahafs_revert_to_
    // version returns 0 on success; non-zero is logged but does not abort
    // the restore (best-effort across pins).
    for (uint32_t i = 0; i < s->fs_pin_count; i++) {
        snapshot_fs_pin_t *p = &s->fs_pins[i];
        int rc = grahafs_revert_to_version((uint32_t)p->inode_id,
                                           p->pinned_version_id);
        if (rc < 0) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "snap_restore: revert(inode=%u, ver=%lu) rc=%d",
                 (unsigned)p->inode_id, (unsigned long)p->pinned_version_id,
                 rc);
        }
    }

    // Channel thaw: only entries actually captured at create-time.
    for (uint32_t i = 0; i < s->chan_count; i++) {
        snapshot_chan_entry_t *ch = &s->chans[i];
        // chan_thaw with the captured queue head/tail rewinds the ring
        // to the snap-time state. CHAN_THAW_KEEP keeps the live queue
        // (used by snap_delete; restore wants the rewind).
        chan_thaw(ch->chan_id, ch->queue_head_at_snap, ch->queue_tail_at_snap);
    }

    // Phase 24 W14.6 closeout — if the snapshot took FREEZE_ALL_CHANS,
    // chan_freeze_all_locked stamped every live channel; thaw them all
    // back to keep-as-is (post-restore queues already match the pre-snap
    // state for SCOPE_SELF; for SCOPE_GLOBAL the per-entry rewind above
    // handles in-scope peers).
    if (s->scope_flags & SNAP_SCOPE_FREEZE_ALL_CHANS) {
        chan_thaw_all_locked(s->id);
    }

    snap_end_barrier();
    s->state = SNAP_STATE_ACTIVE;

    klog(KLOG_INFO, SUBSYS_CORE,
         "snap_restore: id=%lu pid=%d tasks=%u fs_pins=%u chans=%u",
         (unsigned long)s->id, current->id,
         (unsigned)s->task_count, (unsigned)s->fs_pin_count,
         (unsigned)s->chan_count);

    return 0;
}
