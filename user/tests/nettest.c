// user/tests/nettest.c
// Phase 12: TAP 1.4 port of user/nettest.c (Phase 9a E1000 NIC tests).

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// =====================================================
// Test 1: ifconfig syscall returns valid MAC
// =====================================================
static void test_ifconfig_valid(void) {
    printf("\n=== Group 1: ifconfig Syscall ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    int ret = syscall_net_ifconfig(info);
    TAP_ASSERT(ret == 0, "1. ifconfig syscall succeeds (ret=0)");
}

// =====================================================
// Test 2-3: MAC is 6 bytes, first 3 match QEMU OUI
// =====================================================
static void test_mac_address(void) {
    printf("\n=== Group 2: MAC Address ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    syscall_net_ifconfig(info);

    // Check MAC is non-zero
    int nonzero = 0;
    for (int i = 0; i < 6; i++) {
        if (info[i] != 0) nonzero = 1;
    }
    TAP_ASSERT(nonzero, "2. MAC address is non-zero");

    // QEMU E1000 uses OUI 52:54:00 by default
    int qemu_oui = (info[0] == 0x52 && info[1] == 0x54 && info[2] == 0x00);
    TAP_ASSERT(qemu_oui, "3. MAC OUI matches QEMU (52:54:00)");
}

// =====================================================
// Test 4: Link status reports UP
// =====================================================
static void test_link_status(void) {
    printf("\n=== Group 3: Link Status ===\n");

    uint8_t info[7];
    memset(info, 0, sizeof(info));
    syscall_net_ifconfig(info);

    TAP_ASSERT(info[6] == 1, "4. Link status is UP");
}

// =====================================================
// Test 5: E1000 NIC capability registered in CAN
// =====================================================
static void test_can_capability(void) {
    printf("\n=== Group 4: CAN Capability ===\n");

    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        printf("  Failed to query capabilities\n");
        tap_not_ok("5. e1000_nic capability registered and ON", "state query failed");
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
    TAP_ASSERT(found && is_on, "5. e1000_nic capability registered and ON");
}

void _start(void) {
    printf("=== E1000 NIC Driver Test Suite ===\n");
    printf("Phase 9a: Network interface tests\n");

    tap_plan(5);

    test_ifconfig_valid();
    test_mac_address();
    test_link_status();
    test_can_capability();

    tap_done();
    exit(0);
}
