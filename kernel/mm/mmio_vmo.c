// kernel/mm/mmio_vmo.c — Phase 21.

#include "mmio_vmo.h"
#include "vmo.h"
#include "kheap.h"
#include "slab.h"
#include "../sync/spinlock.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/pci_enum.h"
#include <string.h>

// We reuse the same slab cache vmo_init creates. Declared in vmo.c via a
// file-static; expose via this thin allocator that calls vmo_create with
// VMO_MMIO|VMO_PINNED. To avoid duplicating the slab cache, we let
// vmo_create do the kmem_cache_alloc but bypass its pmm_alloc_page loop by
// passing an unsupported size... actually easier: we manually allocate via
// the same path and patch pages[] post-hoc. To avoid touching vmo_create's
// internals, mmio_vmo_create allocates a tiny on-demand VMO (which skips
// the pmm loop), then overwrites pages[] with the BAR phys addresses and
// flips VMO_ONDEMAND off / VMO_MMIO|VMO_PINNED on. This is a bit ugly but
// keeps vmo.c stable.

pci_table_entry_t *mmio_vmo_validate_range(uint64_t phys, uint64_t size) {
    if (size == 0) return NULL;
    uint64_t end = phys + size;
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        pci_table_entry_t *e = &g_pci_table[i];
        for (int b = 0; b < 6; b++) {
            uint64_t bar_base = e->bars[b];
            uint64_t bar_size = e->bar_sizes[b];
            if (bar_size == 0) continue;
            uint64_t bar_end = bar_base + bar_size;
            if (phys >= bar_base && end <= bar_end) return e;
        }
    }
    return NULL;
}

vmo_t *mmio_vmo_create(uint64_t phys, uint64_t size, int32_t owner_pid) {
    if ((phys & 0xFFFu) || (size & 0xFFFu) || size == 0) return NULL;
    if (size > VMO_MAX_SIZE) return NULL;
    if (!mmio_vmo_validate_range(phys, size)) return NULL;

    // Create a small on-demand VMO so vmo_create skips its pmm_alloc loop.
    // Then overwrite pages[] with the BAR physical addresses and flip flags
    // to VMO_MMIO | VMO_PINNED.
    vmo_t *v = vmo_create(size, VMO_ONDEMAND, owner_pid, 0);
    if (!v) return NULL;
    uint32_t npages = (uint32_t)(size / 4096);
    for (uint32_t p = 0; p < npages; p++) {
        v->pages[p] = phys + (uint64_t)p * 4096;
    }
    // Replace the placeholder VMO_ONDEMAND with the real flags. VMO_PINNED
    // documents "do not evict / swap" (academic on x86_64 with no swap, but
    // the contract is what matters). VMO_MMIO drives the cache-disable map +
    // skip-pmm cleanup paths in vmo.c.
    v->flags = VMO_MMIO | VMO_PINNED;
    return v;
}
