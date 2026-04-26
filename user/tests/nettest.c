// user/tests/nettest.c — Phase 22 Stage E migration.
//
// Pre-Phase-22 this called SYS_NET_IFCONFIG (now scheduled for -ENOSYS in
// Stage F). Stage E swaps the call site to libnet_net_query against the
// /sys/net/service channel published by netd. Coverage matches the spec
// migration list (5 assertions) and the integration test "nettest:
// ifconfig/ping/arp all via netd" gate.
//
// Sandbox-friendly: probes /sys/net/service. When netd isn't running (the
// default `make test` autorun=ktest mode), every assertion tap_skips with a
// stable reason so the harness still reports a clean total.

#include "../libtap.h"
#include "../syscalls.h"
#include "../libnet/libnet.h"
#include "../libnet/libnet_msg.h"
#include "../../kernel/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// One-shot probe: if /sys/net/service is unreachable, skip the whole suite.
static int s_netd_alive = -1;
static libnet_client_ctx_t s_ctx;

static int probe_netd(void) {
    if (s_netd_alive != -1) return s_netd_alive;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                                (uint32_t)strlen(LIBNET_NAME_SERVICE),
                                                /*total_timeout_ms=*/2000,
                                                &s_ctx);
    s_netd_alive = (rc >= 0) ? 1 : 0;
    if (s_netd_alive == 0) {
        printf("  netd not reachable (rc=%d) — skipping nettest suite\n", rc);
    }
    return s_netd_alive;
}

static int run_query(uint32_t field, libnet_net_query_resp_t *out) {
    memset(out, 0, sizeof(*out));
    return libnet_net_query(&s_ctx, field, /*timeout_ns=*/2000000000ull, out);
}

static void test_query_all(void) {
    printf("\n=== Group 1: net_query (FIELD_ALL) ===\n");
    if (!probe_netd()) {
        tap_skip("1. net_query(ALL) succeeds", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    int rc = run_query(LIBNET_NET_QUERY_FIELD_ALL, &resp);
    TAP_ASSERT(rc == 0, "1. net_query(ALL) succeeds");
}

static void test_mac_address(void) {
    printf("\n=== Group 2: MAC Address ===\n");
    if (!probe_netd()) {
        tap_skip("2. MAC address is non-zero", "netd not running");
        tap_skip("3. MAC OUI matches QEMU (52:54:00)", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    if (run_query(LIBNET_NET_QUERY_FIELD_CONFIG, &resp) < 0) {
        tap_not_ok("2. MAC address is non-zero", "net_query failed");
        tap_not_ok("3. MAC OUI matches QEMU (52:54:00)", "net_query failed");
        return;
    }

    int nonzero = 0;
    for (int i = 0; i < 6; i++) if (resp.mac[i] != 0) nonzero = 1;
    TAP_ASSERT(nonzero, "2. MAC address is non-zero");

    int qemu_oui = (resp.mac[0] == 0x52 && resp.mac[1] == 0x54 && resp.mac[2] == 0x00);
    TAP_ASSERT(qemu_oui, "3. MAC OUI matches QEMU (52:54:00)");
}

static void test_link_status(void) {
    printf("\n=== Group 3: Link Status ===\n");
    if (!probe_netd()) {
        tap_skip("4. Link status is UP", "netd not running");
        return;
    }
    libnet_net_query_resp_t resp;
    if (run_query(LIBNET_NET_QUERY_FIELD_CONFIG, &resp) < 0) {
        tap_not_ok("4. Link status is UP", "net_query failed");
        return;
    }
    TAP_ASSERT(resp.link_up == 1, "4. Link status is UP");
}

// Test 5 keeps probing the CAN registry directly — that path is unaffected
// by Phase 22 and still validates the e1000_nic capability state.
static void test_can_capability(void) {
    printf("\n=== Group 4: CAN Capability ===\n");

    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        tap_not_ok("5. e1000_nic capability registered and ON", "state query failed");
        return;
    }

    int found = 0;
    int is_on = 0;
    for (uint32_t i = 0; i < caps.count; i++) {
        if (strcmp(caps.caps[i].name, "e1000_nic") == 0) {
            found = 1;
            is_on = (caps.caps[i].state == 2);
            break;
        }
    }
    TAP_ASSERT(found && is_on, "5. e1000_nic capability registered and ON");
}

void _start(void) {
    printf("=== nettest — Phase 22 Stage E (libnet/net_query) ===\n");

    tap_plan(5);

    test_query_all();
    test_mac_address();
    test_link_status();
    test_can_capability();

    tap_done();
    exit(0);
}
