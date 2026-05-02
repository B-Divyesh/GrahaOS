// kernel/snap/cow_fault.c
// Phase 24 W15: COW page-fault handler for snapshot-shared pages.
//
// When snap_create captures a task's user-half PTEs (W14.4, deferred), each
// shared physical page is registered in a per-phys hash table — the
// cow_page_tracker_t — and both the parent's PTE and the snapshot's PTE are
// flipped to read-only. A subsequent write from the parent triggers a #PF
// (error_code = present | write); the chained handler vmm_dispatch_pf calls
// cow_fault_handle, which:
//
//   1. Walks the faulting PTE in the current task's PML4, extracts the
//      physical page, and looks up its cow_page_tracker_t.
//   2. If no tracker is found, returns -1 so vmo_pf_dispatch (Phase 17 VMO
//      COW) gets a shot at the same fault.
//   3. If refcount == 1 (the snapshot is gone, only this task still holds
//      the page), removes the tracker and just promotes the PTE writable
//      in-place — no copy.
//   4. If refcount > 1, allocates a private page, memcpys the contents
//      through the HHDM, installs a writable PTE pointing at the private
//      page, drops the old page's pmm refcount, and decrements the
//      tracker.
//
// The registered handler is tried BEFORE vmo_pf_dispatch (snap COW is the
// faster path for snapshot-pinned pages, and getting the order right keeps
// the VMO COW handler from accidentally allocating a second private page
// when the snap COW handler would have resolved it via promote-in-place).
//
// Lookups under a single global spinlock with 256 hash buckets. Per-CPU
// caches are deferred until measurement shows lookup contention (the plan
// flags this as W15 future work).
//
// Test harness: the hash table primitives + handler chain land here in W15;
// the actual fault is exercised once W14.4 (PML4 deep walk) marks pages RO
// during snap_create. snaptest 7/7 already exercises the lifecycle scaffold
// without the capture; once W14.4 lands, write-after-snap will hit
// cow_fault_handle automatically.

#include "snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mm/slab.h"
#include "../sync/spinlock.h"
#include "../log.h"

#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

// ---------------------------------------------------------------------------
// Hash table for cow_page_tracker_t lookups, keyed on (phys >> 12) & MASK.
// ---------------------------------------------------------------------------
#define COW_HASH_BUCKETS  256u
#define COW_HASH_MASK     (COW_HASH_BUCKETS - 1u)

static cow_page_tracker_t *g_cow_buckets[COW_HASH_BUCKETS];
static spinlock_t          g_cow_lock = SPINLOCK_INITIALIZER("cow_track");
static kmem_cache_t       *g_cow_tracker_cache;

// Fast path: when this counter is 0, NO snapshot is tracking any page and
// snap_cow_pf_dispatch can short-circuit without walking the faulting PTE
// or acquiring g_cow_lock. Bumped on tracker insert, decremented on
// tracker reap. Read with __ATOMIC_RELAXED — false negatives (count
// momentarily looks 0 mid-insert) just send the fault through the slow
// path that re-checks under the lock.
static volatile uint32_t g_cow_tracker_count = 0;

// ---------------------------------------------------------------------------
// Forward declaration: registered with vmm via vmm_install_snap_pf_handler.
// ---------------------------------------------------------------------------
static int snap_cow_pf_dispatch(uint64_t fault_va, uint64_t error_code);

// ---------------------------------------------------------------------------
// cow_init — one-time bootstrap. Called from snap_init.
// ---------------------------------------------------------------------------
void cow_init(void) {
    if (g_cow_tracker_cache) return;  // idempotent

    g_cow_tracker_cache = kmem_cache_create("cow_page_tracker_t",
                                            sizeof(cow_page_tracker_t),
                                            _Alignof(cow_page_tracker_t),
                                            /*ctor=*/NULL,
                                            SUBSYS_MM);
    if (!g_cow_tracker_cache) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "cow_init: kmem_cache_create(cow_page_tracker_t) failed");
        return;
    }

    for (uint32_t i = 0; i < COW_HASH_BUCKETS; i++) {
        g_cow_buckets[i] = NULL;
    }

    vmm_install_snap_pf_handler(snap_cow_pf_dispatch);
    klog(KLOG_INFO, SUBSYS_MM,
         "cow_init: %u-bucket hash + snap PF handler installed",
         (unsigned)COW_HASH_BUCKETS);
}

// ---------------------------------------------------------------------------
// Lookup helpers (caller must hold g_cow_lock).
// ---------------------------------------------------------------------------
static inline uint32_t cow_hash(uint64_t phys) {
    return (uint32_t)((phys >> 12) & COW_HASH_MASK);
}

