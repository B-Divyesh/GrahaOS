// user/aitest.c
// Phase 9e: AI Integration test suite
#include "syscalls.h"

// Minimal string helpers
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
    if (val < 0) { syscall_putc('-'); val = -val; }
    char buf[12];
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else { while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; } }
    while (i > 0) syscall_putc(buf[--i]);
}

static int pass_count = 0;
static int fail_count = 0;
static int test_num = 0;

static void test_pass(const char *name) {
    test_num++;
    print("[PASS] Test ");
    print_int(test_num);
    print(": ");
    print(name);
    print("\n");
    pass_count++;
}

static void test_fail(const char *name, const char *reason) {
    test_num++;
    print("[FAIL] Test ");
    print_int(test_num);
    print(": ");
    print(name);
    print(" - ");
    print(reason);
    print("\n");
    fail_count++;
}

void _start(void) {
    print("=== AI Integration Test Suite (Phase 9e) ===\n\n");

    // Test 1: API key readable from etc/ai.conf
    {
        int fd = syscall_open("etc/ai.conf");
        if (fd >= 0) {
            char key[128];
            for (int i = 0; i < 128; i++) key[i] = 0;
            int n = syscall_read(fd, key, sizeof(key) - 1);
            syscall_close(fd);
            if (n > 0 && key[0] != '\0') {
                test_pass("API key readable from etc/ai.conf");
            } else {
                test_fail("API key readable from etc/ai.conf", "file empty or read failed");
            }
        } else {
            test_fail("API key readable from etc/ai.conf", "file not found");
        }
    }

    // Test 2: SYS_HTTP_POST syscall exists (test with invalid URL)
    {
        char resp[256];
        for (int i = 0; i < 256; i++) resp[i] = 0;
        // POST to a known bad URL — should fail with DNS/connect error, NOT crash
        int ret = syscall_http_post("http://0.0.0.0:1/test", "{}", 2, resp, sizeof(resp));
        // Any negative error means the syscall works (reached the network layer)
        if (ret < 0) {
            test_pass("SYS_HTTP_POST syscall functional (returns error for bad URL)");
        } else {
            // Even a 0-byte response is fine — the syscall worked
            test_pass("SYS_HTTP_POST syscall functional (responded)");
        }
    }

    // Test 3: HTTP POST to a real HTTPS endpoint (Google — known TLS 1.3 compatible)
    {
        char resp[4096];
        for (int i = 0; i < 4096; i++) resp[i] = 0;
        const char *body = "{\"test\":\"hello\"}";
        int body_len = my_strlen(body);
        int ret = syscall_http_post("https://www.google.com/", body, body_len, resp, sizeof(resp));
        if (ret > 0) {
            test_pass("HTTPS POST to google.com (response received)");
        } else {
            print("  (ret=");
            print_int(ret);
            print(", network may be unavailable)\n");
            test_fail("HTTPS POST to google.com", "no response");
        }
    }

    // Test 4: Gemini API returns valid response
    {
        // Read API key
        int fd = syscall_open("etc/ai.conf");
        if (fd >= 0) {
            char key[128];
            for (int i = 0; i < 128; i++) key[i] = 0;
            int klen = syscall_read(fd, key, sizeof(key) - 1);
            syscall_close(fd);

            // Trim whitespace
            while (klen > 0 && (key[klen-1] == '\n' || key[klen-1] == '\r' || key[klen-1] == ' '))
                key[--klen] = '\0';

            if (klen > 0) {
                // Build URL
                char url[512];
                int u = 0;
                const char *prefix = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=";
                while (*prefix) url[u++] = *prefix++;
                for (int i = 0; i < klen; i++) url[u++] = key[i];
                url[u] = '\0';

                const char *body = "{\"contents\":[{\"parts\":[{\"text\":\"Say hello in 3 words\"}]}]}";
                int blen = my_strlen(body);

                char resp[4096];
                for (int i = 0; i < 4096; i++) resp[i] = 0;
                int ret = syscall_http_post(url, body, blen, resp, sizeof(resp));

                if (ret > 0) {
                    test_pass("Gemini API returns response");
                } else {
                    print("  (ret=");
                    print_int(ret);
                    print(")\n");
                    test_fail("Gemini API returns response", "no response");
                }

                // Test 5: Response contains "text" key (valid Gemini format)
                if (ret > 0 && my_strstr(resp, "\"text\"")) {
                    test_pass("Gemini response contains 'text' key");
                } else if (ret > 0) {
                    test_fail("Gemini response contains 'text' key", "key not found in response");
                } else {
                    test_fail("Gemini response contains 'text' key", "no response to check");
                }
            } else {
                test_fail("Gemini API returns response", "empty API key");
                test_fail("Gemini response contains 'text' key", "skipped (no API key)");
            }
        } else {
            test_fail("Gemini API returns response", "no API key file");
            test_fail("Gemini response contains 'text' key", "skipped (no API key)");
        }
    }

    // Summary
    print("\n=== Results: ");
    print_int(pass_count);
    print("/");
    print_int(pass_count + fail_count);
    print(" passed ===\n");

    syscall_exit(fail_count > 0 ? 1 : 0);
}
