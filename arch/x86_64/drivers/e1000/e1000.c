// arch/x86_64/drivers/e1000/e1000.c
// Phase 9a: Intel E1000 (82540EM) NIC driver
// Provides raw Ethernet frame send/receive for Mongoose TCP/IP stack

#include "e1000.h"
#include "../../cpu/pci.h"
#include "../../cpu/ports.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../drivers/serial/serial.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/capability.h"
#include "../../../../kernel/sync/spinlock.h"
#include <stddef.h>
#include "../../../../kernel/log.h"

// HHDM offset for physical-to-virtual conversion
extern uint64_t g_hhdm_offset;

// Driver state
static volatile uint8_t *mmio_base = NULL;
static e1000_rx_desc_t  *rx_descs  = NULL;
static e1000_tx_desc_t  *tx_descs  = NULL;
static uint8_t *rx_buffers[E1000_NUM_RX_DESC];
static uint8_t *tx_buffers[E1000_NUM_TX_DESC];
static uint8_t  mac_addr[6];
static uint32_t rx_tail = 0;
static uint32_t tx_tail = 0;
static bool     e1000_found = false;
static spinlock_t e1000_lock = SPINLOCK_INITIALIZER("e1000");

// Packet statistics
static uint64_t tx_packets = 0;
static uint64_t rx_packets = 0;
static uint64_t tx_bytes   = 0;
static uint64_t rx_bytes   = 0;

// ---- Register access ----

static inline uint32_t e1000_read_reg(uint32_t offset) {
    return *(volatile uint32_t *)(mmio_base + offset);
}

static inline void e1000_write_reg(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(mmio_base + offset) = value;
}

static inline void memory_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

// ---- MAC address ----

static void e1000_read_mac_eeprom(void) {
    // Try reading MAC from EEPROM via EERD register
    for (int i = 0; i < 3; i++) {
        e1000_write_reg(E1000_EERD, (i << 8) | E1000_EERD_START);

        // Poll for DONE bit with timeout
        uint32_t val;
        int timeout = 10000;
        do {
            val = e1000_read_reg(E1000_EERD);
            if (--timeout <= 0) break;
        } while (!(val & E1000_EERD_DONE));

        if (timeout <= 0) {
            // EEPROM read failed, fall back to RAL/RAH
            uint32_t ral = e1000_read_reg(E1000_RAL);
            uint32_t rah = e1000_read_reg(E1000_RAH);
            mac_addr[0] = ral & 0xFF;
            mac_addr[1] = (ral >> 8) & 0xFF;
            mac_addr[2] = (ral >> 16) & 0xFF;
            mac_addr[3] = (ral >> 24) & 0xFF;
            mac_addr[4] = rah & 0xFF;
            mac_addr[5] = (rah >> 8) & 0xFF;
            klog(KLOG_INFO, SUBSYS_DRV, "[E1000] MAC read from RAL/RAH (EEPROM timeout)");
            return;
        }

        uint16_t word = (val >> 16) & 0xFFFF;
        mac_addr[i * 2]     = word & 0xFF;
        mac_addr[i * 2 + 1] = (word >> 8) & 0xFF;
    }

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] MAC read from EEPROM");
}

// ---- RX ring initialization ----

