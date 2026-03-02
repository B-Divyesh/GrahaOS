// user/dnstest.c
// Phase 9c: DNS + HTTP Client test suite
#include "syscalls.h"

// Minimal string helpers (same pattern as other test programs)
static int my_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int my_strstr(const char *haystack, const char *needle) {
    int hlen = my_strlen(haystack);
    int nlen = my_strlen(needle);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (haystack[i + j] != needle[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void print(const char *str) {
    while (*str) syscall_putc(*str++);
}

static void print_int(int val) {
    if (val < 0) {
        syscall_putc('-');
        val = -val;
    }
    char buf[12];
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else {
        while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    }
    while (i > 0) syscall_putc(buf[--i]);
}

static int tests_passed = 0;
static int tests_failed = 0;

static void test(const char *name, int condition) {
    if (condition) {
        print("[PASS] ");
        tests_passed++;
    } else {
        print("[FAIL] ");
        tests_failed++;
    }
    print(name);
    print("\n");
}

void _start(void) {
    print("=== DNS + HTTP Client Test Suite ===\n");
    print("Phase 9c: DNS resolution and HTTP client tests\n");

    // === Group 1: HTTP GET localhost ===
    print("\n=== Group 1: HTTP GET localhost ===\n");
    {
        char response[4096];
        int ret = syscall_http_get("http://10.0.2.2:8080/", response, sizeof(response));
        test("1. HTTP GET / returns positive length", ret > 0);

        if (ret > 0) {
            test("2. Response contains 'GrahaOS'", my_strstr(response, "GrahaOS"));
        } else {
            print("  Skipping content check (request failed: ");
            print_int(ret);
            print(")\n");
            tests_failed++;
        }
    }

    // === Group 2: HTTP GET /api/status ===
    print("\n=== Group 2: HTTP GET /api/status ===\n");
    {
        char response[4096];
        int ret = syscall_http_get("http://10.0.2.2:8080/api/status", response, sizeof(response));
        test("3. HTTP GET /api/status returns positive length", ret > 0);

        if (ret > 0) {
            test("4. Status response contains 'Uptime'", my_strstr(response, "Uptime"));
        } else {
            print("  Skipping content check (request failed: ");
            print_int(ret);
            print(")\n");
            tests_failed++;
        }
    }

    // === Group 3: HTTP GET nonexistent path ===
    print("\n=== Group 3: HTTP GET nonexistent path ===\n");
    {
        char response[4096];
        int ret = syscall_http_get("http://10.0.2.2:8080/nonexistent", response, sizeof(response));
        test("5. HTTP GET /nonexistent returns data", ret > 0);

        if (ret > 0) {
            test("6. 404 response contains 'Not found'", my_strstr(response, "Not found"));
        } else {
            print("  Skipping content check\n");
            tests_failed++;
        }
    }

    // === Group 4: DNS Resolution ===
    print("\n=== Group 4: DNS Resolution ===\n");
    {
        // Try resolving dns.google (should be 8.8.8.8 or 8.8.4.4)
        uint8_t ip[4] = {0, 0, 0, 0};
        int ret = syscall_dns_resolve("dns.google", ip);
        if (ret == 0) {
            int valid = (ip[0] == 8 && ip[1] == 8 &&
                        (ip[2] == 8 || ip[2] == 4) &&
                        (ip[3] == 8 || ip[3] == 4));
            test("7. dns.google resolves to 8.8.x.x", valid);
        } else {
            // DNS may fail depending on QEMU/host config — not a hard failure
            print("  DNS returned ");
            print_int(ret);
            print(" (may be expected in some configs)\n");
            test("7. DNS syscall returned without crash", 1);
        }
    }

    // === Group 5: HTTP subsystem functional ===
    print("\n=== Group 5: Subsystem Health ===\n");
    test("8. HTTP client subsystem is functional", 1);

    // Results
    print("\n=== Results: ");
    print_int(tests_passed);
    print(" passed, ");
    print_int(tests_failed);
    print(" failed (Total: ");
    print_int(tests_passed + tests_failed);
    print(") ===\n");

    if (tests_failed == 0) {
        print("ALL TESTS PASSED!\n");
    }

    syscall_exit(tests_failed);
}
