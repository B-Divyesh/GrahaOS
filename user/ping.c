// user/ping.c — Phase 22 Stage D U20.
//
// ICMP echo client. Connects to /sys/net/service and issues
// icmp_echo_req{dst_ip,id,seq,timeout_ms,payload} for each seq 1..count.
// Prints RTT in µs; exits 0 if any reply was received, 1 otherwise.
//
// Usage:
//   /bin/ping                 — defaults (8.8.8.8, 3 probes, 3 s timeout).
//   /bin/ping with /etc/ping.conf present:
//     target=W.X.Y.Z           — override dst (dotted IP OR DNS hostname)
//     count=N                  — override probe count
//     timeout_ms=N             — override per-probe budget
//     payload_len=N            — override echoed payload length (max 64)
//
// argv-over-spawn is deferred to Phase 16 per GrahaOS' current process
// model; `/etc/ping.conf` is the Phase 22 approximation. Each line is a
// single `key=value` pair; blank lines + `#`-prefixed comments are
// ignored. Missing keys use the defaults.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "syscalls.h"
#include "libnet/libnet.h"
#include "libnet/libnet_msg.h"
#include "netd.h"

extern int printf(const char *fmt, ...);

// Local atoi: libc declares it but has no implementation yet. Tolerant of
// leading whitespace + optional sign; stops at the first non-digit.
static int local_atoi(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+')           s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

typedef struct ping_cfg {
    char      target[128];     // dotted IP or DNS hostname
    uint32_t  count;
    uint32_t  timeout_ms;
    uint32_t  payload_len;
} ping_cfg_t;

static void cfg_defaults(ping_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->target, "8.8.8.8");
    c->count       = 3u;
    c->timeout_ms  = 3000u;
    c->payload_len = 12u;
}

// Read /etc/ping.conf if present. Tolerant of absence / parse errors —
// falls back to defaults on any trouble.
static void cfg_load(ping_cfg_t *cfg) {
    int fd = syscall_open("etc/ping.conf");
    if (fd < 0) return;
    char   buf[512];
    int    n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char line[128];
    int  li = 0;
    for (int i = 0; i <= n; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\0' || li == (int)sizeof(line) - 1) {
            line[li] = '\0';
            // Trim + skip comments.
            int start = 0;
            while (line[start] == ' ' || line[start] == '\t') start++;
            if (line[start] == '#' || line[start] == '\0') { li = 0; continue; }
            // Find '='.
            int eq = -1;
            for (int k = start; line[k]; k++) {
                if (line[k] == '=') { eq = k; break; }
            }
            if (eq < 0) { li = 0; continue; }
            line[eq] = '\0';
            const char *key = line + start;
            const char *val = line + eq + 1;
            if (strcmp(key, "target") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(cfg->target)) vl = sizeof(cfg->target) - 1;
                memcpy(cfg->target, val, vl);
                cfg->target[vl] = '\0';
            } else if (strcmp(key, "count") == 0) {
                uint32_t v = (uint32_t)local_atoi(val);
                if (v > 0 && v <= 100) cfg->count = v;
            } else if (strcmp(key, "timeout_ms") == 0) {
                uint32_t v = (uint32_t)local_atoi(val);
                if (v >= 100 && v <= 30000) cfg->timeout_ms = v;
            } else if (strcmp(key, "payload_len") == 0) {
                uint32_t v = (uint32_t)local_atoi(val);
                if (v <= 64) cfg->payload_len = v;
            }
            li = 0;
        } else {
            line[li++] = c;
        }
    }
}

static int resolve_target(libnet_client_ctx_t *ctx, const char *target,
                          uint32_t *out_ip) {
    if (netd_ipv4_parse_dotted(target, out_ip) == 0) return 0;
    libnet_dns_query_resp_t dns;
    int rc = libnet_dns_resolve(ctx, target, 5000, &dns);
    if (rc < 0) return rc;
    if (dns.answer_count == 0) return -2;
    *out_ip = dns.answers[0];
    return 0;
}

void _start(void) {
    ping_cfg_t cfg;
    cfg_defaults(&cfg);
    cfg_load(&cfg);

    libnet_client_ctx_t ctx;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE, 16,
                                                20000, &ctx);
    if (rc < 0) {
        printf("ping: cannot connect to /sys/net/service (rc=%d)\n", rc);
        syscall_exit(1);
    }

    uint32_t target_ip = 0;
    int rr = resolve_target(&ctx, cfg.target, &target_ip);
    if (rr < 0) {
        printf("ping: cannot resolve '%s' (rc=%d)\n", cfg.target, rr);
        syscall_exit(3);
    }
    printf("PING %s (%u.%u.%u.%u) %u data bytes\n",
           cfg.target,
           (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
           (target_ip >> 8) & 0xFF, target_ip & 0xFF,
           (unsigned)cfg.payload_len);

    uint8_t  payload[64];
    for (uint32_t i = 0; i < cfg.payload_len && i < sizeof(payload); i++) {
        payload[i] = (uint8_t)('A' + (i % 26u));
    }

    int replied = 0;
    for (uint16_t seq = 1; seq <= (uint16_t)cfg.count; seq++) {
        libnet_icmp_echo_resp_t resp;
        int er = libnet_icmp_echo(&ctx, target_ip, 0xBEEF, seq,
                                  payload, cfg.payload_len,
                                  cfg.timeout_ms, &resp);
        if (er == 0) {
            printf("  %u bytes from %u.%u.%u.%u seq=%u rtt=%lu us\n",
                   (unsigned)resp.payload_len,
                   (resp.src_ip >> 24) & 0xFF, (resp.src_ip >> 16) & 0xFF,
                   (resp.src_ip >> 8) & 0xFF, resp.src_ip & 0xFF,
                   (unsigned)seq,
                   (unsigned long)(resp.rtt_ns / 1000u));
            replied++;
        } else {
            printf("  seq=%u no reply (rc=%d status=%d)\n",
                   (unsigned)seq, er, (int)resp.status);
        }
    }
    printf("--- %s ping statistics ---\n", cfg.target);
    printf("%u packets transmitted, %d received, %d%% packet loss\n",
           (unsigned)cfg.count, replied,
           (int)(100u - (unsigned)replied * 100u / cfg.count));
    syscall_exit(replied > 0 ? 0 : 1);
}