static void e1000_init_rx(void) {
    // Allocate descriptor ring (256 * 16 = 4096 = 1 page)
    void *rx_ring_phys = pmm_alloc_page();
    if (!rx_ring_phys) {
        klog(KLOG_ERROR, SUBSYS_DRV, "[E1000] ERROR: Failed to allocate RX descriptor ring");
        return;
    }
    rx_descs = (e1000_rx_desc_t *)((uint64_t)rx_ring_phys + g_hhdm_offset);

    // Zero the descriptor ring
    uint8_t *p = (uint8_t *)rx_descs;
    for (int i = 0; i < 4096; i++) p[i] = 0;

    // Allocate RX buffers (each 2KB, 2 buffers per page)
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        if ((i & 1) == 0) {
            // Allocate a new page every 2 buffers
            void *page_phys = pmm_alloc_page();
            if (!page_phys) {
                klog(KLOG_ERROR, SUBSYS_DRV, "[E1000] ERROR: Failed to allocate RX buffer");
                return;
            }
            rx_buffers[i] = (uint8_t *)((uint64_t)page_phys + g_hhdm_offset);
            rx_descs[i].addr = (uint64_t)page_phys;
            if (i + 1 < E1000_NUM_RX_DESC) {
                rx_buffers[i + 1] = rx_buffers[i] + E1000_BUF_SIZE;
                rx_descs[i + 1].addr = (uint64_t)page_phys + E1000_BUF_SIZE;
            }
        }
        rx_descs[i].status = 0;
    }

    // Configure RX descriptor ring registers
    uint64_t rx_phys = (uint64_t)rx_ring_phys;
    e1000_write_reg(E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000_write_reg(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write_reg(E1000_RDH, 0);
    e1000_write_reg(E1000_RDT, E1000_NUM_RX_DESC - 1);
    rx_tail = 0;

    // Enable receiver
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC;
    e1000_write_reg(E1000_RCTL, rctl);

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] RX ring initialized (256 descriptors)");
}

// ---- TX ring initialization ----

static void e1000_init_tx(void) {
    // Allocate descriptor ring (256 * 16 = 4096 = 1 page)
    void *tx_ring_phys = pmm_alloc_page();
    if (!tx_ring_phys) {
        klog(KLOG_ERROR, SUBSYS_DRV, "[E1000] ERROR: Failed to allocate TX descriptor ring");
        return;
    }
    tx_descs = (e1000_tx_desc_t *)((uint64_t)tx_ring_phys + g_hhdm_offset);

    // Zero the descriptor ring
    uint8_t *p = (uint8_t *)tx_descs;
    for (int i = 0; i < 4096; i++) p[i] = 0;

    // Allocate TX buffers (each 2KB, 2 buffers per page)
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        if ((i & 1) == 0) {
            void *page_phys = pmm_alloc_page();
            if (!page_phys) {
                klog(KLOG_ERROR, SUBSYS_DRV, "[E1000] ERROR: Failed to allocate TX buffer");
                return;
            }
            tx_buffers[i] = (uint8_t *)((uint64_t)page_phys + g_hhdm_offset);
            if (i + 1 < E1000_NUM_TX_DESC) {
                tx_buffers[i + 1] = tx_buffers[i] + E1000_BUF_SIZE;
            }
        }
        // Mark all TX descriptors as done (available for use)
        tx_descs[i].status = E1000_DESC_DD;
        tx_descs[i].addr = 0;
    }

    // Configure TX descriptor ring registers
    uint64_t tx_phys = (uint64_t)tx_ring_phys;
    e1000_write_reg(E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000_write_reg(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write_reg(E1000_TDH, 0);
    e1000_write_reg(E1000_TDT, 0);
    tx_tail = 0;

    // Enable transmitter
    // CT = 0x10 (collision threshold), COLD = 0x40 (collision distance for full-duplex)
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 4) | (0x40 << 12);
    e1000_write_reg(E1000_TCTL, tctl);

    // Set inter-packet gap (standard values for IEEE 802.3)
    // IPGT=10, IPGR1=8, IPGR2=6
    e1000_write_reg(E1000_TIPG, 0x0060200A);

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] TX ring initialized (256 descriptors)");
}

// ---- CAN stats callback ----

static int e1000_get_stats(state_driver_stat_t *stats, int max) {
    if (!stats || max < 4) return 0;

    const char *keys[] = {"tx_packets", "rx_packets", "tx_bytes", "rx_bytes"};
    uint64_t vals[] = {tx_packets, rx_packets, tx_bytes, rx_bytes};

    for (int i = 0; i < 4 && i < max; i++) {
        const char *k = keys[i];
        for (int j = 0; k[j] && j < STATE_STAT_KEY_LEN - 1; j++) stats[i].key[j] = k[j];
        stats[i].key[STATE_STAT_KEY_LEN - 1] = '\0';
        stats[i].value = vals[i];
    }
    return 4;
}

// ---- Driver initialization ----