static cow_page_tracker_t *cow_lookup_locked(uint64_t phys) {
    cow_page_tracker_t *t = g_cow_buckets[cow_hash(phys)];
    while (t) {
        if (t->phys_page == phys) return t;
        t = t->next;
    }
    return NULL;
}

static void cow_unlink_locked(cow_page_tracker_t *t) {
    cow_page_tracker_t **slot = &g_cow_buckets[cow_hash(t->phys_page)];
    while (*slot) {
        if (*slot == t) {
            *slot = t->next;
            t->next = NULL;
            return;
        }
        slot = &(*slot)->next;
    }
}

// ---------------------------------------------------------------------------
// Public tracker API (declared in snapshot.h).
// ---------------------------------------------------------------------------
//
// cow_page_tracker_get: lookup + lazy-create + bump refcount. Returns NULL
// if the slab cache is missing or kmem_cache_alloc fails. The returned
// pointer is owned by the hash table; callers do NOT free it.
//
cow_page_tracker_t *cow_page_tracker_get(uint64_t phys) {
    if (!g_cow_tracker_cache) return NULL;
    phys &= PAGE_MASK;
    if (!phys) return NULL;

    spinlock_acquire(&g_cow_lock);
    cow_page_tracker_t *t = cow_lookup_locked(phys);
    if (t) {
        // Saturate at UINT32_MAX-1 to leave headroom for future +1 ops.
        if (t->refcount < 0xFFFFFFFFu) t->refcount++;
        spinlock_release(&g_cow_lock);
        return t;
    }
    spinlock_release(&g_cow_lock);

    // Drop the lock to call the slab allocator (kmem_cache_alloc may take
    // its own spinlock; hold time of g_cow_lock should stay short).
    cow_page_tracker_t *fresh =
        (cow_page_tracker_t *)kmem_cache_alloc(g_cow_tracker_cache);
    if (!fresh) return NULL;
    fresh->phys_page     = phys;
    fresh->refcount      = 1;
    fresh->owner_snap_id = 0;
    fresh->next          = NULL;

    spinlock_acquire(&g_cow_lock);
    // Re-check: someone else could have inserted the same phys while we
    // were unlocked.
    cow_page_tracker_t *winner = cow_lookup_locked(phys);
    if (winner) {
        if (winner->refcount < 0xFFFFFFFFu) winner->refcount++;
        spinlock_release(&g_cow_lock);
        kmem_cache_free(g_cow_tracker_cache, fresh);
        return winner;
    }
    fresh->next = g_cow_buckets[cow_hash(phys)];
    g_cow_buckets[cow_hash(phys)] = fresh;
    __atomic_fetch_add(&g_cow_tracker_count, 1, __ATOMIC_RELAXED);
    spinlock_release(&g_cow_lock);
    return fresh;
}

void cow_page_tracker_bump(uint64_t phys) {
    (void)cow_page_tracker_get(phys);
}

void cow_page_tracker_put(uint64_t phys) {
    if (!g_cow_tracker_cache) return;
    phys &= PAGE_MASK;
    if (!phys) return;

    spinlock_acquire(&g_cow_lock);
    cow_page_tracker_t *t = cow_lookup_locked(phys);
    if (!t) {
        spinlock_release(&g_cow_lock);
        return;
    }
    if (t->refcount > 0) t->refcount--;
    bool reap = (t->refcount == 0);
    if (reap) {
        cow_unlink_locked(t);
        __atomic_fetch_sub(&g_cow_tracker_count, 1, __ATOMIC_RELAXED);
    }
    spinlock_release(&g_cow_lock);

    if (reap) kmem_cache_free(g_cow_tracker_cache, t);
}

// ---------------------------------------------------------------------------
// snap_cow_pf_dispatch — registered with vmm_install_snap_pf_handler.
//
// Returns 0 if the fault was resolved (caller resumes); -1 otherwise so
// vmm_dispatch_pf falls through to vmo_pf_dispatch.
// ---------------------------------------------------------------------------
static int snap_cow_pf_dispatch(uint64_t fault_va, uint64_t error_code) {
    // Fast path: when no snapshot is tracking any page, every fault here
    // is going to fall through to vmo_pf_dispatch anyway. Skip the PTE
    // walk + lock acquire to keep the snap handler at single-load cost
    // for the steady-state workload. Reading g_cow_tracker_count with
    // __ATOMIC_RELAXED is fine — a stale 0 just sends a real snap fault
    // through the slow path one extra cycle later, which is harmless.
    if (__atomic_load_n(&g_cow_tracker_count, __ATOMIC_RELAXED) == 0)
        return -1;

    // Snap COW only triggers on writes to present pages; non-present and
    // read faults aren't ours.
    bool is_write   = (error_code & 0x2) != 0;
    bool is_present = (error_code & 0x1) != 0;
    if (!is_write || !is_present) return -1;

    // Use the bridge form so this file doesn't need to know about
    // struct interrupt_frame (snapshot.h's prototype has it but we never
    // dereference it here — pass NULL). Calling cow_fault_handle keeps
    // any future inline-handling path centralised.
    return cow_fault_handle(fault_va, error_code, /*regs=*/NULL);
}

