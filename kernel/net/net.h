// kernel/net/net.h
// Phase 9b: Network subsystem public API
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Initialize Mongoose TCP/IP stack (call after e1000_init)
void net_init(void);

// Network status info (for SYS_NET_STATUS syscall)
typedef struct {
    uint8_t  stack_running;
    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint32_t rx_count;
    uint32_t tx_count;
} net_status_t;

// Get current network status
void net_get_status(net_status_t *status);