void e1000_init(void) {
    spinlock_init(&e1000_lock, "e1000");

    // Scan PCI for Ethernet controller
    pci_device_t net_dev;
    if (!pci_scan_for_device(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, &net_dev)) {
        klog(KLOG_INFO, SUBSYS_DRV, "[E1000] No Ethernet controller found on PCI bus");
        return;
    }

    // Verify it's an Intel E1000
    if (net_dev.vendor_id != E1000_VENDOR_ID || net_dev.device_id != E1000_DEVICE_ID) {
        klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Found NIC but not E1000 (vendor=0x0x%lx device=0x0x%lx)", (unsigned long)(net_dev.vendor_id), (unsigned long)(net_dev.device_id));
        return;
    }

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Found at bus=%lu dev=%lu func=%lu", (unsigned long)(net_dev.bus), (unsigned long)(net_dev.device), (unsigned long)(net_dev.function));

    // Enable PCI bus mastering (required for DMA)
    pci_enable_bus_mastering(net_dev.bus, net_dev.device, net_dev.function);
    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Bus mastering enabled");

    // Read BAR0 (Memory-Mapped I/O base)
    uint32_t bar0 = pci_read_bar(net_dev.bus, net_dev.device, net_dev.function, 0);
    uint64_t bar0_phys = bar0 & 0xFFFFFFF0;  // Mask type bits

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] BAR0 physical: 0x0x%lx", (unsigned long)(bar0_phys));

    // Map E1000 MMIO region (128KB = 32 pages)
    for (uint64_t offset = 0; offset < 0x20000; offset += 0x1000) {
        vmm_map_page(vmm_get_kernel_space(),
                     bar0_phys + offset + g_hhdm_offset,
                     bar0_phys + offset,
                     PTE_PRESENT | PTE_WRITABLE | PTE_CACHEDISABLE | PTE_NX);
    }
    mmio_base = (volatile uint8_t *)(bar0_phys + g_hhdm_offset);
    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] MMIO mapped at virtual 0x0x%lx", (unsigned long)((uint64_t)mmio_base));

    // Reset the device
    uint32_t ctrl = e1000_read_reg(E1000_CTRL);
    e1000_write_reg(E1000_CTRL, ctrl | E1000_CTRL_RST);

    // Wait for reset to complete (~10ms busy-wait)
    for (volatile int i = 0; i < 100000; i++) {
        asm volatile("pause");
    }

    // Disable all interrupts (we're using polling mode)
    e1000_write_reg(E1000_IMC, 0xFFFFFFFF);
    e1000_read_reg(E1000_ICR);  // Clear pending interrupts

    // Set link up + auto-speed detection
    ctrl = e1000_read_reg(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_RST;  // Clear reset bit
    e1000_write_reg(E1000_CTRL, ctrl);

    // Read MAC address
    e1000_read_mac_eeprom();

    // Program MAC into RAL/RAH for receive filtering
    uint32_t ral = (uint32_t)mac_addr[0] | ((uint32_t)mac_addr[1] << 8) |
                   ((uint32_t)mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24);
    uint32_t rah = (uint32_t)mac_addr[4] | ((uint32_t)mac_addr[5] << 8) | (1U << 31);  // AV=Valid
    e1000_write_reg(E1000_RAL, ral);
    e1000_write_reg(E1000_RAH, rah);

    // Format the MAC into one klog message rather than printing each
    // byte separately (Phase 13 sweep — single-line output).
    klog(KLOG_INFO, SUBSYS_DRV,
         "[E1000] MAC: %lx:%lx:%lx:%lx:%lx:%lx",
         (unsigned long)mac_addr[0], (unsigned long)mac_addr[1],
         (unsigned long)mac_addr[2], (unsigned long)mac_addr[3],
         (unsigned long)mac_addr[4], (unsigned long)mac_addr[5]);

    // Clear Multicast Table Array (128 entries)
    for (int i = 0; i < 128; i++) {
        e1000_write_reg(E1000_MTA_BASE + (i * 4), 0);
    }

    // Initialize RX and TX rings
    e1000_init_rx();
    e1000_init_tx();

    // Check link status
    uint32_t status = e1000_read_reg(E1000_STATUS);
    if (status & E1000_STATUS_LU) {
        klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Link UP");
    } else {
        klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Link DOWN (will come up when QEMU network is ready)");
    }

    e1000_found = true;

    // Register with CAN
    const char *e1000_deps[] = {"pci_bus"};
    cap_op_t e1000_ops[2];
    cap_op_set(&e1000_ops[0], "send", 2, 1);
    cap_op_set(&e1000_ops[1], "receive", 2, 0);
    cap_register("e1000_nic", CAP_DRIVER, CAP_SUBTYPE_OTHER, -1,
                 e1000_deps, 1, NULL, NULL, e1000_ops, 2, e1000_get_stats);

    klog(KLOG_INFO, SUBSYS_DRV, "[E1000] Driver initialized successfully");
}

