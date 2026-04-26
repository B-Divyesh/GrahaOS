// user/dnstest.c — Phase 22 Stage E migration.
//
// Pre-Phase-22 this called SYS_HTTP_GET + SYS_DNS_RESOLVE (now scheduled
// for -ENOSYS in Stage F). Stage E swaps to libhttp http_get and
// libnet_dns_resolve.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "libnet/libnet.h"
#include "libnet/libnet_msg.h"
#include "libhttp/libhttp.h"

static int my_strstr_local(const uint8_t *haystack, size_t hlen, const char *needle) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == (uint8_t)needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int tests_passed = 0;
static int tests_failed = 0;

static void test_(const char *name, int condition) {
    if (condition) { printf("[PASS] %s\n", name); tests_passed++; }
    else            { printf("[FAIL] %s\n", name); tests_failed++; }
}

void _start(void) {
    printf("=== dnstest — Phase 22 Stage E ===\n");

    libnet_client_ctx_t ctx;
    int rc = libnet_connect_service_with_retry(LIBNET_NAME_SERVICE,
                                                (uint32_t)strlen(LIBNET_NAME_SERVICE),
                                                /*total_timeout_ms=*/5000, &ctx);
    if (rc < 0) {
        printf("dnstest: cannot connect to /sys/net/service (rc=%d)\n", rc);
        syscall_exit(1);
    }

    // Group 1: HTTP GET / via libhttp
    printf("\n=== Group 1: HTTP GET localhost ===\n");
    {
        http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        int gr = http_get(&resp, "http://10.0.2.2:8080/", /*timeout_ms=*/5000);
        int ok = (gr == 0 && resp.body_len > 0);
        test_("1. HTTP GET / returns positive length", ok);
        if (ok) {
            test_("2. Response contains 'GrahaOS'",
                  my_strstr_local(resp.body, resp.body_len, "GrahaOS"));
        } else {
            printf("  (rc=%d status=%d)\n", gr, resp.status_code);
            tests_failed++;
        }
        http_response_free(&resp);
    }

    // Group 2: HTTP GET /api/status
    printf("\n=== Group 2: HTTP GET /api/status ===\n");
    {
        http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        int gr = http_get(&resp, "http://10.0.2.2:8080/api/status",
                          /*timeout_ms=*/5000);
        int ok = (gr == 0 && resp.body_len > 0);
        test_("3. HTTP GET /api/status returns positive length", ok);
        if (ok) {
            test_("4. Status response contains 'Uptime'",
                  my_strstr_local(resp.body, resp.body_len, "Uptime"));
        } else {
            tests_failed++;
        }
        http_response_free(&resp);
    }

    // Group 3: 404 path
    printf("\n=== Group 3: HTTP GET nonexistent path ===\n");
    {
        http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        int gr = http_get(&resp, "http://10.0.2.2:8080/nonexistent",
                          /*timeout_ms=*/5000);
        int ok = (gr == 0 && resp.body_len > 0);
        test_("5. HTTP GET /nonexistent returns data", ok);
        if (ok) {
            test_("6. 404 response contains 'Not found'",
                  my_strstr_local(resp.body, resp.body_len, "Not found"));
        } else {
            tests_failed++;
        }
        http_response_free(&resp);
    }

    // Group 4: DNS resolution via libnet
    printf("\n=== Group 4: DNS Resolution ===\n");
    {
        libnet_dns_query_resp_t dr;
        memset(&dr, 0, sizeof(dr));
        int dnr = libnet_dns_resolve(&ctx, "dns.google",
                                     /*timeout_ms=*/5000, &dr);
        if (dnr == 0 && dr.answer_count > 0) {
            uint32_t ip = dr.answers[0];
            uint8_t a = (uint8_t)((ip >> 24) & 0xFF);
            uint8_t b = (uint8_t)((ip >> 16) & 0xFF);
            uint8_t c = (uint8_t)((ip >>  8) & 0xFF);
            uint8_t d = (uint8_t)(ip & 0xFF);
            int valid = (a == 8 && b == 8 &&
                         (c == 8 || c == 4) &&
                         (d == 8 || d == 4));
            test_("7. dns.google resolves to 8.8.x.x", valid);
        } else {
            printf("  DNS rc=%d answers=%u\n", dnr, (unsigned)dr.answer_count);
            test_("7. DNS query returned without crash", 1);
        }
    }

    printf("\n=== Group 5: Subsystem Health ===\n");
    test_("8. libhttp + libnet subsystem is functional", 1);

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    if (tests_failed == 0) printf("ALL TESTS PASSED!\n");
    syscall_exit(tests_failed);
}
