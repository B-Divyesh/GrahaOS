// arch/x86_64/drivers/ahci/ahci.c - COMPLETE FILE
#include "ahci.h"
#include "../../cpu/pci.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/fs/vfs.h"
#include "../../../../kernel/sync/spinlock.h"
#include "../../../../kernel/cap/can.h"
#include "../../../../kernel/log.h"

#define HBA_PORT_DEV_PRESENT 0x3
#define HBA_PORT_IPM_ACTIVE 0x1
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_SEMB  0xC33C0101
#define SATA_SIG_PM    0x96690101

#define HBA_PxCMD_ST  0x0001
#define HBA_PxCMD_FRE 0x0010
#define HBA_PxCMD_FR  0x4000
#define HBA_PxCMD_CR  0x8000

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_CACHE   0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

static ahci_hba_mem_t *hba_mem;
static ahci_port_t* ports[32];
static int port_count = 0;
static spinlock_t ahci_lock = SPINLOCK_INITIALIZER("ahci");

// Phase 16: CAN activation flag (gates the read/write fast paths) and a
// snapshot of per-port saved-CMD bits so reactivation can restore the same
// ST+FRE state the port had before deactivation.
static bool g_ahci_active = true;
static uint32_t g_ahci_saved_cmd[32];

// Phase 23 P23.deferred.2 mitigation: Snapshot of each port's CLB/FB
// physical addresses captured at port_rebase. Used by
// ahci_restore_after_userdrv_death to recover from a userspace ahcid
// daemon that called port_init (overwriting our pointers) and then
// died. Without this, kernel writes to PxCLB after ahcid dies hit
// unmapped memory because ahcid's DMA VMO is freed.
static uint64_t g_ahci_saved_clb[32];
static uint64_t g_ahci_saved_fb[32];
// Drain / quiesce timeout limits. Values chosen to match Phase 16 spec.
#define AHCI_CR_TIMEOUT_ITERS  2000000   // ~1s of pause loops on QEMU.
#define AHCI_FR_TIMEOUT_ITERS  1000000   // ~500ms of pause loops.
#define AHCI_CI_DRAIN_ITERS    2000000   // ~1s for in-flight commands to drain.

// Memory barrier for cache coherency
static inline void memory_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

// Helper to stop a port's command engine
static void port_stop_cmd(ahci_port_t *port) {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    while ((port->cmd & HBA_PxCMD_FR) || (port->cmd & HBA_PxCMD_CR));
}

// Helper to start a port's command engine
static void port_start_cmd(ahci_port_t *port) {
    while (port->cmd & HBA_PxCMD_CR);
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

// Rebase a port and allocate its command structures
static void port_rebase(ahci_port_t *port, int portno) {
    port_stop_cmd(port);

    // Allocate Command List (1KB, 1KB aligned)
    void* cmd_list_phys = pmm_alloc_page();
    port->clb = (uint64_t)cmd_list_phys;
    g_ahci_saved_clb[portno] = (uint64_t)cmd_list_phys;  // Phase 23 P23.deferred.2

    // Allocate FIS Receive Buffer (256 bytes, 256-byte aligned)
    void* fis_buf_phys = pmm_alloc_page();
    port->fb = (uint64_t)fis_buf_phys;
    g_ahci_saved_fb[portno] = (uint64_t)fis_buf_phys;    // Phase 23 P23.deferred.2

    // Get virtual addresses
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((uint64_t)cmd_list_phys + g_hhdm_offset);
    
    // Clear and setup command headers
    for (int i = 0; i < 32; i++) {
        cmd_header[i].prdtl = 8;
        
        // Allocate Command Table
        void* cmd_tbl_phys = pmm_alloc_page();
        cmd_header[i].ctba = (uint64_t)cmd_tbl_phys;
        
        // Clear command table
        ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)((uint64_t)cmd_tbl_phys + g_hhdm_offset);
        void* p = (void*)cmd_table;
        for (int j = 0; j < 4096; j++) {
            ((uint8_t*)p)[j] = 0;
        }
    }

    port_start_cmd(port);
    ports[portno] = port;
    port_count++;
}

// Find free command slot
static int find_cmd_slot(ahci_port_t *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) return i;
        slots >>= 1;
    }
    return -1;
}

