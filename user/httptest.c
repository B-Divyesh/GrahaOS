// user/httptest.c
// Phase 9b: Mongoose TCP/IP stack test suite
// 5 automated tests covering network stack status

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/state.h"

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, condition) do { \
    if (condition) { \
        printf("[PASS] %s\n", name); \
        tests_passed++; \
    } else { \
        printf("[FAIL] %s\n", name); \
        tests_failed++; \
    } \
} while(0)

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
// Test 1: NET_STATUS syscall succeeds and stack is running
// =====================================================
void test_stack_running(void) {
    printf("\n=== Group 1: Stack Status ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    int ret = syscall_net_status(&status);
    TEST("1. NET_STATUS syscall succeeds (ret=0)", ret == 0);
    TEST("2. TCP/IP stack is running", status.stack_running == 1);
}

// =====================================================
// Test 2: IP address is 10.0.2.15
// =====================================================
void test_ip_address(void) {
    printf("\n=== Group 2: IP Configuration ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_ip = (status.ip[0] == 10 && status.ip[1] == 0 &&
                      status.ip[2] == 2 && status.ip[3] == 15);
    TEST("3. IP address is 10.0.2.15", correct_ip);
}

// =====================================================
// Test 3: Netmask is 255.255.255.0
// =====================================================
void test_netmask(void) {
    printf("\n=== Group 3: Netmask ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_mask = (status.netmask[0] == 255 && status.netmask[1] == 255 &&
                        status.netmask[2] == 255 && status.netmask[3] == 0);
    TEST("4. Netmask is 255.255.255.0", correct_mask);
}

// =====================================================
// Test 4: Gateway is 10.0.2.2
// =====================================================
void test_gateway(void) {
    printf("\n=== Group 4: Gateway ===\n");

    net_status_t status;
    memset(&status, 0, sizeof(status));
    syscall_net_status(&status);

    int correct_gw = (status.gateway[0] == 10 && status.gateway[1] == 0 &&
                      status.gateway[2] == 2 && status.gateway[3] == 2);
    TEST("5. Gateway is 10.0.2.2", correct_gw);
}

// =====================================================
// Test 5: tcp_ip CAN capability is registered and ON
// =====================================================
void test_can_capability(void) {
    printf("\n=== Group 5: CAN Capability ===\n");

    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        printf("  Failed to query capabilities\n");
        tests_failed++;
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
    TEST("6. tcp_ip capability registered and ON", found && is_on);
}

// Main
void _start(void) {
    printf("=== Mongoose TCP/IP Stack Test Suite ===\n");
    printf("Phase 9b: Network stack tests\n");

    test_stack_running();
    test_ip_address();
    test_netmask();
    test_gateway();
    test_can_capability();

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("ALL TESTS PASSED!\n");
    }

    syscall_exit(tests_failed);
}
