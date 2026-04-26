// kernel/mm/mmio_vmo.h
//
// Phase 21 — MMIO VMO variant.
//
// `mmio_vmo_create(phys, size, owner_pid)` allocates a vmo_t whose backing
// pages[] are direct PCI BAR physical addresses (not RAM). The VMO carries
// VMO_MMIO | VMO_PINNED flags so:
//   - vmo_free skips pmm_page_unref (the pages aren't pmm-tracked).
//   - vmo_map forces PTE_CACHEDISABLE | PTE_WRITETHROUGH on every PTE.
//   - vmo_unmap skips pmm_page_unref + rlimit_account_free_mem.
//   - COW clone on an MMIO VMO is rejected (semantically meaningless).
//
// The allocator validates `phys` against `g_pci_table` so callers can only
// map regions that fall within an enumerated BAR. The caller (typically
// SYS_MMIO_VMO_CREATE) additionally checks that the PCI device is owned by
// the calling task before invoking this helper.
//
// Lifecycle: the returned vmo_t* is unwrapped — caller wraps it in a
// CAP_KIND_MMIO_REGION cap_object via cap_object_alloc + insert.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "vmo.h"

// Validate that [phys, phys+size) lies entirely within a single BAR of some
// enumerated PCI device. Returns the matching pci_table_entry_t* on success,
// NULL on failure. Used by sys_mmio_vmo_create's gate; exposed here so other
// kernel callers (e.g., e1000_proxy_init's optional BAR check) can reuse it.
struct pci_table_entry;
struct pci_table_entry *mmio_vmo_validate_range(uint64_t phys, uint64_t size);

// Create an MMIO-backed VMO. `phys` and `size` must be page-aligned;
// `owner_pid` becomes the VMO's audience. Caller is responsible for ALSO
// validating that the calling task owns the PCI device (sys_mmio_vmo_create
// does this); this helper only checks that the range fits in *some* BAR.
//
// Returns vmo_t* on success (with refcount=1), NULL on:
//   - phys/size not page aligned
//   - phys+size not within any single BAR
//   - kheap exhaustion
vmo_t *mmio_vmo_create(uint64_t phys, uint64_t size, int32_t owner_pid);
