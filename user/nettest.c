// user/nettest.c
// Phase 9a: E1000 NIC driver test suite
// 4 automated tests covering ifconfig syscall, MAC address, link status, CAN

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

// =====================================================
// Test 1: ifconfig syscall returns valid MAC
// =====================================================
void test_ifconfig_valid(void) {
    printf("\n=== Group 1: ifconfig Syscall ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    int ret = syscall_net_ifconfig(info);
    TEST("1. ifconfig syscall succeeds (ret=0)", ret == 0);
}

// =====================================================
// Test 2: MAC is 6 bytes, first 3 match QEMU OUI
// =====================================================
void test_mac_address(void) {
    printf("\n=== Group 2: MAC Address ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    syscall_net_ifconfig(info);

    // Check MAC is non-zero
    int nonzero = 0;
    for (int i = 0; i < 6; i++) {
        if (info[i] != 0) nonzero = 1;
    }
    TEST("2. MAC address is non-zero", nonzero);

    // QEMU E1000 uses OUI 52:54:00 by default
    int qemu_oui = (info[0] == 0x52 && info[1] == 0x54 && info[2] == 0x00);
    TEST("3. MAC OUI matches QEMU (52:54:00)", qemu_oui);
}

// =====================================================
// Test 3: Link status reports UP
// =====================================================
void test_link_status(void) {
    printf("\n=== Group 3: Link Status ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    syscall_net_ifconfig(info);

    TEST("4. Link status is UP", info[6] == 1);
}

// =====================================================
// Test 4: E1000 NIC capability registered in CAN
// =====================================================
void test_can_capability(void) {
    printf("\n=== Group 4: CAN Capability ===\n");

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
        if (strcmp(caps.caps[i].name, "e1000_nic") == 0) {
            found = 1;
            is_on = (caps.caps[i].state == 2);  // ON
            break;
        }
    }
    TEST("5. e1000_nic capability registered and ON", found && is_on);
}

// Main
void _start(void) {
    printf("=== E1000 NIC Driver Test Suite ===\n");
    printf("Phase 9a: Network interface tests\n");

    test_ifconfig_valid();
    test_mac_address();
    test_link_status();
    test_can_capability();

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("ALL TESTS PASSED!\n");
    }

    syscall_exit(tests_failed);
}
