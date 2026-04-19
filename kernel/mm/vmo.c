// kernel/mm/vmo.c — Phase 17.
#include "vmo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kheap.h"
#include "slab.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../cap/token.h"
#include "../cap/object.h"
#include "../audit.h"
#include "../log.h"

// --- Global state --------------------------------------------------------
static kmem_cache_t *g_vmo_cache = NULL;
static uint64_t g_next_vmo_id = 1;
static spinlock_t g_vmo_id_lock = SPINLOCK_INITIALIZER("vmo_id");

// Per-task mapping table. Indexed by task_id (max 64 tasks in MVP, matches
// MAX_TASKS in sched.h). Each task has up to VMO_MAPPINGS_PER_TASK slots.
// A slot with vaddr==0 is free.
#define VMO_MAX_TASKS 64
static vmo_mapping_t g_vmo_task_maps[VMO_MAX_TASKS][VMO_MAPPINGS_PER_TASK];
static spinlock_t g_vmo_map_lock = SPINLOCK_INITIALIZER("vmo_map");

static uint64_t next_vmo_id(void) {
    spinlock_acquire(&g_vmo_id_lock);
    uint64_t id = g_next_vmo_id++;
    spinlock_release(&g_vmo_id_lock);
    return id;
}

void vmo_init(void) {
    g_vmo_cache = kmem_cache_create("vmo_t", sizeof(vmo_t), _Alignof(vmo_t),
                                     NULL, SUBSYS_CAP);
    if (!g_vmo_cache) {
        klog(KLOG_FATAL, SUBSYS_MM, "vmo_init: kmem_cache_create failed");
        return;
    }
    // Zero the per-task mapping table.
    for (int t = 0; t < VMO_MAX_TASKS; t++)
        for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++)
            g_vmo_task_maps[t][s].vaddr = 0;
    // Install the page-fault hook.
    vmm_install_pf_handler(vmo_pf_dispatch);
    klog(KLOG_INFO, SUBSYS_MM, "vmo_init: slab + pf hook ready");
}

// --- Small helpers -------------------------------------------------------
static inline void *phys_to_kv(uint64_t phys) {
    return (void *)(phys + g_hhdm_offset);
}

// Validate and normalize a VMO handle. Returns true on good, sets *out.
static bool vmo_check(vmo_t *v) {
    return v && v->magic == VMO_MAGIC && v->npages > 0;
}

// --- vmo_create ----------------------------------------------------------
vmo_t *vmo_create(uint64_t size_bytes, uint32_t flags,
                  int32_t owner_pid, int32_t audience_pid) {
    (void)audience_pid;  // audience is applied at cap_object wrap time.
    if (size_bytes == 0 || (size_bytes & 0xFFFu) != 0) return NULL;
    if (size_bytes > VMO_MAX_SIZE) return NULL;

    uint64_t npages = size_bytes / 4096;

    vmo_t *v = (vmo_t *)kmem_cache_alloc(g_vmo_cache);
    if (!v) return NULL;
    v->magic       = VMO_MAGIC;
    v->flags       = flags;
    v->id          = next_vmo_id();
    v->size_bytes  = size_bytes;
    v->npages      = (uint32_t)npages;
    v->refcount    = 1;
    v->parent      = NULL;
    v->owner_pid   = owner_pid;
    v->cap_object_idx = 0;
    spinlock_init(&v->lock, "vmo");

    v->pages = (uint64_t *)kmalloc(sizeof(uint64_t) * npages, SUBSYS_MM);
    if (!v->pages) {
        kmem_cache_free(g_vmo_cache, v);
        return NULL;
    }
    memset(v->pages, 0, sizeof(uint64_t) * npages);

    // Eager allocation unless VMO_ONDEMAND is set.
    if (!(flags & VMO_ONDEMAND)) {
        for (uint64_t p = 0; p < npages; p++) {
            void *pa = pmm_alloc_page();
            if (!pa) {
                // Roll back all previously allocated pages.
                for (uint64_t q = 0; q < p; q++) {
                    if (v->pages[q])
                        pmm_page_unref((void *)v->pages[q]);
                }
                kfree(v->pages);
                kmem_cache_free(g_vmo_cache, v);
                return NULL;
            }
            v->pages[p] = (uint64_t)pa;
            if (flags & VMO_ZEROED) memset(phys_to_kv(v->pages[p]), 0, 4096);
        }
    }
    return v;
}

