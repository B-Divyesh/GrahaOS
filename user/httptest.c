// user/httptest.c — Phase 22 Stage E migration.
//
// Pre-Phase-22 this called SYS_NET_STATUS. Stage E swaps to libnet_net_query
// against /sys/net/service. Same 6 assertions: stack running + IP/netmask/
// gateway match + tcp_ip CAN capability ON.

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

static void ip_octets(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)((ip >> 24) & 0xFF);
    out[1] = (uint8_t)((ip >> 16) & 0xFF);
    out[2] = (uint8_t)((ip >> 8)  & 0xFF);
    out[3] = (uint8_t)(ip & 0xFF);
}

void _start(void) {
    printf("=== httptest — Phase 22 Stage E ===\n");

    libnet_client_ctx_t ctx;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                                (uint32_t)strlen(LIBNET_NAME_SERVICE),
                                                /*total_timeout_ms=*/5000, &ctx);
    if (rc < 0) {
        printf("httptest: cannot connect to /sys/net/service (rc=%d)\n", rc);
        syscall_exit(1);
    }

    libnet_net_query_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    rc = libnet_net_query(&ctx, LIBNET_NET_QUERY_FIELD_ALL,
                          /*timeout_ns=*/2000000000ull, &resp);
    TEST("1. net_query syscall succeeds (rc=0)", rc == 0);
    TEST("2. TCP/IP stack is running", resp.stack_running == 1);

    uint8_t ip[4], mask[4], gw[4];
    ip_octets(resp.ip, ip);
    ip_octets(resp.netmask, mask);
    ip_octets(resp.gateway, gw);

    int correct_ip = (ip[0] == 10 && ip[1] == 0 && ip[2] == 2 && ip[3] == 15);
    TEST("3. IP address is 10.0.2.15", correct_ip);

    int correct_mask = (mask[0] == 255 && mask[1] == 255 && mask[2] == 255 && mask[3] == 0);
    TEST("4. Netmask is 255.255.255.0", correct_mask);

    int correct_gw = (gw[0] == 10 && gw[1] == 0 && gw[2] == 2 && gw[3] == 2);
    TEST("5. Gateway is 10.0.2.2", correct_gw);

    state_cap_list_t caps;
    long sret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    int found_cap = 0, on = 0;
    if (sret >= 0) {
        for (uint32_t i = 0; i < caps.count; i++) {
            if (strcmp(caps.caps[i].name, "tcp_ip") == 0) {
                found_cap = 1;
                on = (caps.caps[i].state == 2);
                break;
            }
        }
    }
    TEST("6. tcp_ip capability registered and ON", found_cap && on);

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    if (tests_failed == 0) printf("ALL TESTS PASSED!\n");
    syscall_exit(tests_failed);
}
