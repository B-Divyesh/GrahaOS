// arch/x86_64/drivers/ahci/ahci.c — Phase 24a W10 strip.
//
// Pre-strip this file was 633 LOC of kernel-resident SATA driver: PRDT setup,
// ATA command FIS, port_rebase + per-slot bookkeeping, ahci_read / ahci_write
// + ahci_flush_cache + VFS adapters, plus CAN activate / deactivate / refuse
// hooks.  After Phase 24a closeout (channel-mode FS validated end-to-end via
// /bin/ahcid + W1 doorbell IPI + W2 same-CPU yield + W3 batching + W5 SPSC
// ring), the kernel-direct fallback is dead code.
//
// What survived:
//   1. ahci_init                — PCI scan, BAR5 map, BIOS hand-off, GHC.AE,
//                                 walk implemented ports, snapshot PxCLB/PxFB
//                                 for L10.  No CAN registration.  No VFS
//                                 registration.  No port_rebase (ahcid does
//                                 it via SYS_DRV_REGISTER + DMA VMOs).
//   2. ahci_expose_to_userdrv   — mark the HBA claimable by /bin/ahcid.
//   3. ahci_restore_after_userdrv_death (L10 substrate) — re-point PxCLB/PxFB
//                                 to the kernel's saved values when ahcid
//                                 dies, so the HBA is left in a valid state
//                                 for the next ahcid spawn.
//
// Everything else lives in user/drivers/ahcid.c.  See specs/phase-24-cow-
// snapshots.yml::phase_23_completion_handoff for the cutover history.
#include "ahci.h"
#include "../../cpu/pci.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/sync/spinlock.h"
#include "../../../../kernel/log.h"

#define HBA_PORT_DEV_PRESENT 0x3
#define HBA_PORT_IPM_ACTIVE  0x1
#define SATA_SIG_ATA         0x00000101

#define HBA_PxCMD_ST  0x0001
#define HBA_PxCMD_FRE 0x0010
#define HBA_PxCMD_FR  0x4000
#define HBA_PxCMD_CR  0x8000

static ahci_hba_mem_t *hba_mem;
static ahci_port_t    *ports[32];
static spinlock_t      ahci_lock = SPINLOCK_INITIALIZER("ahci");

// Snapshot of each port's CLB/FB physical addresses.  L10 substrate
// (Phase 23 P23.deferred.2): captured at ahci_init when we first see a
// port present + ATA-signed; restored by ahci_restore_after_userdrv_death
// after a userspace ahcid daemon dies (its port_init overwrites these
// pointers with addresses from its own DMA VMOs, which are freed on exit).
// Without the restore, the HBA would DMA into unmapped memory on the
// next ahcid spawn or kernel-side recovery probe.
static uint64_t g_ahci_saved_clb[32];
static uint64_t g_ahci_saved_fb[32];

void ahci_init(void) {
    spinlock_init(&ahci_lock, "ahci");

    pci_device_t ahci_dev;
    if (!pci_scan_for_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, &ahci_dev)) {
        framebuffer_draw_string("AHCI: No SATA controller found.", 10, 600,
                                COLOR_YELLOW, 0x00101828);
        return;
    }

    uint64_t ahci_base_phys = ahci_dev.bar5 & 0xFFFFFFF0;
    vmm_map_page(vmm_get_kernel_space(),
                 (uint64_t)ahci_base_phys + g_hhdm_offset,
                 ahci_base_phys,
                 PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    hba_mem = (ahci_hba_mem_t *)(ahci_base_phys + g_hhdm_offset);

    // Take ownership from BIOS (BOHC handoff).
    if (hba_mem->bohc & (1u << 1)) {
        hba_mem->bohc |= (1u << 0);
        while (!(hba_mem->bohc & (1u << 0))) { /* spin */ }
    }

    // Enable AHCI mode (GHC.AE).
    hba_mem->ghc |= (1u << 31);

    // Walk implemented ports.  We don't program PxCLB/PxFB here — ahcid does
    // that when it claims the HBA via SYS_DRV_REGISTER.  We DO snapshot the
    // pre-existing PxCLB/PxFB so the L10 restore hook has something to fall
    // back to if ahcid dies before publishing /sys/blk/service.
    uint32_t ports_implemented = hba_mem->pi;
    for (int i = 0; i < 32; i++) {
        if (!(ports_implemented & (1u << i))) continue;
        ahci_port_t *p = (ahci_port_t *)((uint64_t)hba_mem + 0x100 + (i * 0x80));

        uint32_t ssts = p->ssts;
        uint8_t  det  = ssts & 0x0F;
        uint8_t  ipm  = (ssts >> 8) & 0x0F;
        if (det != HBA_PORT_DEV_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) continue;
        if (p->sig != SATA_SIG_ATA) continue;

        ports[i]            = p;
        g_ahci_saved_clb[i] = p->clb;
        g_ahci_saved_fb[i]  = p->fb;

        char msg[] = "AHCI: SATA drive at port X (ahcid will claim)";
        msg[24] = '0' + i;
        framebuffer_draw_string(msg, 100, 620 + (i * 20),
                                COLOR_GREEN, 0x00101828);
    }
}

void ahci_expose_to_userdrv(void) {
    extern int userdrv_mark_claimable(uint16_t vendor_id, uint16_t device_id);
    // QEMU AHCI is Intel ICH9 family.  Real hardware enumerates whatever the
    // BIOS reports; userdrv_mark_claimable looks up by (vendor, device).
    int rc = userdrv_mark_claimable(0x8086, 0x2922);
    if (rc != 0) {
        klog(KLOG_INFO, SUBSYS_DRV,
             "ahci_expose_to_userdrv: no AHCI HBA found (rc=%d)", rc);
    } else {
        klog(KLOG_INFO, SUBSYS_DRV,
             "ahci_expose_to_userdrv: AHCI HBA claimable by ahcid");
    }
}

// L10 substrate: when ahcid dies, its DMA VMOs are torn down but the HBA may
// still hold pointers (PxCLB / PxFB) into that freed memory.  Re-stop each
// port's command engine, restore our saved physical addresses, clear pending
// IRQs, and restart the engine so the HBA is in a valid state for either
// kernel-side recovery probes or the next ahcid spawn.  Idempotent.
void ahci_restore_after_userdrv_death(void) {
    if (!hba_mem) return;
    spinlock_acquire(&ahci_lock);
    for (int i = 0; i < 32; i++) {
        if (!ports[i] || g_ahci_saved_clb[i] == 0) continue;
        ahci_port_t *p = ports[i];

        p->cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
        int budget = 200000;
        while ((p->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR)) && budget-- > 0) {
            asm volatile("pause" ::: "memory");
        }

        p->clb = g_ahci_saved_clb[i];
        p->fb  = g_ahci_saved_fb[i];
        p->is  = 0xFFFFFFFFu;

        budget = 200000;
        while ((p->cmd & HBA_PxCMD_CR) && budget-- > 0) {
            asm volatile("pause" ::: "memory");
        }
        p->cmd |= HBA_PxCMD_FRE;
        p->cmd |= HBA_PxCMD_ST;
        asm volatile("mfence" ::: "memory");
    }
    spinlock_release(&ahci_lock);
    klog(KLOG_INFO, SUBSYS_DRV,
         "ahci_restore_after_userdrv_death: ports re-pointed to kernel state");
}