// Wait for port to be idle
static int wait_port_idle(ahci_port_t *port) {
    int timeout = 1000000; // 1 second timeout
    while (timeout-- > 0) {
        if ((port->tfd & (0x80 | 0x08)) == 0) {
            return 0; // Port is idle
        }
        for (volatile int i = 0; i < 100; i++); // Small delay
    }
    return -1; // Timeout
}

// Flush cache to ensure persistence
int ahci_flush_cache(int port_num) {
    if (port_num >= port_count || !ports[port_num]) return -1;
    
    spinlock_acquire(&ahci_lock);
    ahci_port_t *port = ports[port_num];
    
    // Clear interrupts
    port->is = (uint32_t)-1;
    
    int slot = find_cmd_slot(port);
    if (slot == -1) {
        spinlock_release(&ahci_lock);
        return -2;
    }
    
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((port->clb + g_hhdm_offset));
    cmd_header += slot;
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->w = 0;
    cmd_header->prdtl = 0; // No data transfer
    cmd_header->c = 1; // Clear BSY on completion
    
    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)((cmd_header->ctba + g_hhdm_offset));
    
    // Clear command FIS
    for (int i = 0; i < 64; i++) {
        cmd_table->cfis[i] = 0;
    }
    
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmd_table->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = 0xE7; // FLUSH CACHE command
    cmdfis->device = 1 << 6; // LBA mode
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion with timeout
    int timeout = 10000000; // 10 seconds
    while (timeout-- > 0) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) { // Task file error
            spinlock_release(&ahci_lock);
            return -3;
        }
        for (volatile int i = 0; i < 100; i++); // Small delay
    }
    
    if (timeout <= 0) {
        spinlock_release(&ahci_lock);
        return -4; // Timeout
    }
    
    spinlock_release(&ahci_lock);
    return 0;
}

// VFS block device functions
int ahci_vfs_read(int dev_id, uint64_t block_num, uint16_t block_count, void* buf) {
    // Convert 4096-byte blocks to 512-byte sectors
    uint64_t sector_lba = block_num * 8;
    uint16_t sector_count = block_count * 8;
    // Pass dev_id directly as port number
    return ahci_read(dev_id, sector_lba, sector_count, buf);
}

int ahci_vfs_write(int dev_id, uint64_t block_num, uint16_t block_count, void* buf) {
    // Convert 4096-byte blocks to 512-byte sectors  
    uint64_t sector_lba = block_num * 8;
    uint16_t sector_count = block_count * 8;
    
    // Pass dev_id directly as port number
    int result = ahci_write(dev_id, sector_lba, sector_count, buf);
    if (result == 0) {
        // Flush cache after successful write
        ahci_flush_cache(dev_id);
    }
    return result;
}

