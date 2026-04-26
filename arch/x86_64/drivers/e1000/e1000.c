// arch/x86_64/drivers/e1000/e1000.c
//
// Phase 21.1 — Stripped to ~70 LOC. The 551-line in-kernel E1000 driver
// has migrated to user/drivers/e1000d.c. What remains:
//
//   1. `e1000_init()` — boot-time PCI scan to confirm the device is on
//      the bus + a klog line. Idempotent; no MMIO mapping, no DMA, no
//      CAN registration (the CAN cap moved to e1000_proxy_init).
//
//   2. `e1000_expose_to_userdrv()` — flip the device's `is_claimable`
//      bit in the userdrv ownership table so the daemon can call
//      `sys_drv_register(0x8086, 0x100E, 0x02, &caps)`.
//
// The daemon (registered after init), through libdriver's drv_register
// path, takes ownership of the BAR, allocates DMA rings, programs RCTL/
// TCTL, enables IRQs, and pushes ANNOUNCE to the kernel-side proxy. The
// kernel knows nothing about descriptor rings or EEPROM after this strip.

#include "e1000.h"
#include "../../cpu/pci_enum.h"
#include "../../../../kernel/log.h"

void e1000_init(void) {
    pci_table_entry_t *e = pci_table_find_by_id(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (!e) {
        klog(KLOG_INFO, SUBSYS_DRV,
             "[E1000] not present on PCI bus (kernel will not load)");
        return;
    }
    // Phase 21.1 leaves the driver detection in place but does NOT enable
    // bus-mastering, map BAR0, or program the device. All of that is the
    // daemon's responsibility now (sys_drv_register grants the caps + the
    // BAR, then libdriver's drv_mmio_map maps it into daemon space, then
    // e1000d.c does the reset/EEPROM/ring-init dance).
    klog(KLOG_INFO, SUBSYS_DRV,
         "[E1000] kernel-side detect ok: pci=%lu:%lu.%lu vendor=0x%lx device=0x%lx bar0=0x%lx size=0x%lx irq=%lu",
         (unsigned long)e->bus, (unsigned long)e->device, (unsigned long)e->function,
         (unsigned long)e->vendor_id, (unsigned long)e->device_id,
         (unsigned long)e->bars[0], (unsigned long)e->bar_sizes[0],
         (unsigned long)e->irq_line);
}

void e1000_expose_to_userdrv(void) {
    extern int userdrv_mark_claimable(uint16_t vendor_id, uint16_t device_id);
    int rc = userdrv_mark_claimable(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (rc == 0) {
        klog(KLOG_INFO, SUBSYS_DRV,
             "[E1000] exposed to userdrv: vendor=0x%lx device=0x%lx (claimable)",
             (unsigned long)E1000_VENDOR_ID, (unsigned long)E1000_DEVICE_ID);
    } else {
        klog(KLOG_WARN, SUBSYS_DRV,
             "[E1000] expose failed: rc=%ld (device not enumerated?)",
             (long)rc);
    }
}