// --- vmo_free ------------------------------------------------------------
void vmo_free(vmo_t *v) {
    if (!vmo_check(v)) return;
    // Drop parent reference if COW child.
    if (v->parent) {
        vmo_unref(v->parent);
        v->parent = NULL;
    }
    if (v->pages) {
        for (uint32_t p = 0; p < v->npages; p++) {
            if (v->pages[p]) pmm_page_unref((void *)v->pages[p]);
        }
        kfree(v->pages);
        v->pages = NULL;
    }
    v->magic = 0;  // poison
    kmem_cache_free(g_vmo_cache, v);
}

void vmo_ref(vmo_t *v) {
    if (!vmo_check(v)) return;
    spinlock_acquire(&v->lock);
    v->refcount++;
    spinlock_release(&v->lock);
}

void vmo_unref(vmo_t *v) {
    if (!vmo_check(v)) return;
    spinlock_acquire(&v->lock);
    bool zero = (--v->refcount == 0);
    spinlock_release(&v->lock);
    if (zero) vmo_free(v);
}

// --- Per-task mapping helpers --------------------------------------------
static int vmo_alloc_map_slot(int32_t task_id) {
    if (task_id < 0 || task_id >= VMO_MAX_TASKS) return -1;
    for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++) {
        if (g_vmo_task_maps[task_id][s].vaddr == 0) return s;
    }
    return -1;
}

static int vmo_find_map_slot(int32_t task_id, uint64_t vaddr) {
    if (task_id < 0 || task_id >= VMO_MAX_TASKS) return -1;
    for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++) {
        if (g_vmo_task_maps[task_id][s].vaddr == vaddr &&
            g_vmo_task_maps[task_id][s].vaddr != 0) return s;
    }
    return -1;
}

// Find the mapping slot that contains the given VA (any page in range).
static int vmo_find_map_slot_containing(int32_t task_id, uint64_t va) {
    if (task_id < 0 || task_id >= VMO_MAX_TASKS) return -1;
    for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++) {
        vmo_mapping_t *m = &g_vmo_task_maps[task_id][s];
        if (m->vaddr == 0) continue;
        uint64_t end = m->vaddr + ((uint64_t)m->len_pages * 4096);
        if (va >= m->vaddr && va < end) return s;
    }
    return -1;
}

static uint64_t prot_to_pte_flags(uint32_t prot) {
    uint64_t f = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) f |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) f |= PTE_NX;
    return f;
}

// --- vmo_map -------------------------------------------------------------
uint64_t vmo_map(vmo_t *v, task_t *t,
                 uint64_t addr_hint, uint64_t offset, uint64_t len,
                 uint32_t prot) {
    if (!vmo_check(v) || !t) return 0;
    if ((offset & 0xFFFu) || (len & 0xFFFu) || len == 0) return 0;
    if (offset + len > v->size_bytes) return 0;
    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) return 0;

    uint64_t start_page = offset / 4096;
    uint64_t npages = len / 4096;

    spinlock_acquire(&g_vmo_map_lock);
    int slot = vmo_alloc_map_slot(t->id);
    if (slot < 0) { spinlock_release(&g_vmo_map_lock); return 0; }

    uint64_t vaddr = addr_hint ? addr_hint : vmm_reserve_va_by_cr3(t->cr3, len);
    if (!vaddr) { spinlock_release(&g_vmo_map_lock); return 0; }

    uint64_t pte_flags = prot_to_pte_flags(prot);

    // If this is a COW child VMO and prot requested write, map read-only
    // initially so first write faults through vmo_pf_dispatch.
    if ((v->flags & VMO_COW_CHILD) && (prot & PROT_WRITE)) {
        pte_flags &= ~PTE_WRITABLE;
    }

    for (uint64_t p = 0; p < npages; p++) {
        uint64_t phys = v->pages[start_page + p];
        if (phys == 0) {
            // On-demand page — allocate now. (Simple eager-on-first-map for
            // Phase 17; true demand-paging can land in Phase 18.)
            void *pa = pmm_alloc_page();
            if (!pa) {
                // Roll back the partial map.
                for (uint64_t q = 0; q < p; q++) {
                    vmm_unmap_page_by_cr3(t->cr3, vaddr + q * 4096);
                    pmm_page_unref((void *)(v->pages[start_page + q]));
                }
                spinlock_release(&g_vmo_map_lock);
                return 0;
            }
            v->pages[start_page + p] = (uint64_t)pa;
            if (v->flags & VMO_ZEROED) memset(phys_to_kv((uint64_t)pa), 0, 4096);
            phys = (uint64_t)pa;
        } else {
            // Page already backs the VMO — bump its refcount because the
            // mapping itself is a new "owner" distinct from the vmo's
            // original allocation-time reference.
            pmm_page_ref((void *)phys);
        }

        if (!vmm_map_page_by_cr3(t->cr3, vaddr + p * 4096, phys, pte_flags)) {
            for (uint64_t q = 0; q <= p; q++) {
                vmm_unmap_page_by_cr3(t->cr3, vaddr + q * 4096);
                pmm_page_unref((void *)(v->pages[start_page + q]));
            }
            spinlock_release(&g_vmo_map_lock);
            return 0;
        }
    }

    // Record the mapping and reference the vmo.
    g_vmo_task_maps[t->id][slot].vaddr     = vaddr;
    g_vmo_task_maps[t->id][slot].vmo       = v;
    g_vmo_task_maps[t->id][slot].offset    = offset;
    g_vmo_task_maps[t->id][slot].len_pages = (uint32_t)npages;
    g_vmo_task_maps[t->id][slot].prot      = prot;
    vmo_ref(v);
    spinlock_release(&g_vmo_map_lock);
    return vaddr;
}