// Driver framework stats callback
static int ahci_get_driver_stats(state_driver_stat_t *stats, int max) {
    if (!stats || max < 2) return 0;
    // Stat 0: port_count
    const char *k0 = "port_count";
    for (int i = 0; k0[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[0].key[i] = k0[i];
    stats[0].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[0].value = (uint64_t)port_count;
    // Stat 1: sector_size
    const char *k1 = "sector_size";
    for (int i = 0; k1[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[1].key[i] = k1[i];
    stats[1].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[1].value = 512;
    return 2;
}

void ahci_init(void) {
    spinlock_init(&ahci_lock, "ahci");
    
    pci_device_t ahci_dev;
    if (!pci_scan_for_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, &ahci_dev)) {
        framebuffer_draw_string("AHCI: No SATA controller found.", 10, 600, COLOR_YELLOW, 0x00101828);
        return;
    }

    uint64_t ahci_base_phys = ahci_dev.bar5 & 0xFFFFFFF0;
    
    vmm_map_page(vmm_get_kernel_space(), (uint64_t)ahci_base_phys + g_hhdm_offset, 
                 ahci_base_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    hba_mem = (ahci_hba_mem_t*)(ahci_base_phys + g_hhdm_offset);

    // Take ownership from BIOS
    if (hba_mem->bohc & (1 << 1)) {
        hba_mem->bohc |= (1 << 0);
        while (!(hba_mem->bohc & (1 << 0)));
    }

    // Enable AHCI mode
    hba_mem->ghc |= (1 << 31);

    // Enumerate ports
    uint32_t ports_implemented = hba_mem->pi;
    for (int i = 0; i < 32; i++) {
        if (ports_implemented & (1 << i)) {
            ahci_port_t *port = (ahci_port_t*)((uint64_t)hba_mem + 0x100 + (i * 0x80));
            
            uint32_t ssts = port->ssts;
            uint8_t det = ssts & 0x0F;
            uint8_t ipm = (ssts >> 8) & 0x0F;

            if (det == HBA_PORT_DEV_PRESENT && ipm == HBA_PORT_IPM_ACTIVE) {
                if (port->sig == SATA_SIG_ATA) {
                    port_rebase(port, i);
                    vfs_register_block_device(i, 512, ahci_vfs_read, ahci_vfs_write);
                    
                    char msg[] = "AHCI: Found SATA drive at port X";
                    msg[31] = '0' + i;
                    framebuffer_draw_string(msg, 100, 620 + (i*20), COLOR_GREEN, 0x00101828);
                }
            }
        }
    }

    // Register with Capability Activation Network
    const char *ahci_deps[] = {"pci_bus"};
    cap_op_t ahci_ops[3];
    cap_op_set(&ahci_ops[0], "read",  3, 0);
    cap_op_set(&ahci_ops[1], "write", 3, 1);
    cap_op_set(&ahci_ops[2], "flush", 1, 1);
    int disk_cap_id = cap_register("disk", CAP_DRIVER, CAP_SUBTYPE_BLOCK, -1,
                                   ahci_deps, 1, ahci_activate, ahci_deactivate,
                                   ahci_ops, 3, ahci_get_driver_stats);
    if (disk_cap_id >= 0) {
        // Phase 16: refuse to quiesce while commands are in flight.
        cap_set_refuse_hook(disk_cap_id, ahci_can_refuse_deactivate);
    }
}

// Phase 16: refuse predicate. Returns nonzero if any port has unacknowledged
// command-issue bits set, in which case cap_deactivate returns -CAP_ERR_BUSY
// without touching any state. Conservative — the read/write paths also
// respect g_ahci_active, so most callers won't bump the refuse threshold.
int ahci_can_refuse_deactivate(void) {
    for (int i = 0; i < 32; i++) {
        if (!ports[i]) continue;
        if (ports[i]->ci != 0) return 1;  // refuse
    }
    return 0;
}

// Phase 16: CAN deactivate. Flushes the VFS first to push dirty pages, then
// per-port: spin for CI==0 (pending commands drain), clear CMD.ST, spin for
// CR==0, clear CMD.FRE, spin for FR==0. If any spin exceeds its budget, abort
// with -CAP_ERR_BUSY; partially-stopped ports stay stopped (driver is still
// coherent, they'll just be reactivated on the next cap_activate("disk")).
int ahci_deactivate(void) {
    // Step 0: flush filesystem caches so no dirty pages are left behind when
    // the disk becomes unresponsive. vfs_sync returns void; we proceed either
    // way, accepting the risk documented in the Phase 16 plan.
    vfs_sync();

    int refused = 0;
    for (int i = 0; i < 32; i++) {
        if (!ports[i]) continue;
        ahci_port_t *port = ports[i];

        // Snapshot current CMD for later restore. Only the low bits are
        // interesting (ST, FRE); rest are HBA-maintained.
        g_ahci_saved_cmd[i] = port->cmd;

        // Drain any in-flight command-issue bits.
        int budget = AHCI_CI_DRAIN_ITERS;
        while (port->ci != 0 && budget-- > 0) asm volatile("pause");
        if (port->ci != 0) { refused = 1; break; }

        // Clear ST, wait for CR=0.
        port->cmd &= ~HBA_PxCMD_ST;
        budget = AHCI_CR_TIMEOUT_ITERS;
        while ((port->cmd & HBA_PxCMD_CR) && budget-- > 0) asm volatile("pause");
        if (port->cmd & HBA_PxCMD_CR) { refused = 1; break; }

        // Clear FRE, wait for FR=0.
        port->cmd &= ~HBA_PxCMD_FRE;
        budget = AHCI_FR_TIMEOUT_ITERS;
        while ((port->cmd & HBA_PxCMD_FR) && budget-- > 0) asm volatile("pause");
        if (port->cmd & HBA_PxCMD_FR) { refused = 1; break; }

        memory_barrier();
    }

    if (refused) {
        klog(KLOG_WARN, SUBSYS_DRV,
             "[AHCI] deactivate timed out; returning -EBUSY, state stays ON");
        return CAP_ERR_BUSY;
    }

    g_ahci_active = false;
    klog(KLOG_INFO, SUBSYS_DRV, "[AHCI] deactivated (all ports quiesced)");
    return 0;
}

// Phase 16: CAN activate. Per-port: set FRE, wait for FR ready, set ST.
int ahci_activate(void) {
    for (int i = 0; i < 32; i++) {
        if (!ports[i]) continue;
        ahci_port_t *port = ports[i];

        // Set FRE, wait until CR is clear (prerequisite to re-enable).
        int budget = AHCI_CR_TIMEOUT_ITERS;
        while ((port->cmd & HBA_PxCMD_CR) && budget-- > 0) asm volatile("pause");
        port->cmd |= HBA_PxCMD_FRE;
        port->cmd |= HBA_PxCMD_ST;
        memory_barrier();
    }
    g_ahci_active = true;
    klog(KLOG_INFO, SUBSYS_DRV, "[AHCI] activated (ports reopened)");
    return 0;
}

bool     ahci_is_active(void)          { return g_ahci_active; }
uint32_t ahci_debug_port_cmd(int port_num) {
    if (port_num < 0 || port_num >= 32 || !ports[port_num]) return 0;
    return ports[port_num]->cmd;
}

// Phase 23 P23.deferred.2: restore the kernel's view of each port's
// command list and FIS receive buffer pointers after a userspace ahcid
// daemon died. ahcid's port_init overwrites PxCLB/PxFB with addresses
// from its own DMA VMOs; once ahcid exits those VMOs are unmapped, so
// the next ahci_read/ahci_write would touch invalid memory. Restoring
// our saved addresses (captured at port_rebase) makes the kernel-resident
// driver functional again.
//
// Steps per port:
//   1. Stop the command engine (clear ST/FRE; spin until CR/FR clear).
//   2. Re-write PxCLB and PxFB to our saved physical addresses.
//   3. Clear interrupt status (PxIS).
//   4. Restart the command engine (FRE then ST).
//
// Idempotent. Safe to call even if no userspace driver was ever active
// (the saved values are simply re-written; effectively a no-op).
//
// Called from userdrv_on_owner_death after the AHCI MMIO cap is destroyed.
// At that point ahcid's process memory has been torn down — but we still
// have safe access to the HBA registers because hba_mem maps the BAR
// physically.
void ahci_restore_after_userdrv_death(void) {
    if (!hba_mem) return;
    spinlock_acquire(&ahci_lock);
    for (int i = 0; i < 32; i++) {
        if (!ports[i]) continue;
        if (g_ahci_saved_clb[i] == 0) continue;  // never rebased
        ahci_port_t *p = ports[i];

        // Stop CMD engine.
        p->cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
        int budget = 200000;
        while ((p->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR)) && budget-- > 0) {
            asm volatile("pause" ::: "memory");
        }

        // Restore our pointers.
        p->clb = g_ahci_saved_clb[i];
        p->fb  = g_ahci_saved_fb[i];

        // Clear pending interrupts.
        p->is = 0xFFFFFFFFu;

        // Restart the engine.
        budget = 200000;
        while ((p->cmd & HBA_PxCMD_CR) && budget-- > 0) {
            asm volatile("pause" ::: "memory");
        }
        p->cmd |= HBA_PxCMD_FRE;
        p->cmd |= HBA_PxCMD_ST;
        memory_barrier();
    }
    spinlock_release(&ahci_lock);
    klog(KLOG_INFO, SUBSYS_DRV,
         "ahci_restore_after_userdrv_death: ports re-pointed to kernel state");
}

// Phase 23 S2: mark this device as claimable so /bin/ahcid can call
// SYS_DRV_REGISTER and obtain MMIO + IRQ caps. The kernel-side driver
// above continues to operate the hardware; the cutover (kernel strip +
// channel-mediated FS I/O) is the Phase 23 production close. Today this
// hook lets the ahcid daemon exist on the live system without disturbing
// grahafs mount or the in-kernel I/O paths.
void ahci_expose_to_userdrv(void) {
    extern int userdrv_mark_claimable(uint16_t vendor_id, uint16_t device_id);
    // QEMU AHCI is always Intel ICH9 family vendor=0x8086 device=0x2922.
    // Real hardware enumerates whatever the BIOS reports; userdrv_mark_
    // claimable looks up by (vendor, device).
    int rc = userdrv_mark_claimable(0x8086, 0x2922);
    if (rc != 0) {
        klog(KLOG_INFO, SUBSYS_DRV,
             "ahci_expose_to_userdrv: no AHCI HBA found (rc=%d)", rc);
    } else {
        klog(KLOG_INFO, SUBSYS_DRV,
             "ahci_expose_to_userdrv: AHCI HBA claimable by ahcid");
    }
}

int ahci_read(int port_num, uint64_t lba, uint16_t count, void *buf) {
    if (port_num >= port_count || !ports[port_num]) return -1;
    if (!g_ahci_active) return -1;  // Phase 16: fast-reject while deactivated.

    spinlock_acquire(&ahci_lock);
    ahci_port_t *port = ports[port_num];

    // Wait for port to be ready
    if (wait_port_idle(port) != 0) {
        spinlock_release(&ahci_lock);
        return -4;
    }

    port->is = (uint32_t)-1;
    int slot = find_cmd_slot(port);
    if (slot == -1) {
        spinlock_release(&ahci_lock);
        return -2;
    }

    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((port->clb + g_hhdm_offset));
    cmd_header += slot;
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->w = 0;
    cmd_header->prdtl = ((count - 1) / 16) + 1;

    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)((cmd_header->ctba + g_hhdm_offset));
    
    uint64_t buf_phys;
    uint64_t buf_addr = (uint64_t)buf;
    
    if (buf_addr >= 0xFFFF800000000000ULL) {
        buf_phys = buf_addr - g_hhdm_offset;
    } else {
        buf_phys = buf_addr;
    }
    
    // Setup PRDT
    int i;
    for (i = 0; i < cmd_header->prdtl - 1; i++) {
        cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
        cmd_table->prdt_entry[i].dbc = 8192 - 1;
        cmd_table->prdt_entry[i].i = 1;
    }
    cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
    cmd_table->prdt_entry[i].dbc = ((count % 16 == 0) ? 16 : (count % 16)) * 512 - 1;
    cmd_table->prdt_entry[i].i = 1;

    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmd_table->cfis);
    
    for (int j = 0; j < 64; j++) {
        cmd_table->cfis[j] = 0;
    }
    
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_READ_DMA_EXT;
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6;
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);

    // Memory barrier before issuing command
    memory_barrier();

    // Issue command
    port->ci = 1 << slot;

    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            spinlock_release(&ahci_lock);
            return -3;
        }
    }

    // Memory barrier after completion
    memory_barrier();

    spinlock_release(&ahci_lock);
    return 0;
}