// ---------------------------------------------------------------------------
// cow_fault_handle — declared in snapshot.h. Resolves a write fault on a
// snap-tracked RO page by either promoting in place (refcount==1) or
// allocating a private copy (refcount>1).
// ---------------------------------------------------------------------------
int cow_fault_handle(uint64_t fault_va, uint64_t error_code,
                     struct interrupt_frame *regs) {
    (void)regs;
    bool is_write   = (error_code & 0x2) != 0;
    bool is_present = (error_code & 0x1) != 0;
    if (!is_write || !is_present) return -1;

    task_t *cur = sched_get_current_task();
    if (!cur) return -1;

    uint64_t page_va = fault_va & PAGE_MASK;
    uint64_t pte = vmm_get_pte(cur->cr3, page_va);
    if (!pte) return -1;
    if (!(pte & PTE_USER)) return -1;  // kernel-only mapping; not snap-COW
    if (pte & PTE_WRITABLE) return -1; // already writable; spurious

    // CRITICAL: PAGE_MASK in vmm.h is 0xFFFFFFFFFFFFF000 — that includes
    // bits 52-63 which on x86_64 carry protection keys (52-58) and NX
    // (63). For data pages NX is set, so masking the PTE with PAGE_MASK
    // would leave bit 63 set in old_phys; the subsequent HHDM addition
    // (old_phys + 0xFFFF800000000000) overflows into a non-canonical
    // virtual address and the next memcpy through HHDM #GPs.
    //
    // Use the physical-address-only mask: bits 12..51 (40-bit phys cap
    // matches QEMU's TCG x86_64 default, which is what we run under).
    const uint64_t PHYS_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
    uint64_t old_phys  = pte & PHYS_ADDR_MASK;
    uint64_t old_flags = pte & ~PHYS_ADDR_MASK;

    spinlock_acquire(&g_cow_lock);
    cow_page_tracker_t *t = cow_lookup_locked(old_phys);
    spinlock_release(&g_cow_lock);

    if (!t) return -1;

    // Phase 24 v1: always COW. The W14.4 page-table cloning ("clone the
    // user-half PML4 so the snapshot has its own PTEs") is deferred —
    // v1 stores cr3_snapshot == cr3_original and relies on the captured
    // (virt, phys, flags) list to know what to restore later. That means
    // the caller's PTE IS the only PTE that points at this phys, and
    // promoting it in place would change content visible through the
    // snapshot's captured_pages entry. Allocate a fresh page for the
    // caller, copy contents, and leave the tracker intact so
    // snap_delete can reclaim everything via cow_page_tracker_put.
    //
    // The tracker refcount is NOT decremented here. snap_create's
    // initial bump represents the snapshot's claim on this phys; the
    // snapshot does not lose its claim because the parent COW'd.
    void *new_phys_v = pmm_alloc_page();
    if (!new_phys_v) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "cow_fault: pmm_alloc_page failed");
        return -1;
    }
    uint64_t new_phys = (uint64_t)new_phys_v;

    // Copy through the higher-half direct map; user mode has no kernel
    // pointer for either side here.
    memcpy((void *)(new_phys + g_hhdm_offset),
           (void *)(old_phys + g_hhdm_offset),
           PAGE_SIZE);

    // Replace the PTE: drop the old phys pmm ref (the snapshot still
    // holds its ref via snap_create's pmm_page_ref bump, so old_phys
    // stays alive). Install new phys with PTE_WRITABLE re-applied.
    vmm_unmap_page_by_cr3(cur->cr3, page_va);
    pmm_page_unref((void *)old_phys);
    if (!vmm_map_page_by_cr3(cur->cr3, page_va, new_phys,
                             old_flags | PTE_WRITABLE)) {
        klog(KLOG_ERROR, SUBSYS_MM,
             "cow_fault: vmm_map_page_by_cr3 failed va=0x%lx new_phys=0x%lx",
             (unsigned long)page_va, (unsigned long)new_phys);
        pmm_free_page(new_phys_v);
        return -1;
    }
    return 0;
}
