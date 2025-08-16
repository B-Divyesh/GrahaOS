// arch/x86_64/drivers/ahci/ahci.c
#include "ahci.h"
#include "../../cpu/pci.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/fs/vfs.h"
#include "../../../../kernel/sync/spinlock.h"

#define HBA_PORT_DEV_PRESENT 0x3
#define HBA_PORT_IPM_ACTIVE 0x1
#define SATA_SIG_ATAPI 0xEB140101 // ATAPI device
#define SATA_SIG_ATA   0x00000101 // SATA drive
#define SATA_SIG_SEMB  0xC33C0101 // Enclosure management bridge
#define SATA_SIG_PM    0x96690101 // Port multiplier

#define HBA_PxCMD_ST  0x0001
#define HBA_PxCMD_FRE 0x0010
#define HBA_PxCMD_FR  0x4000
#define HBA_PxCMD_CR  0x8000

static ahci_hba_mem_t *hba_mem;
static ahci_port_t* ports[32];
static int port_count = 0;
static spinlock_t ahci_lock = SPINLOCK_INITIALIZER("ahci");

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

    // Allocate FIS Receive Buffer (256 bytes, 256-byte aligned)
    void* fis_buf_phys = pmm_alloc_page();
    port->fb = (uint64_t)fis_buf_phys;

    // Get virtual addresses
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((uint64_t)cmd_list_phys + g_hhdm_offset);
    
    // Clear command headers
    for (int i = 0; i < 32; i++) {
        cmd_header[i].prdtl = 8;
        
        // Allocate Command Table (256 bytes, 128-byte aligned)
        void* cmd_tbl_phys = pmm_alloc_page();
        cmd_header[i].ctba = (uint64_t)cmd_tbl_phys;
    }

    port_start_cmd(port);
    ports[portno] = port;
    port_count++;
}

// VFS block device read/write functions
int ahci_vfs_read(int dev_id, uint64_t lba, uint16_t count, void* buf) {
    return ahci_read(dev_id, lba, count, buf);
}

int ahci_vfs_write(int dev_id, uint64_t lba, uint16_t count, void* buf) {
    return ahci_write(dev_id, lba, count, buf);
}

void ahci_init(void) {
    spinlock_init(&ahci_lock, "ahci");
    
    pci_device_t ahci_dev;
    if (!pci_scan_for_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, &ahci_dev)) {
        framebuffer_draw_string("AHCI: No SATA controller found.", 10, 600, COLOR_YELLOW, 0x00101828);
        return;
    }

    // BAR5 contains the physical address of the HBA memory registers
    uint64_t ahci_base_phys = ahci_dev.bar5 & 0xFFFFFFF0;
    
    // Map the HBA memory into virtual address space
    vmm_map_page(vmm_get_kernel_space(), (uint64_t)ahci_base_phys + g_hhdm_offset, ahci_base_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    hba_mem = (ahci_hba_mem_t*)(ahci_base_phys + g_hhdm_offset);

    // Take ownership from BIOS if needed
    if (hba_mem->bohc & (1 << 1)) {
        hba_mem->bohc |= (1 << 0); // Request ownership
        while (!(hba_mem->bohc & (1 << 0))); // Wait for grant
    }

    // Enable AHCI mode
    hba_mem->ghc |= (1 << 31);

    // Enumerate implemented ports
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
                    // Register this drive with the VFS
                    vfs_register_block_device(i, 512, ahci_vfs_read, ahci_vfs_write);
                    
                    char msg[] = "AHCI: Found SATA drive at port X";
                    msg[30] = '0' + i;
                    framebuffer_draw_string(msg, 10, 620 + (i*20), COLOR_GREEN, 0x00101828);
                }
            }
        }
    }
}

static int find_cmd_slot(ahci_port_t *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) return i;
        slots >>= 1;
    }
    return -1; // No free command slots
}

int ahci_read(int port_num, uint64_t lba, uint16_t count, void *buf) {
    if (port_num >= port_count || !ports[port_num]) return -1;
    
    spinlock_acquire(&ahci_lock);
    ahci_port_t *port = ports[port_num];

    port->is = (uint32_t)-1; // Clear pending interrupts
    int slot = find_cmd_slot(port);
    if (slot == -1) {
        spinlock_release(&ahci_lock);
        return -2;
    }

    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)((port->clb + g_hhdm_offset));
    cmd_header += slot;
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header->w = 0; // Read
    cmd_header->prdtl = ((count - 1) / 16) + 1;

    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)((cmd_header->ctba + g_hhdm_offset));
    
    // Convert virtual buffer to physical for DMA
    uint64_t buf_phys = (uint64_t)buf;
    if (buf_phys >= 0xFFFF800000000000ULL) {
        buf_phys -= g_hhdm_offset;
    }
    
    // Setup PRDT
    int i;
    for (i = 0; i < cmd_header->prdtl - 1; i++) {
        cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
        cmd_table->prdt_entry[i].dbc = 8192 - 1; // 8K bytes
        cmd_table->prdt_entry[i].i = 1;
    }
    cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
    cmd_table->prdt_entry[i].dbc = ((count % 16 == 0) ? 16 : (count % 16)) * 512 - 1;
    cmd_table->prdt_entry[i].i = 1;

    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmd_table->cfis);
    
    // Clear the FIS
    for (int j = 0; j < 64; j++) {
        cmd_table->cfis[j] = 0;
    }
    
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = 0x25; // DMA READ EXT
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6; // LBA mode
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);

    // Issue command
    port->ci = 1 << slot;

    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            spinlock_release(&ahci_lock);
            return -3; // Task File Error
        }
    }

    spinlock_release(&ahci_lock);
    return 0;
}

int ahci_write(int port_num, uint64_t lba, uint16_t count, void *buf) {
    if (port_num >= port_count || !ports[port_num]) return -1;
    
    spinlock_acquire(&ahci_lock);
    ahci_port_t *port = ports[port_num];

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
    
    // Convert virtual buffer to physical for DMA
    uint64_t buf_phys = (uint64_t)buf;
    if (buf_phys >= 0xFFFF800000000000ULL) {
        buf_phys -= g_hhdm_offset;
    }
    
    int i;
    for (i = 0; i < cmd_header->prdtl - 1; i++) {
        cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
        cmd_table->prdt_entry[i].dbc = 8192 - 1;
    }
    cmd_table->prdt_entry[i].dba = buf_phys + (i * 8192);
    cmd_table->prdt_entry[i].dbc = ((count % 16 == 0) ? 16 : (count % 16)) * 512 - 1;

    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmd_table->cfis);
    
    // Clear the FIS
    for (int j = 0; j < 64; j++) {
        cmd_table->cfis[j] = 0;
    }
    
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = 0x35; // DMA WRITE EXT
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6;
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);

    port->ci = 1 << slot;

    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            spinlock_release(&ahci_lock);
            return -3;
        }
    }

    spinlock_release(&ahci_lock);
    return 0;
}