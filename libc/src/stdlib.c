// libc/src/stdlib.c
// Phase 22 closeout (G1.3): added atoi + strtol + strtoul.  Required by
// vendored Mongoose's URL/header parsing paths (linked but not exercised
// in TLS-only mode — but symbols must resolve).

#include <stdlib.h>
#include <stddef.h>

static int isdigit_local(int c) { return c >= '0' && c <= '9'; }
static int isspace_local(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s && isspace_local((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') {
        if (*s == '-') neg = 1;
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (base == 0) base = 16;
    } else if (base == 0 && *s == '0') {
        s++;
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    long acc = 0;
    while (*s) {
        int d;
        if (isdigit_local((unsigned char)*s)) {
            d = *s - '0';
        } else if (*s >= 'a' && *s <= 'z') {
            d = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'Z') {
            d = *s - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) break;
        acc = acc * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -acc : acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}
