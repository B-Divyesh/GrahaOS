// arch/x86_64/drivers/e1000/e1000.h
// Phase 9a: Intel E1000 (82540EM) Gigabit Ethernet driver for QEMU
#pragma once
#include <stdint.h>
#include <stdbool.h>

// PCI identification
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E  // 82540EM (QEMU default)

// Register offsets (from MMIO base)
#define E1000_CTRL          0x00000  // Device Control
#define E1000_STATUS        0x00008  // Device Status
#define E1000_EERD          0x00014  // EEPROM Read
#define E1000_ICR           0x000C0  // Interrupt Cause Read
#define E1000_IMS           0x000D0  // Interrupt Mask Set
#define E1000_IMC           0x000D8  // Interrupt Mask Clear
#define E1000_RCTL          0x00100  // Receive Control
#define E1000_TCTL          0x00400  // Transmit Control
#define E1000_TIPG          0x00410  // TX Inter-Packet Gap
#define E1000_RDBAL         0x02800  // RX Descriptor Base Address Low
#define E1000_RDBAH         0x02804  // RX Descriptor Base Address High
#define E1000_RDLEN         0x02808  // RX Descriptor Length (bytes)
#define E1000_RDH           0x02810  // RX Descriptor Head
#define E1000_RDT           0x02818  // RX Descriptor Tail
#define E1000_TDBAL         0x03800  // TX Descriptor Base Address Low
#define E1000_TDBAH         0x03804  // TX Descriptor Base Address High
#define E1000_TDLEN         0x03808  // TX Descriptor Length (bytes)
#define E1000_TDH           0x03810  // TX Descriptor Head
#define E1000_TDT           0x03818  // TX Descriptor Tail
#define E1000_RAL           0x05400  // Receive Address Low (MAC bytes 0-3)
#define E1000_RAH           0x05404  // Receive Address High (MAC bytes 4-5 + Valid)
#define E1000_MTA_BASE      0x05200  // Multicast Table Array (128 x 32-bit entries)

// CTRL register bits
#define E1000_CTRL_FD       (1 << 0)   // Full Duplex
#define E1000_CTRL_ASDE     (1 << 5)   // Auto-Speed Detection Enable
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up
#define E1000_CTRL_RST      (1 << 26)  // Device Reset

// STATUS register bits
#define E1000_STATUS_LU     (1 << 1)   // Link Up

// RCTL register bits
#define E1000_RCTL_EN       (1 << 1)   // Receiver Enable
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promiscuous Enable
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promiscuous Enable
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2K (0 << 16)  // Buffer Size 2048 bytes
#define E1000_RCTL_SECRC    (1 << 26)  // Strip Ethernet CRC

// TCTL register bits
#define E1000_TCTL_EN       (1 << 1)   // Transmit Enable
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets

// TX descriptor command bits
#define E1000_TXD_CMD_EOP   (1 << 0)   // End of Packet
#define E1000_TXD_CMD_RS    (1 << 3)   // Report Status

// Descriptor status bits
#define E1000_DESC_DD       (1 << 0)   // Descriptor Done
#define E1000_RXD_EOP       (1 << 1)   // End of Packet (RX)

// EERD register bits
#define E1000_EERD_START    (1 << 0)   // Start Read
#define E1000_EERD_DONE     (1 << 4)   // Read Done

// Ring configuration
#define E1000_NUM_RX_DESC   256
#define E1000_NUM_TX_DESC   256
#define E1000_BUF_SIZE      2048

// Legacy Receive Descriptor (16 bytes, must be packed)
typedef struct {
    uint64_t addr;          // Buffer physical address
    uint16_t length;        // Received packet length (set by hardware)
    uint16_t checksum;      // Packet checksum (set by hardware)
    uint8_t  status;        // Descriptor status (DD, EOP)
    uint8_t  errors;        // Error flags
    uint16_t special;       // VLAN tag
} __attribute__((packed)) e1000_rx_desc_t;

// Legacy Transmit Descriptor (16 bytes, must be packed)
typedef struct {
    uint64_t addr;          // Buffer physical address
    uint16_t length;        // Packet length
    uint8_t  cso;           // Checksum offset (0 for no offload)
    uint8_t  cmd;           // Command (EOP, RS)
    uint8_t  status;        // Status (DD set by hardware when done)
    uint8_t  css;           // Checksum start (0)
    uint16_t special;       // VLAN tag (0)
} __attribute__((packed)) e1000_tx_desc_t;

// Driver API
void e1000_init(void);
int  e1000_send(const void *data, uint16_t length);
int  e1000_receive(void *buffer, uint16_t max_length);
void e1000_get_mac(uint8_t mac[6]);
bool e1000_link_up(void);
bool e1000_is_present(void);