// --- vmo_unmap -----------------------------------------------------------
int vmo_unmap(task_t *t, uint64_t vaddr, uint64_t len) {
    if (!t || vaddr == 0 || (vaddr & 0xFFFu) || (len & 0xFFFu)) return CAP_V2_EINVAL;
    spinlock_acquire(&g_vmo_map_lock);
    int slot = vmo_find_map_slot(t->id, vaddr);
    if (slot < 0) { spinlock_release(&g_vmo_map_lock); return CAP_V2_EINVAL; }
    vmo_mapping_t *m = &g_vmo_task_maps[t->id][slot];
    if ((uint64_t)m->len_pages * 4096 != len) {
        spinlock_release(&g_vmo_map_lock);
        return CAP_V2_EINVAL;
    }
    vmo_t *v = m->vmo;
    uint64_t start_page = m->offset / 4096;
    for (uint32_t p = 0; p < m->len_pages; p++) {
        uint64_t va = m->vaddr + (uint64_t)p * 4096;
        uint64_t phys = v->pages[start_page + p];
        vmm_unmap_page_by_cr3(t->cr3, va);
        if (phys) pmm_page_unref((void *)phys);
    }
    m->vaddr = 0;
    m->vmo   = NULL;
    spinlock_release(&g_vmo_map_lock);
    vmo_unref(v);
    return 0;
}

// --- vmo_clone_cow -------------------------------------------------------
vmo_t *vmo_clone_cow(vmo_t *src, int32_t owner_pid) {
    if (!vmo_check(src)) return NULL;

    vmo_t *child = (vmo_t *)kmem_cache_alloc(g_vmo_cache);
    if (!child) return NULL;
    child->magic       = VMO_MAGIC;
    child->flags       = (src->flags | VMO_COW_CHILD) & ~VMO_ZEROED;
    child->id          = next_vmo_id();
    child->size_bytes  = src->size_bytes;
    child->npages      = src->npages;
    child->refcount    = 1;
    child->parent      = src;
    child->owner_pid   = owner_pid;
    child->cap_object_idx = 0;
    spinlock_init(&child->lock, "vmo");

    child->pages = (uint64_t *)kmalloc(sizeof(uint64_t) * src->npages, SUBSYS_MM);
    if (!child->pages) {
        kmem_cache_free(g_vmo_cache, child);
        return NULL;
    }

    // Share pages and bump refcounts. Mark all currently mapped pages of
    // src as read-only in every existing task mapping so the first write
    // faults through vmo_pf_dispatch.
    spinlock_acquire(&src->lock);
    for (uint32_t p = 0; p < src->npages; p++) {
        child->pages[p] = src->pages[p];
        if (child->pages[p]) pmm_page_ref((void *)child->pages[p]);
    }
    src->refcount++;  // parent gets a reference from the child
    spinlock_release(&src->lock);

    // Downgrade every mapping of src (in any task) to read-only.
    spinlock_acquire(&g_vmo_map_lock);
    for (int t = 0; t < VMO_MAX_TASKS; t++) {
        for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++) {
            vmo_mapping_t *m = &g_vmo_task_maps[t][s];
            if (m->vmo != src || m->vaddr == 0) continue;
            uint64_t cr3 = 0;
            task_t *tt = sched_get_task(t);
            if (tt) cr3 = tt->cr3;
            if (!cr3) continue;
            uint64_t flags_ro = prot_to_pte_flags(m->prot) & ~PTE_WRITABLE;
            for (uint32_t p = 0; p < m->len_pages; p++) {
                vmm_protect_page_by_cr3(cr3, m->vaddr + (uint64_t)p * 4096, flags_ro);
            }
        }
    }
    spinlock_release(&g_vmo_map_lock);
    return child;
}

