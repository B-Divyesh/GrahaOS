// user/libdriver.h
//
// Phase 21 — userspace driver helper library.
//
// Linked into every driver daemon (`/bin/e1000d` and future siblings). Wraps
// the three new Phase 21 syscalls in idiomatic C plus a few convenience
// helpers (DMA buffer allocation via VMO_CONTIGUOUS, pledge self-check).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"

// drv_register: claim a PCI device for this daemon. Returns 0 on success +
// fills *out, or negative errno (-ENODEV / -EBUSY / -EPLEDGE / -EFAULT /
// -ENOMEM). On success, the caps in *out are usable for subsequent calls.
int drv_register(uint16_t vendor_id, uint16_t device_id,
                 uint8_t device_class, drv_caps_t *out);

// drv_irq_wait: block (or poll if timeout_ms == 0) for IRQ messages.
// Returns the count copied (0..max_msgs), 0 on timeout, or negative errno.
long drv_irq_wait(uint64_t irq_handle, drv_irq_msg_t *out_msgs,
                  uint32_t max_msgs, uint32_t timeout_ms);

// drv_mmio_map: convenience wrapper around SYS_MMIO_VMO_CREATE +
// SYS_VMO_MAP. Maps a BAR-region physical range into the daemon's address
// space. Returns the user virtual address, or 0 on failure.
void *drv_mmio_map(uint64_t phys, uint64_t size, uint32_t prot);

// drv_dma_alloc: allocate `npages` contiguous physical pages backed by a
// VMO_CONTIGUOUS VMO and map them into the daemon's address space.
// Writes the physical base of the allocation to *phys_out (so the daemon
// can program device DMA descriptors). Returns the mapped user virtual
// address, or 0 on failure.
//
// Capped at 64 pages per call (matches kernel `pmm_alloc_contiguous` limit).
// For larger DMA regions, allocate multiple VMOs.
void *drv_dma_alloc(uint32_t npages, uint64_t *phys_out);

// drv_dma_alloc_ex: like drv_dma_alloc but also surfaces the underlying
// VMO handle to the caller via *handle_out. Used by Phase 22 e1000d to
// obtain the handles for the rx/tx ring VMOs so it can `syscall_vmo_clone`
// them (VMO_CLONE_SHARED) and hand the clones to netd via a channel
// message — the zero-copy producer/consumer pattern for network frames.
//
// handle_out may be NULL for callers that don't need the handle (same as
// drv_dma_alloc). Returned VA + phys semantics match drv_dma_alloc.
void *drv_dma_alloc_ex(uint32_t npages, uint64_t *phys_out,
                       cap_token_u_t *handle_out);

// drv_self_pledge_check: assert that this daemon was spawned with the
// required pledge classes. If any required bit is missing, the function
// writes an error message to stderr/klog and exits with code 99 (so init
// can see "wrong-pledge child" in its respawn log).
void drv_self_pledge_check(uint16_t required_pledge);
