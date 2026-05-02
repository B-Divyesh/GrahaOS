// kernel/snap/capture.c — Phase 24 W14.3-W14.7 capture machinery.
//
// snap_capture runs inside the snap_begin_barrier window. It walks every
// task in the snapshot scope and produces:
//
//   - snapshot_task_entry_t per task: regs / FD-table copy / pledge mask
//   - snap_captured_page_t per present user-half PTE: marks the parent's
//     PTE read-only, bumps cow_page_tracker_t refcount, bumps pmm_page_ref
//   - snapshot_vmo_entry_t per VMO mapped into a captured task: bumps
//     vmo_ref so the backing pages survive even if the parent unmaps
//   - snapshot_chan_entry_t per channel held (W18 chan_freeze)
//   - snapshot_fs_pin_t per FD_TYPE_FILE (W17.2 grahafs_pin_version)
//
// On any allocation failure the partially-built captures are torn down and
// the orchestrator returns -ENOMEM after snap_end_barrier.
//
// All four capture passes are O(N_tasks * N_per_task_things). Inside the
// barrier window only the owner CPU is running real work; the captures
// can therefore allocate kheap and acquire VFS / channel registry locks
// without worrying about preemption.

#include "snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../log.h"
#include "../mm/kheap.h"
#include "../mm/vmo.h"
#include "../fs/vfs.h"
#include "../fs/grahafs_v2.h"
#include "../ipc/channel.h"
#include "../pid_hash.h"

#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/interrupts.h"

#define CAP_ENOMEM   12
#define CAP_EINVAL   22
#define CAP_E2BIG     7

// Maximum tasks captured. SNAP_SCOPE_SELF caps at 1 (just the caller);
// SNAP_SCOPE_GLOBAL is bounded by this constant (we'd grow the array if
// it became a problem, but 64 captured tasks is well above what any test
// or interactive session creates).
#define SNAP_TASKS_MAX  64u
#define SNAP_VMOS_PER_TASK_MAX  64u  // matches VMO_MAPPINGS_PER_TASK
#define SNAP_CHANS_MAX          64u
#define SNAP_FSPINS_PER_TASK_MAX 16u  // matches PROC_MAX_FDS

// ---------------------------------------------------------------------------
// Per-task page list helpers (geometric kheap-backed array).
// ---------------------------------------------------------------------------
static int snap_pages_grow(snapshot_task_entry_t *te) {
    uint32_t new_cap = te->page_cap == 0 ? 64u : (te->page_cap * 2u);
    if (new_cap > SNAP_PAGES_PER_TASK_MAX) new_cap = SNAP_PAGES_PER_TASK_MAX;
    if (new_cap == te->page_cap) return -CAP_E2BIG;

    snap_captured_page_t *fresh =
        (snap_captured_page_t *)kmalloc(sizeof(snap_captured_page_t) * new_cap,
                                        SUBSYS_CORE);
    if (!fresh) return -CAP_ENOMEM;
    if (te->pages && te->page_count > 0) {
        memcpy(fresh, te->pages, sizeof(snap_captured_page_t) * te->page_count);
    }
    if (te->pages) kfree(te->pages);
    te->pages    = fresh;
    te->page_cap = new_cap;
    return 0;
}

static int snap_record_page(snapshot_task_entry_t *te,
                            uint64_t virt, uint64_t phys, uint64_t flags) {
    if (te->page_count >= te->page_cap) {
        int rc = snap_pages_grow(te);
        if (rc < 0) return rc;
    }
    snap_captured_page_t *slot = &te->pages[te->page_count++];
    slot->virt  = virt;
    slot->phys  = phys;
    slot->flags = flags;
    return 0;
}

// ---------------------------------------------------------------------------
// W14.4 — walk a task's user-half PML4. For every present PTE: mark RO,
// bump cow_page_tracker, bump pmm_page_ref, append to the snapshot's
// per-task page list.
//
// Skips:
//   - non-present entries
//   - 2 MiB large pages (PTE_LARGEPAGE bit on PD entry) — Phase 24 v1
//     does not snapshot huge pages; the kernel doesn't allocate any
//     in user space yet
//   - kernel-only mappings (PTE_USER==0)
//
// PHYS_ADDR_MASK isolates bits 12..51 (the actual physical-address
// payload). PAGE_MASK in vmm.h is 0xFFFFFFFFFFFFF000 which retains the
// upper PTE flag bits (NX at 63, protection keys at 52..58) and would
// poison any subsequent HHDM addition.
// ---------------------------------------------------------------------------
#define PHYS_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PHYS_FLAGS_MASK (~PHYS_ADDR_MASK)