// ---- Send packet ----

int e1000_send(const void *data, uint16_t length) {
    if (!e1000_found || !data || length == 0 || length > E1000_BUF_SIZE) {
        return -1;
    }

    spinlock_acquire(&e1000_lock);

    uint32_t tail = tx_tail;

    // Check if descriptor is available (DD bit set = hardware is done with it)
    if (!(tx_descs[tail].status & E1000_DESC_DD)) {
        spinlock_release(&e1000_lock);
        return -2;  // TX ring full
    }

    // Copy packet data to pre-allocated buffer
    uint8_t *buf = tx_buffers[tail];
    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < length; i++) {
        buf[i] = src[i];
    }

    // Get physical address of buffer
    uint64_t buf_phys;
    uint64_t buf_virt = (uint64_t)buf;
    if (buf_virt >= 0xFFFF800000000000ULL) {
        buf_phys = buf_virt - g_hhdm_offset;
    } else {
        buf_phys = buf_virt;
    }

    // Fill descriptor
    tx_descs[tail].addr = buf_phys;
    tx_descs[tail].length = length;
    tx_descs[tail].cso = 0;
    tx_descs[tail].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
    tx_descs[tail].status = 0;  // Clear DD, hardware will set when done
    tx_descs[tail].css = 0;
    tx_descs[tail].special = 0;

    memory_barrier();

    // Advance tail pointer (tell hardware to send)
    tx_tail = (tail + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_TDT, tx_tail);

    tx_packets++;
    tx_bytes += length;

    spinlock_release(&e1000_lock);
    return 0;
}

// ---- Receive packet ----

int e1000_receive(void *buffer, uint16_t max_length) {
    if (!e1000_found || !buffer || max_length == 0) {
        return -1;
    }

    spinlock_acquire(&e1000_lock);

    uint32_t tail = rx_tail;

    // Check if the current descriptor has a received packet
    if (!(rx_descs[tail].status & E1000_DESC_DD)) {
        spinlock_release(&e1000_lock);
        return -1;  // No packet available
    }

    // Check for errors
    if (rx_descs[tail].errors) {
        // Bad packet, recycle descriptor
        rx_descs[tail].status = 0;
        uint32_t old_tail = tail;
        rx_tail = (tail + 1) % E1000_NUM_RX_DESC;
        e1000_write_reg(E1000_RDT, old_tail);
        spinlock_release(&e1000_lock);
        return -2;
    }

    // Copy received data
    uint16_t pkt_len = rx_descs[tail].length;
    if (pkt_len > max_length) pkt_len = max_length;

    uint8_t *dst = (uint8_t *)buffer;
    uint8_t *src = rx_buffers[tail];
    for (uint16_t i = 0; i < pkt_len; i++) {
        dst[i] = src[i];
    }

    // Recycle descriptor: clear status, give back to hardware
    rx_descs[tail].status = 0;
    uint32_t old_tail = tail;
    rx_tail = (tail + 1) % E1000_NUM_RX_DESC;
    e1000_write_reg(E1000_RDT, old_tail);

    rx_packets++;
    rx_bytes += pkt_len;

    spinlock_release(&e1000_lock);
    return (int)pkt_len;
}

// ---- Query functions ----

void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = mac_addr[i];
    }
}

bool e1000_link_up(void) {
    if (!e1000_found) return false;
    return (e1000_read_reg(E1000_STATUS) & E1000_STATUS_LU) != 0;
}

bool e1000_is_present(void) {
    return e1000_found;
}
