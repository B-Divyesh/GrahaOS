// arch/x86_64/drivers/e1000/e1000.h
//
// Phase 21.1 — Stripped header. The full register/descriptor definitions
// (lifted from this file plus the kernel driver's send/receive logic) now
// live in user/drivers/e1000d.h and user/drivers/e1000d.c. The kernel side
// keeps only the PCI vendor/device identification and the
// `e1000_expose_to_userdrv` hook that flags the device as claimable in the
// userdrv ownership table at boot time.
//
// All five legacy "device API" entry points (`e1000_send`, `e1000_receive`,
// `e1000_get_mac`, `e1000_link_up`, `e1000_is_present`) plus the CAN
// activate/deactivate callbacks have moved into kernel/net/e1000_proxy.{h,c}
// where they call out to the userspace daemon over the channels Phase 21
// substrate hands back from `sys_drv_register`.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// PCI identification (kept here so userdrv ownership lookup and the
// daemon-side e1000d.h can both reference one source of truth).
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E  // 82540EM (QEMU default)

// Kernel-side bring-up. Now a thin no-op that just confirms the PCI table
// has the device — the actual MMIO/DMA work happens entirely in
// /bin/e1000d. Kept as a function so the kmain call site (Phase 9a) can
// remain in place while Phase 21.1 settles.
void e1000_init(void);

// Phase 21.1: tag the e1000 entry in the userdrv ownership table as
// claimable so a daemon may call sys_drv_register on it. Looked up by
// (E1000_VENDOR_ID, E1000_DEVICE_ID) — first match wins. No-op if the
// device wasn't enumerated.
void e1000_expose_to_userdrv(void);