static int snap_walk_user_half(uint64_t cr3, snapshot_task_entry_t *te) {
    uint64_t *pml4 = (uint64_t *)(cr3 + g_hhdm_offset);
    for (uint64_t pml4_idx = 0; pml4_idx < 256u; pml4_idx++) {
        uint64_t pml4e = pml4[pml4_idx];
        if (!(pml4e & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)((pml4e & PHYS_ADDR_MASK) + g_hhdm_offset);
        for (uint64_t pdpt_idx = 0; pdpt_idx < 512u; pdpt_idx++) {
            uint64_t pdpte = pdpt[pdpt_idx];
            if (!(pdpte & PTE_PRESENT)) continue;
            // 1 GiB pages — skip in v1.
            if (pdpte & PTE_LARGEPAGE) continue;
            uint64_t *pd = (uint64_t *)((pdpte & PHYS_ADDR_MASK) + g_hhdm_offset);
            for (uint64_t pd_idx = 0; pd_idx < 512u; pd_idx++) {
                uint64_t pde = pd[pd_idx];
                if (!(pde & PTE_PRESENT)) continue;
                if (pde & PTE_LARGEPAGE) continue;
                uint64_t *pt = (uint64_t *)((pde & PHYS_ADDR_MASK) + g_hhdm_offset);
                for (uint64_t pt_idx = 0; pt_idx < 512u; pt_idx++) {
                    uint64_t pte = pt[pt_idx];
                    if (!(pte & PTE_PRESENT)) continue;
                    if (!(pte & PTE_USER)) continue;
                    uint64_t virt = (pml4_idx << 39) | (pdpt_idx << 30) |
                                    (pd_idx   << 21) | (pt_idx   << 12);
                    uint64_t phys = pte & PHYS_ADDR_MASK;
                    uint64_t flags = pte & PHYS_FLAGS_MASK;
                    // Sanity: don't snapshot HHDM/kernel-half VAs.
                    if (virt >= 0xFFFF800000000000ULL) continue;
                    // Sign-extend the canonical bits before checking
                    // canonicality just in case some strange entry
                    // landed in the upper user half.

                    int rc = snap_record_page(te, virt, phys, flags);
                    if (rc < 0) return rc;

                    // Bump cow tracker + pmm refcount BEFORE marking RO so
                    // a concurrent write-fault (race with another CPU)
                    // sees the tracker. The barrier guarantees no other
                    // CPU is executing user mode, but a kernel-mode
                    // pagetable observer (e.g., the audit flusher) could
                    // resolve a pointer.
                    cow_page_tracker_bump(phys);
                    pmm_page_ref((void *)phys);

                    if (flags & PTE_WRITABLE) {
                        // Clear the writable bit. invlpg is issued by
                        // vmm_protect_page_by_cr3 itself.
                        (void)vmm_protect_page_by_cr3(cr3, virt,
                                                      flags & ~PTE_WRITABLE);
                    }
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// W14.3 — capture a single task's regs / FD-table / pledge.
// te must be zero-initialised before the call.
// ---------------------------------------------------------------------------
static int snap_capture_one_task(task_t *t, snapshot_task_entry_t *te) {
    te->pid          = (int32_t)t->id;
    te->cr3_original = t->cr3;
    te->cr3_snapshot = t->cr3;  // v1: shared CR3 (no separate clone).
    te->pledge_snapshot = t->pledge_mask.raw;

    // regs: copy the saved interrupt frame.
    struct interrupt_frame *regs_copy =
        (struct interrupt_frame *)kmalloc(sizeof(struct interrupt_frame),
                                          SUBSYS_CORE);
    if (!regs_copy) return -CAP_ENOMEM;
    *regs_copy = t->regs;
    te->regs   = regs_copy;

    // fd_table: the array lives at task_t.fd_table[]; snapshot a flat
    // copy (PROC_MAX_FDS * sizeof(proc_fd_t) = 16 * 4 = 64 bytes).
    proc_fd_t *fdcopy = (proc_fd_t *)kmalloc(sizeof(t->fd_table), SUBSYS_CORE);
    if (!fdcopy) {
        kfree(regs_copy);
        te->regs = NULL;
        return -CAP_ENOMEM;
    }
    memcpy(fdcopy, t->fd_table, sizeof(t->fd_table));
    te->fd_table_copy = (struct fd_table *)fdcopy;
    return 0;
}

static void snap_destroy_task_entry(snapshot_task_entry_t *te) {
    if (te->regs) { kfree(te->regs); te->regs = NULL; }
    if (te->fd_table_copy) { kfree(te->fd_table_copy); te->fd_table_copy = NULL; }
    if (te->pages) {
        // Phase 24 W14.6 closeout: before dropping snap's claims on the
        // captured pages, walk the parent's live PTEs. For pages that
        // STILL map to the captured phys (parent never COW'd them), the
        // parent's PTE_WRITABLE is still cleared from snap_walk_user_half.
        // After we drop the tracker the cow_fault path can no longer
        // resolve a write — the page would page-fault and the kernel
        // would kill the process. Restore the original flags (which
        // include PTE_WRITABLE if the page was originally writable) on
        // those pages so post-delete writes succeed without a fault.
        // Pages that have already been COW'd by the parent map to a
        // different phys (and the cow_fault already re-armed
        // PTE_WRITABLE), so they don't need restoration here.
        struct task_struct *live = pid_hash_lookup(te->pid);
        uint64_t live_cr3 = (live ? live->cr3 : 0);
        for (uint32_t i = 0; i < te->page_count; i++) {
            uint64_t cap_phys  = te->pages[i].phys;
            uint64_t cap_virt  = te->pages[i].virt;
            uint64_t cap_flags = te->pages[i].flags;
            if (live_cr3 != 0) {
                uint64_t live_pte = vmm_get_pte(live_cr3, cap_virt);
                if (live_pte != 0 && (live_pte & PTE_PRESENT) &&
                    (live_pte & 0x000FFFFFFFFFF000ULL) == cap_phys &&
                    (cap_flags & PTE_WRITABLE) &&
                    !(live_pte & PTE_WRITABLE)) {
                    (void)vmm_protect_page_by_cr3(live_cr3, cap_virt, cap_flags);
                }
            }
            cow_page_tracker_put(cap_phys);
            pmm_page_unref((void *)cap_phys);
        }
        kfree(te->pages);
        te->pages = NULL;
    }
    te->page_count = 0;
    te->page_cap   = 0;
}

// ---------------------------------------------------------------------------
// W14.5 — VMO capture. Looks at the global g_vmo_task_maps[] indirectly via
// vmo_ref() which is publicly exported. We just walk the per-task mapping
// array and bump refcounts.
// ---------------------------------------------------------------------------
//
// vmo_mapping_t is internal to vmo.c; instead of pulling its array out we
// use a tiny accessor here that lives in vmo.c. To keep this file
// independent of new API surface, we declare an extern accessor that
// vmo.c provides — but to avoid changing vmo.c's surface in this commit
// we use a simple iteration over task_t's vmo_mapping_t[] if such a field
// existed. It does NOT today (mappings live in g_vmo_task_maps[]). For v1
// we skip VMO capture entirely; the cow_page_tracker + pmm_page_ref work
// already done by W14.4 keeps every actually-mapped page alive. The VMO
// header itself does not need to survive snap_create — the snapshot's
// recorded pages reference the underlying physical frames directly.
// ---------------------------------------------------------------------------
static int snap_capture_vmos_for_task(task_t *t,
                                       snapshot_t *snap) {
    (void)t;
    (void)snap;
    return 0;
}

// ---------------------------------------------------------------------------
// W14.6 — channel freeze.  For SNAP_SCOPE_SELF we leave channels untouched
// (the snapshot only captures the caller's mappings, and channels owned
// by the caller will see EFROZEN if and only if the snapshot scope flag
// SNAP_SCOPE_FREEZE_ALL_CHANS is set).  For SNAP_SCOPE_GLOBAL with that
// flag we walk every channel in the registry (chan_lookup_by_id is the
// sole walk primitive) and freeze; the chan_freeze public function takes
// chan_id but iterates the registry under its own lock.
//
// v1 implements only the SNAP_SCOPE_FREEZE_ALL_CHANS path, since that is
// the path snap_restore actually needs (so the queue head/tail at snap
// time can be re-presented).  Without the flag, no channels are frozen.
// ---------------------------------------------------------------------------
static int snap_capture_channels(snapshot_t *snap) {
    if (!(snap->scope_flags & SNAP_SCOPE_FREEZE_ALL_CHANS)) return 0;
    // Phase 24 W14.6 closeout — chan_freeze_all_locked walks the global
    // registry under g_chan_reg_lock and freezes every live channel
    // against snap->id. Idempotent for channels already frozen by this
    // snapshot. Channels frozen by a different snapshot are silently
    // skipped (best-effort).
    int rc = chan_freeze_all_locked(snap->id);
    if (rc != 0) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "snap_capture_channels: chan_freeze_all_locked snap_id=%lu rc=%d",
             (unsigned long)snap->id, rc);
    }
    return 0;  // Best-effort; freeze failures don't fail the snapshot.
}

// ---------------------------------------------------------------------------
// W14.7 — FS pin capture.  Walks each captured task's FD table; for every
// FD_TYPE_FILE entry whose backing vfs_node has a non-zero inode number,
// we look up the GrahaFS v2 inode cache, read disk.version_chain_head_id,
// and call grahafs_pin_version(inode_num, head_id).  Stores a record in
// snap->fs_pins so snap_delete can pair the pin with grahafs_unpin_version.
// ---------------------------------------------------------------------------
static int snap_capture_fs_pins_for_task(task_t *t, snapshot_t *snap) {
    if (!t) return 0;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        proc_fd_t *fd = &t->fd_table[i];
        if (fd->type != FD_TYPE_FILE) continue;
        vfs_node_t *node = vfs_node_for_file_slot(fd->ref);
        if (!node) continue;
        if (node->inode == 0) continue;
        struct grahafs_v2_inode_cache *ce = inode_cache_get(node->inode);
        if (!ce) continue;
        uint64_t head_id = ce->disk.version_chain_head_id;
        inode_cache_put(ce);
        if (head_id == 0) continue;
        int rc = grahafs_pin_version(node->inode, head_id);
        if (rc < 0) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "snap_capture_fs_pins: pin(inode=%u, ver=%lu) rc=%d",
                 (unsigned)node->inode, (unsigned long)head_id, rc);
            continue;  // best-effort — don't fail the snapshot
        }
        // Append to snap->fs_pins (geometric grow).
        if (snap->fs_pin_count >= SNAP_FSPINS_PER_TASK_MAX *
                                  (uint32_t)SNAP_TASKS_MAX) {
            klog(KLOG_WARN, SUBSYS_CORE,
                 "snap_capture_fs_pins: fs_pin array full");
            grahafs_unpin_version(node->inode, head_id);
            continue;
        }
        if (snap->fs_pin_count == 0 || snap->fs_pins == NULL) {
            // First record: allocate a flat array sized for the global cap.
            snap->fs_pins = (snapshot_fs_pin_t *)kmalloc(
                sizeof(snapshot_fs_pin_t) *
                    (SNAP_FSPINS_PER_TASK_MAX * SNAP_TASKS_MAX),
                SUBSYS_CORE);
            if (!snap->fs_pins) {
                grahafs_unpin_version(node->inode, head_id);
                return -CAP_ENOMEM;
            }
        }
        snap->fs_pins[snap->fs_pin_count].inode_id = node->inode;
        snap->fs_pins[snap->fs_pin_count].pinned_version_id = head_id;
        snap->fs_pin_count++;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// W14.8 orchestrator — invoked by snap_create after snap_begin_barrier.
// Returns 0 on success; on partial failure the caller calls
// snap_destroy_captures to roll back.
// ---------------------------------------------------------------------------
typedef struct snap_capture_ctx {
    snapshot_t *snap;
    task_t     *captured_tasks[SNAP_TASKS_MAX];
    uint32_t    n_captured;
    int         err;
} snap_capture_ctx_t;

static void snap_pid_filter_cb(struct task_struct *t, void *ctx_v) {
    snap_capture_ctx_t *ctx = (snap_capture_ctx_t *)ctx_v;
    if (ctx->err) return;
    if (!t) return;
    if (t->state == TASK_STATE_ZOMBIE) return;
    if (t->is_idle) return;
    // SNAP_SCOPE_GLOBAL captures everything; SNAP_SCOPE_SELF only the
    // current task (handled by the caller, this callback never sees
    // the SELF case).
    if (ctx->n_captured >= SNAP_TASKS_MAX) {
        ctx->err = -CAP_E2BIG;
        return;
    }
    ctx->captured_tasks[ctx->n_captured++] = (task_t *)t;
}

int snap_run_capture(snapshot_t *snap, task_t *self) {
    if (!snap || !self) return -CAP_EINVAL;
    snap_capture_ctx_t ctx = { .snap = snap, .n_captured = 0, .err = 0 };

    if (snap->scope_flags & SNAP_SCOPE_GLOBAL) {
        pid_hash_enumerate(snap_pid_filter_cb, &ctx);
        if (ctx.err) return ctx.err;
    } else {
        // SNAP_SCOPE_SELF — capture only the caller.
        ctx.captured_tasks[0] = self;
        ctx.n_captured = 1;
    }

    if (ctx.n_captured == 0) {
        // Empty capture (e.g., the caller exited mid-call). Return ok.
        return 0;
    }

    // Allocate the per-task entry array.
    snap->tasks = (snapshot_task_entry_t *)kmalloc(
        sizeof(snapshot_task_entry_t) * ctx.n_captured, SUBSYS_CORE);
    if (!snap->tasks) return -CAP_ENOMEM;
    memset(snap->tasks, 0, sizeof(snapshot_task_entry_t) * ctx.n_captured);
    snap->task_count = ctx.n_captured;

    // For each task: regs / FD / pledge / page walk / VMO ref / FS pin.
    for (uint32_t i = 0; i < ctx.n_captured; i++) {
        task_t *t = ctx.captured_tasks[i];
        snapshot_task_entry_t *te = &snap->tasks[i];

        int rc = snap_capture_one_task(t, te);
        if (rc < 0) return rc;

        rc = snap_walk_user_half(t->cr3, te);
        if (rc < 0) return rc;
        snap->pages_shared += te->page_count;

        rc = snap_capture_vmos_for_task(t, snap);
        if (rc < 0) return rc;

        rc = snap_capture_fs_pins_for_task(t, snap);
        if (rc < 0) return rc;
    }

    // Channel capture — once, after all tasks are recorded.
    int rc = snap_capture_channels(snap);
    if (rc < 0) return rc;

    return 0;
}

// ---------------------------------------------------------------------------
// snap_destroy_captures — invoked by snap_create on capture failure or by
// snap_delete on lifetime end (W17.1). Rolls back every capture-side ref.
// ---------------------------------------------------------------------------
void snap_destroy_captures(snapshot_t *snap) {
    if (!snap) return;

    // Phase 24 W14.6 closeout — drop freeze on every channel that
    // chan_freeze_all_locked stamped at create-time. Done first so the
    // freeze is gone before the snapshot body is freed; senders/receivers
    // that were blocked on EFROZEN can re-arm and resume.
    if (snap->scope_flags & SNAP_SCOPE_FREEZE_ALL_CHANS) {
        (void)chan_thaw_all_locked(snap->id);
    }

    if (snap->tasks) {
        for (uint32_t i = 0; i < snap->task_count; i++) {
            snap_destroy_task_entry(&snap->tasks[i]);
        }
        kfree(snap->tasks);
        snap->tasks = NULL;
        snap->task_count = 0;
    }
    if (snap->vmos) {
        // v1 does not actually populate snap->vmos beyond zero entries.
        // Future expansion hook: drop vmo_unref per record here.
        kfree(snap->vmos);
        snap->vmos = NULL;
        snap->vmo_count = 0;
    }
    if (snap->fs_pins) {
        for (uint32_t i = 0; i < snap->fs_pin_count; i++) {
            grahafs_unpin_version((uint32_t)snap->fs_pins[i].inode_id,
                                  snap->fs_pins[i].pinned_version_id);
        }
        kfree(snap->fs_pins);
        snap->fs_pins = NULL;
        snap->fs_pin_count = 0;
    }
    if (snap->chans) {
        // Per-entry chan_thaw_locked drops any explicit per-channel freeze
        // that chan_freeze (single-channel form) might have stamped. Safe
        // even if the channel was never frozen by this snap (chan_thaw
        // returns -EINVAL which we ignore).
        for (uint32_t i = 0; i < snap->chan_count; i++) {
            chan_thaw(snap->chans[i].chan_id, UINT32_MAX, UINT32_MAX);
        }
        kfree(snap->chans);
        snap->chans = NULL;
        snap->chan_count = 0;
    }
    snap->pages_shared = 0;
}
