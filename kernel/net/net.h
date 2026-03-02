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

// Phase 9c: HTTP Client + DNS Resolve

// Error codes (returned to user-space as negative values)
#define NET_ERR_TIMEOUT     -10
#define NET_ERR_DNS_FAIL    -11
#define NET_ERR_CONNECT     -12
#define NET_ERR_PROTOCOL    -13
#define NET_ERR_BUSY        -14
#define NET_ERR_NOMEM       -15
#define NET_ERR_BAD_URL     -16
#define NET_ERR_NO_NET      -17

#define NET_MAX_RESPONSE_SIZE 4096
#define NET_REQUEST_TIMEOUT_MS 10000

// Start an HTTP GET request for a task (non-blocking, returns immediately)
// Returns: 0 on success, NET_ERR_BUSY if already in flight, other negative on error
int  net_http_get_start(int task_id, const char *url);

// Check if HTTP GET result is ready for a task
// Returns: body length (>=0) if done, negative error if failed, -99 if still pending
int  net_http_get_check(int task_id, char *user_buf, int max_len);

// Start a DNS resolution for a task (non-blocking)
int  net_dns_start(int task_id, const char *hostname);

// Check DNS result. Copies 4-byte IPv4 to ip_buf if done.
// Returns: 0 if done, negative error if failed, -99 if still pending
int  net_dns_check(int task_id, uint8_t *ip_buf);

// Cleanup pending request for a task (called from SYS_EXIT)
void net_cleanup_task(int task_id);

// Check for timed-out requests (called from mongoose_poll_task)
void net_check_timeouts(void);