// --- vmo_pf_dispatch -----------------------------------------------------
int vmo_pf_dispatch(uint64_t fault_va, uint64_t error_code) {
    // We only handle user-mode write faults on present pages.
    // error_code bit 0 = present, bit 1 = write, bit 2 = user.
    bool is_user = (error_code & 0x4) != 0;
    bool is_write = (error_code & 0x2) != 0;
    bool is_present = (error_code & 0x1) != 0;
    if (!is_user || !is_write || !is_present) return -1;

    task_t *cur = sched_get_current_task();
    if (!cur) return -1;

    spinlock_acquire(&g_vmo_map_lock);
    int slot = vmo_find_map_slot_containing(cur->id, fault_va & ~0xFFFull);
    if (slot < 0) { spinlock_release(&g_vmo_map_lock); return -1; }
    vmo_mapping_t *m = &g_vmo_task_maps[cur->id][slot];
    if (!(m->prot & PROT_WRITE)) {
        // Handle had no write right — audit and deny.
        uint32_t obj = m->vmo ? m->vmo->cap_object_idx : 0;
        spinlock_release(&g_vmo_map_lock);
        audit_write_vmo_fault(cur->id, obj, fault_va,
                              CAP_V2_EPERM, "write denied (RO mapping)");
        return -1;  // Let the generic handler kill the task
    }
    vmo_t *v = m->vmo;
    if (!vmo_check(v) || !(v->flags & VMO_COW_CHILD)) {
        spinlock_release(&g_vmo_map_lock);
        return -1;
    }

    uint64_t page_va   = fault_va & ~0xFFFull;
    uint64_t off_bytes = page_va - m->vaddr;
    uint32_t page_idx  = (uint32_t)((m->offset + off_bytes) / 4096);
    if (page_idx >= v->npages) {
        spinlock_release(&g_vmo_map_lock);
        return -1;
    }
    uint64_t old_phys = v->pages[page_idx];
    if (!old_phys) {
        spinlock_release(&g_vmo_map_lock);
        return -1;
    }

    // Allocate a private copy.
    void *new_pa = pmm_alloc_page();
    if (!new_pa) {
        spinlock_release(&g_vmo_map_lock);
        audit_write_vmo_fault(cur->id, v->cap_object_idx, fault_va,
                              CAP_V2_ENOMEM, "cow alloc failed");
        return -1;
    }
    memcpy(phys_to_kv((uint64_t)new_pa), phys_to_kv(old_phys), 4096);

    // Re-map with write-enabled pointing at the fresh frame.
    uint64_t flags = prot_to_pte_flags(m->prot);
    // Unmap the old PTE first (this will pmm_page_unref the shared frame).
    vmm_unmap_page_by_cr3(cur->cr3, page_va);
    pmm_page_unref((void *)old_phys);

    vmm_map_page_by_cr3(cur->cr3, page_va, (uint64_t)new_pa, flags);
    v->pages[page_idx] = (uint64_t)new_pa;  // child now owns this frame

    spinlock_release(&g_vmo_map_lock);
    audit_write_vmo_fault(cur->id, v->cap_object_idx, fault_va, 0,
                          "cow copy-on-write satisfied");
    return 0;
}

// --- cap_object integration ---------------------------------------------
void vmo_cap_deactivate(cap_object_t *obj) {
    if (!obj) return;
    vmo_t *v = (vmo_t *)obj->kind_data;
    if (!vmo_check(v)) return;
    vmo_unref(v);
}

// Called from sched_reap_zombie to release any mappings the exiting task
// still holds. Prevents handle leaks across process death.
void vmo_cleanup_task(int32_t task_id) {
    if (task_id < 0 || task_id >= VMO_MAX_TASKS) return;
    spinlock_acquire(&g_vmo_map_lock);
    for (int s = 0; s < VMO_MAPPINGS_PER_TASK; s++) {
        vmo_mapping_t *m = &g_vmo_task_maps[task_id][s];
        if (m->vaddr == 0) continue;
        vmo_t *v = m->vmo;
        // PTEs are torn down by vmm_destroy_address_space_by_cr3; we just
        // release refcount accounting.
        for (uint32_t p = 0; p < m->len_pages; p++) {
            if (v && v->pages) {
                uint64_t start_page = m->offset / 4096;
                uint64_t phys = v->pages[start_page + p];
                if (phys) pmm_page_unref((void *)phys);
            }
        }
        m->vaddr = 0;
        m->vmo = NULL;
        if (v) {
            spinlock_release(&g_vmo_map_lock);
            vmo_unref(v);
            spinlock_acquire(&g_vmo_map_lock);
        }
    }
    spinlock_release(&g_vmo_map_lock);
}
