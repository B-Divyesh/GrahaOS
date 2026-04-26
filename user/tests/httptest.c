// user/tests/httptest.c — Phase 22 Stage E migration.
//
// Pre-Phase-22 this called SYS_NET_STATUS (TCP/IP stack status). Stage E
// swaps to libnet_net_query against /sys/net/service published by netd.
// Coverage parity: 6 assertions (stack running, IP, netmask, gateway, plus
// CAN cap registered+ON which still routes through the kernel state API).
//
// Sandbox-friendly: probes /sys/net/service before running. Auto-skip if
// netd isn't running (the default ktest autorun mode).

#include "../libtap.h"
#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"
#include "../../kernel/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int s_netd_alive = -1;
static libnet_client_ctx_t s_ctx;

static int probe_netd(void) {
    if (s_netd_alive != -1) return s_netd_alive;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                                (uint32_t)strlen(LIBNET_NAME_SERVICE),
                                                /*total_timeout_ms=*/2000,
                                                &s_ctx);
    s_netd_alive = (rc >= 0) ? 1 : 0;
    if (!s_netd_alive) {
        printf("  netd not reachable (rc=%d) — skipping httptest suite\n", rc);
    }
    return s_netd_alive;
}

static int run_query(uint32_t field, libnet_net_query_resp_t *out) {
    memset(out, 0, sizeof(*out));
    return libnet_net_query(&s_ctx, field, /*timeout_ns=*/2000000000ull, out);
}

// Decompose a host-order IPv4 into bytes (high byte = 1st octet).
static void ip_octets(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)((ip >> 24) & 0xFF);
    out[1] = (uint8_t)((ip >> 16) & 0xFF);
    out[2] = (uint8_t)((ip >> 8)  & 0xFF);
    out[3] = (uint8_t)(ip & 0xFF);
}

// =====================================================
// Tests 1-2: net_query succeeds + stack is running
// =====================================================
static void test_stack_running(void) {
    printf("\n=== Group 1: Stack Status ===\n");
    if (!probe_netd()) {
        tap_skip("1. net_query(STATUS) succeeds", "netd not running");
        tap_skip("2. TCP/IP stack is running", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    int rc = run_query(LIBNET_NET_QUERY_FIELD_STATUS, &resp);
    TAP_ASSERT(rc == 0, "1. net_query(STATUS) succeeds");
    TAP_ASSERT(resp.stack_running == 1, "2. TCP/IP stack is running");
}

static void test_ip_address(void) {
    printf("\n=== Group 2: IP Configuration ===\n");
    if (!probe_netd()) {
        tap_skip("3. IP address is 10.0.2.15", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    if (run_query(LIBNET_NET_QUERY_FIELD_CONFIG, &resp) < 0) {
        tap_not_ok("3. IP address is 10.0.2.15", "net_query failed");
        return;
    }
    uint8_t ip[4];
    ip_octets(resp.ip, ip);
    int correct_ip = (ip[0] == 10 && ip[1] == 0 && ip[2] == 2 && ip[3] == 15);
    TAP_ASSERT(correct_ip, "3. IP address is 10.0.2.15");
}

static void test_netmask(void) {
    printf("\n=== Group 3: Netmask ===\n");
    if (!probe_netd()) {
        tap_skip("4. Netmask is 255.255.255.0", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    if (run_query(LIBNET_NET_QUERY_FIELD_CONFIG, &resp) < 0) {
        tap_not_ok("4. Netmask is 255.255.255.0", "net_query failed");
        return;
    }
    uint8_t mask[4];
    ip_octets(resp.netmask, mask);
    int correct_mask = (mask[0] == 255 && mask[1] == 255 && mask[2] == 255 && mask[3] == 0);
    TAP_ASSERT(correct_mask, "4. Netmask is 255.255.255.0");
}

static void test_gateway(void) {
    printf("\n=== Group 4: Gateway ===\n");
    if (!probe_netd()) {
        tap_skip("5. Gateway is 10.0.2.2", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    if (run_query(LIBNET_NET_QUERY_FIELD_CONFIG, &resp) < 0) {
        tap_not_ok("5. Gateway is 10.0.2.2", "net_query failed");
        return;
    }
    uint8_t gw[4];
    ip_octets(resp.gateway, gw);
    int correct_gw = (gw[0] == 10 && gw[1] == 0 && gw[2] == 2 && gw[3] == 2);
    TAP_ASSERT(correct_gw, "5. Gateway is 10.0.2.2");
}

// Test 6: tcp_ip CAN capability remains registered + ON. CAN registry
// survives Phase 22 unchanged so this assertion stays as before.
static void test_can_capability(void) {
    printf("\n=== Group 5: CAN Capability ===\n");

    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        tap_not_ok("6. tcp_ip capability registered and ON", "state query failed");
        return;
    }

    int found = 0;
    int is_on = 0;
    for (uint32_t i = 0; i < caps.count; i++) {
        if (strcmp(caps.caps[i].name, "tcp_ip") == 0) {
            found = 1;
            is_on = (caps.caps[i].state == 2);
            break;
        }
    }
    TAP_ASSERT(found && is_on, "6. tcp_ip capability registered and ON");
}

void _start(void) {
    printf("=== httptest — Phase 22 Stage E (libnet/net_query) ===\n");

    tap_plan(6);

    test_stack_running();
    test_ip_address();
    test_netmask();
    test_gateway();
    test_can_capability();

    tap_done();
    exit(0);
}
