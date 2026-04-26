// user/ifconfig.c — Phase 22 Stage C.
//
// Network-state reader. Connects to /sys/net/service, sends
// net_query_req{field=ALL}, renders the response in the Unix-ish ifconfig
// layout that pre-Phase-22 users expect. Replaces the built-in gash
// `ifconfig` command that dispatched into SYS_NET_IFCONFIG (now removed).
//
// Exit codes:
//   0 — success
//   1 — netd not reachable (rawnet registry returned -EBADF for 20 s)
//   2 — RPC failed (inline-payload too short, bad op, timeout)

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "syscalls.h"
#include "libnet/libnet.h"
#include "libnet/libnet_msg.h"

extern int printf(const char *fmt, ...);

static void print_ip(const char *label, uint32_t ip) {
    printf("%s%u.%u.%u.%u\n", label,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
           (ip >> 8) & 0xFF, ip & 0xFF);
}

void _start(void) {
    libnet_client_ctx_t ctx;
    int rc = libnet_connect_service_with_retry("/sys/net/service", 16,
                                                20000, &ctx);
    if (rc < 0) {
        printf("ifconfig: cannot connect to /sys/net/service (rc=%d)\n", rc);
        syscall_exit(1);
    }

    libnet_net_query_resp_t resp;
    rc = libnet_net_query(&ctx, LIBNET_NET_QUERY_FIELD_ALL,
                           2000000000ull /* 2 s */, &resp);
    if (rc < 0) {
        printf("ifconfig: net_query failed (rc=%d)\n", rc);
        syscall_exit(2);
    }

    printf("eth0\n");
    printf("  HWaddr %02x:%02x:%02x:%02x:%02x:%02x link %s\n",
           resp.mac[0], resp.mac[1], resp.mac[2],
           resp.mac[3], resp.mac[4], resp.mac[5],
           resp.link_up ? "up" : "down");
    print_ip("  inet ",    resp.ip);
    print_ip("  netmask ", resp.netmask);
    print_ip("  gateway ", resp.gateway);
    print_ip("  dns ",     resp.dns);
    printf("  RX packets %lu bytes %lu\n",
           (unsigned long)resp.rx_packets,
           (unsigned long)resp.rx_bytes);
    printf("  TX packets %lu bytes %lu\n",
           (unsigned long)resp.tx_packets,
           (unsigned long)resp.tx_bytes);
    printf("  arp entries %u  tcp sockets %u  udp sockets %u  stack %s\n",
           (unsigned)resp.arp_entries_count,
           (unsigned)resp.tcp_sockets_count,
           (unsigned)resp.udp_sockets_count,
           resp.stack_running ? "running" : "pending");
    syscall_exit(0);
}
