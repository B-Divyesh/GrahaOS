// kernel/vsnprintf.c
// Phase 13: kernel-space vsnprintf / snprintf. See vsnprintf.h.
#include "vsnprintf.h"

#include <stdint.h>

static void write_char(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos + 1 < cap) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void write_str(char *buf, size_t cap, size_t *pos,
                      const char *s, int max_len) {
    if (!s) s = "(null)";
    size_t i = 0;
    while (s[i] != '\0' && (max_len < 0 || (int)i < max_len)) {
        write_char(buf, cap, pos, s[i]);
        i++;
    }
}

static void write_uint(char *buf, size_t cap, size_t *pos,
                       uint64_t v, unsigned base) {
    char tmp[32];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        static const char digits[] = "0123456789abcdef";
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }
    while (n > 0) {
        write_char(buf, cap, pos, tmp[--n]);
    }
}

static void write_int(char *buf, size_t cap, size_t *pos, int64_t v) {
    if (v < 0) {
        write_char(buf, cap, pos, '-');
        // Unsigned negation handles INT64_MIN correctly.
        write_uint(buf, cap, pos, (uint64_t)(-(v + 1)) + 1, 10);
        return;
    }
    write_uint(buf, cap, pos, (uint64_t)v, 10);
}

static void write_ptr(char *buf, size_t cap, size_t *pos, uintptr_t p) {
    write_char(buf, cap, pos, '0');
    write_char(buf, cap, pos, 'x');
    for (int i = 0; i < 16; i++) {
        unsigned d = (unsigned)((p >> (60 - i * 4)) & 0xFu);
        char c = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        write_char(buf, cap, pos, c);
    }
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (!fmt) {
        if (cap > 0) buf[0] = '\0';
        return 0;
    }
    while (*fmt) {
        if (*fmt != '%') {
            write_char(buf, cap, &pos, *fmt++);
            continue;
        }
        fmt++;  // past '%'

        // Optional precision: %.*s, %.5s, etc. Only %s currently uses
        // precision; other conversions ignore it. Keep parsing minimal.
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        // Length modifier: l, ll. Both map to 64-bit on x86_64.
        int is_long = 0;
        while (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd': case 'i':
            if (is_long) {
                write_int(buf, cap, &pos, va_arg(ap, long));
            } else {
                write_int(buf, cap, &pos, va_arg(ap, int));
            }
            break;
        case 'u':
            if (is_long) {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned long), 10);
            } else {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned int), 10);
            }
            break;
        case 'x':
            if (is_long) {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned long), 16);
            } else {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned int), 16);
            }
            break;
        case 'p':
            write_ptr(buf, cap, &pos, (uintptr_t)va_arg(ap, void *));
            break;
        case 's':
            write_str(buf, cap, &pos, va_arg(ap, const char *), precision);
            break;
        case 'c':
            write_char(buf, cap, &pos, (char)va_arg(ap, int));
            break;
        case '%':
            write_char(buf, cap, &pos, '%');
            break;
        case '\0':
            goto done;
        default:
            // Unknown conversion — emit literally so the raw format is visible.
            write_char(buf, cap, &pos, '%');
            write_char(buf, cap, &pos, *fmt);
            break;
        }
        fmt++;
    }
done:
    if (cap > 0) {
        if (pos < cap) {
            buf[pos] = '\0';
        } else {
            buf[cap - 1] = '\0';
        }
    }
    return (int)pos;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
