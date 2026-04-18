// scripts/test_vsnprintf.c
// Host-side unit test for kernel/vsnprintf.c. Builds on the host and
// checks kvsnprintf output matches libc snprintf for every format Phase 13
// actually uses. Run from Makefile or directly:
//   gcc -I.. -I../kernel scripts/test_vsnprintf.c ../kernel/vsnprintf.c \
//       -o /tmp/test_vsnprintf && /tmp/test_vsnprintf
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../kernel/vsnprintf.h"

static int g_fails;
static int g_total;

static void check(const char *label, const char *got, const char *expected) {
    g_total++;
    if (strcmp(got, expected) == 0) {
        printf("ok %d - %s\n", g_total, label);
    } else {
        printf("not ok %d - %s: got=<%s> expected=<%s>\n",
               g_total, label, got, expected);
        g_fails++;
    }
}

#define CHECK_FMT(label, expected, fmt, ...) do {           \
    char buf[256];                                          \
    ksnprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);        \
    check(label, buf, expected);                            \
} while (0)

int main(void) {
    CHECK_FMT("plain literal",         "hello",               "hello");
    CHECK_FMT("empty",                 "",                    "");
    CHECK_FMT("percent percent",       "50%",                 "50%%");

    CHECK_FMT("%d positive",           "42",                  "%d", 42);
    CHECK_FMT("%d negative",           "-7",                  "%d", -7);
    CHECK_FMT("%d zero",               "0",                   "%d", 0);
    CHECK_FMT("%d INT_MIN",            "-2147483648",         "%d", (int)(-2147483647 - 1));

    CHECK_FMT("%ld long",              "1234567890",          "%ld", 1234567890L);
    CHECK_FMT("%ld negative long",     "-9876543210",         "%ld", -9876543210L);

    CHECK_FMT("%u unsigned",           "4294967295",          "%u", 4294967295u);
    CHECK_FMT("%lu unsigned long",     "18446744073709551615","%lu", 18446744073709551615ul);

    CHECK_FMT("%x hex",                "deadbeef",            "%x", 0xdeadbeefu);
    CHECK_FMT("%lx long hex",          "cafef00dcafef00d",    "%lx", 0xcafef00dcafef00dul);

    CHECK_FMT("%p",                    "0x00007fff12345678",  "%p", (void *)0x7fff12345678ul);
    CHECK_FMT("%p null",               "0x0000000000000000",  "%p", (void *)0);

    CHECK_FMT("%s",                    "world",               "%s", "world");
    CHECK_FMT("%s null",               "(null)",              "%s", (const char *)NULL);
    CHECK_FMT("%.*s precision",        "hel",                 "%.*s", 3, "hello");
    CHECK_FMT("%.5s precision const",  "hello",               "%.5s", "hello world");

    CHECK_FMT("%c",                    "A",                   "%c", 'A');

    CHECK_FMT("mixed",
              "pid=12 addr=0xffffffff80100000 name=kinit",
              "pid=%d addr=%p name=%s", 12,
              (void *)0xffffffff80100000ul, "kinit");

    // Truncation behaviour: when the formatted output would exceed the
    // buffer, ksnprintf must null-terminate at cap-1 and return the full
    // would-have-been length.
    {
        char small[8];
        int n = ksnprintf(small, sizeof(small), "abcdefghij");
        g_total++;
        if (n == 10 && strcmp(small, "abcdefg") == 0) {
            printf("ok %d - truncation\n", g_total);
        } else {
            printf("not ok %d - truncation: got=<%s> n=%d\n",
                   g_total, small, n);
            g_fails++;
        }
    }

    // Unknown conversion should emit literally.
    CHECK_FMT("unknown conversion",    "%q",                  "%q");

    if (g_fails == 0) {
        printf("1..%d\n", g_total);
        printf("# all %d kvsnprintf cases passed\n", g_total);
        return 0;
    }
    printf("# %d / %d kvsnprintf cases FAILED\n", g_fails, g_total);
    return 1;
}
