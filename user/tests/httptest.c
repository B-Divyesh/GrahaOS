// user/tests/httptest.c
// Phase 12: TAP 1.4 port of user/httptest.c (Phase 9b TCP/IP stack status tests).

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Net status structure (matches kernel/net/net.h)
typedef struct {
    uint8_t  stack_running;
    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint32_t rx_count;
    uint32_t tx_count;
} net_status_t;

// =====================================================
// Test 1-2: NET_STATUS syscall succeeds and stack is running
// =====================================================
static void test_stack_running(void) {
    printf("\n=== Group 1: Stack Status ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    int ret = syscall_net_status(&status);
    TAP_ASSERT(ret == 0, "1. NET_STATUS syscall succeeds (ret=0)");
    TAP_ASSERT(status.stack_running == 1, "2. TCP/IP stack is running");
}

// =====================================================
// Test 3: IP address is 10.0.2.15
// =====================================================
static void test_ip_address(void) {
    printf("\n=== Group 2: IP Configuration ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_ip = (status.ip[0] == 10 && status.ip[1] == 0 &&
                      status.ip[2] == 2 && status.ip[3] == 15);
    TAP_ASSERT(correct_ip, "3. IP address is 10.0.2.15");
}

// =====================================================
// Test 4: Netmask is 255.255.255.0
// =====================================================
static void test_netmask(void) {
    printf("\n=== Group 3: Netmask ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_mask = (status.netmask[0] == 255 && status.netmask[1] == 255 &&
                        status.netmask[2] == 255 && status.netmask[3] == 0);
    TAP_ASSERT(correct_mask, "4. Netmask is 255.255.255.0");
}

// =====================================================
// Test 5: Gateway is 10.0.2.2
// =====================================================
static void test_gateway(void) {
    printf("\n=== Group 4: Gateway ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_gw = (status.gateway[0] == 10 && status.gateway[1] == 0 &&
                      status.gateway[2] == 2 && status.gateway[3] == 2);
    TAP_ASSERT(correct_gw, "5. Gateway is 10.0.2.2");
}

// =====================================================
// Test 6: tcp_ip CAN capability is registered and ON
// =====================================================
static void test_can_capability(void) {
    printf("\n=== Group 5: CAN Capability ===\n");

    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        printf("  Failed to query capabilities\n");
        tap_not_ok("6. tcp_ip capability registered and ON", "state query failed");
        return;
    }

    int found = 0;
    int is_on = 0;
    for (uint32_t i = 0; i < caps.count; i++) {
        if (strcmp(caps.caps[i].name, "tcp_ip") == 0) {
            found = 1;
            is_on = (caps.caps[i].state == 2);  // ON
            break;
        }
    }
    TAP_ASSERT(found && is_on, "6. tcp_ip capability registered and ON");
}

void _start(void) {
    printf("=== Mongoose TCP/IP Stack Test Suite ===\n");
    printf("Phase 9b: Network stack tests\n");

    tap_plan(6);

    test_stack_running();
    test_ip_address();
    test_netmask();
    test_gateway();
    test_can_capability();

    tap_done();
    exit(0);
}