int ahci_write(int port_num, uint64_t lba, uint16_t count, void *buf) {
    if (port_num >= port_count || !ports[port_num]) return -1;
    if (!g_ahci_active) return -1;  // Phase 16: fast-reject while deactivated.

    spinlock_acquire(&ahci_lock);
    ahci_port_t *port = ports[port_num];

    // Wait for port to be ready
    if (wait_port_idle(port) != 0) {
        spinlock_release(&ahci_lock);
        return -4;
    }

    port->is = (uint32_t)-1;
    int slot = find_cmd_slot(port);
    if (slot == -1) {
        spinlock_release(&ahci_lock);
        return -2;
    }

    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((port->clb + g_hhdm_offset));
    cmd_header += slot;
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->w = 1; // Write
    cmd_header->prdtl = ((count - 1) / 16) + 1;

    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)((cmd_header->ctba + g_hhdm_offset));
    
    uint64_t buf_phys;
    uint64_t buf_addr = (uint64_t)buf;
    
    if (buf_addr >= 0xFFFF800000000000ULL) {
        buf_phys = buf_addr - g_hhdm_offset;
    } else {
        buf_phys = buf_addr;
    }
    
    // Setup PRDT
    int i;
    for (i = 0; i < cmd_header->prdtl - 1; i++) {
        cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
        cmd_table->prdt_entry[i].dbc = 8192 - 1;
    }
    cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
    cmd_table->prdt_entry[i].dbc = ((count % 16 == 0) ? 16 : (count % 16)) * 512 - 1;

    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmd_table->cfis);
    
    for (int j = 0; j < 64; j++) {
        cmd_table->cfis[j] = 0;
    }
    
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_WRITE_DMA_EXT;
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6;
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);

    // Memory barrier before issuing command
    memory_barrier();

    port->ci = 1 << slot;

    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            spinlock_release(&ahci_lock);
            return -3;
        }
    }

    // Memory barrier after completion
    memory_barrier();

    spinlock_release(&ahci_lock);
    return 0;
}

