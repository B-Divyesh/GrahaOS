// user/nettest.c — Phase 22 Stage E migration.
//
// Pre-Phase-22 this called SYS_NET_IFCONFIG (now removed in Stage F). Stage E
// swaps the call site to libnet_net_query against /sys/net/service. Coverage
// matches the original 5 assertions: query succeeds, MAC non-zero, MAC OUI
// matches QEMU, link is UP, e1000_nic CAN cap registered+ON.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "libnet/libnet.h"
#include "libnet/libnet_msg.h"
#include "../kernel/state.h"

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

void _start(void) {
    printf("=== nettest — Phase 22 Stage E ===\n");

    libnet_client_ctx_t ctx;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                                (uint32_t)strlen(LIBNET_NAME_SERVICE),
                                                /*total_timeout_ms=*/5000, &ctx);
    if (rc < 0) {
        printf("nettest: cannot connect to /sys/net/service (rc=%d)\n", rc);
        printf("=== Results: 0 passed, 0 failed (netd not reachable) ===\n");
        syscall_exit(1);
    }

    libnet_net_query_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    rc = libnet_net_query(&ctx, LIBNET_NET_QUERY_FIELD_ALL,
                          /*timeout_ns=*/2000000000ull, &resp);
    TEST("1. net_query(ALL) succeeds (rc=0)", rc == 0);

    int nonzero = 0;
    for (int i = 0; i < 6; i++) if (resp.mac[i] != 0) nonzero = 1;
    TEST("2. MAC address is non-zero", nonzero);

    int qemu_oui = (resp.mac[0] == 0x52 && resp.mac[1] == 0x54 && resp.mac[2] == 0x00);
    TEST("3. MAC OUI matches QEMU (52:54:00)", qemu_oui);

    TEST("4. Link status is UP", resp.link_up == 1);

    state_cap_list_t caps;
    long sret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    int found_cap = 0, on = 0;
    if (sret >= 0) {
        for (uint32_t i = 0; i < caps.count; i++) {
            if (strcmp(caps.caps[i].name, "e1000_nic") == 0) {
                found_cap = 1;
                on = (caps.caps[i].state == 2);
                break;
            }
        }
    }
    TEST("5. e1000_nic capability registered and ON", found_cap && on);

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    if (tests_failed == 0) printf("ALL TESTS PASSED!\n");
    syscall_exit(tests_failed);
}
