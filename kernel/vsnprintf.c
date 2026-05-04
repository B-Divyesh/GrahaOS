// kernel/vsnprintf.c
// Phase 13: kernel-space vsnprintf / snprintf. See vsnprintf.h.
// Phase 26 (FU26.C): width + flags parser. Existing call sites without width
// specs see no behaviour change; new callers can use %04x / %5d / %-10s / %08x.
#include "vsnprintf.h"

#include <stdint.h>

// FU26.C: format-spec flags. Bit field consumed by the write_* helpers.
#define FLAG_ZERO_PAD   (1u << 0)   // '0' — pad with zeros (default ' ')
#define FLAG_LEFT       (1u << 1)   // '-' — left align (overrides ZERO_PAD)
#define FLAG_PLUS       (1u << 2)   // '+' — force sign on signed conversions
#define FLAG_SPACE      (1u << 3)   // ' ' — space prefix on non-negative signed

static void write_char(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos + 1 < cap) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void write_pad(char *buf, size_t cap, size_t *pos, char pad, int n) {
    for (int i = 0; i < n; i++) {
        write_char(buf, cap, pos, pad);
    }
}

// strlen for width-padding decisions. Caller may pass max < 0 for unbounded.
static int bounded_strlen(const char *s, int max) {
    int n = 0;
    while (s[n] != '\0' && (max < 0 || n < max)) n++;
    return n;
}

// Width-aware string emit. ZERO_PAD is ignored for strings (always space).
static void write_str(char *buf, size_t cap, size_t *pos,
                      const char *s, int max_len, int width, unsigned flags) {
    if (!s) s = "(null)";
    int s_len = bounded_strlen(s, max_len);
    int pad_n = (width > s_len) ? (width - s_len) : 0;
    if (pad_n > 0 && !(flags & FLAG_LEFT)) {
        write_pad(buf, cap, pos, ' ', pad_n);
    }
    for (int i = 0; i < s_len; i++) {
        write_char(buf, cap, pos, s[i]);
    }
    if (pad_n > 0 && (flags & FLAG_LEFT)) {
        write_pad(buf, cap, pos, ' ', pad_n);
    }
}

// Compute digit string for `v` in `base` into tmp (LSB first). Returns count.
static int format_uint_digits(uint64_t v, unsigned base, char *tmp, int tmp_size) {
    int n = 0;
    if (v == 0) {
        if (n < tmp_size) tmp[n++] = '0';
    } else {
        static const char digits[] = "0123456789abcdef";
        while (v > 0 && n < tmp_size) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }
    return n;
}

// Width-aware unsigned emit.
static void write_uint(char *buf, size_t cap, size_t *pos,
                       uint64_t v, unsigned base, int width, unsigned flags) {
    char tmp[32];
    int n = format_uint_digits(v, base, tmp, (int)sizeof(tmp));
    int body_len = n;
    int pad_n = (width > body_len) ? (width - body_len) : 0;
    char pad_char = ((flags & FLAG_ZERO_PAD) && !(flags & FLAG_LEFT)) ? '0' : ' ';
    if (pad_n > 0 && !(flags & FLAG_LEFT)) {
        write_pad(buf, cap, pos, pad_char, pad_n);
    }
    while (n > 0) {
        write_char(buf, cap, pos, tmp[--n]);
    }
    if (pad_n > 0 && (flags & FLAG_LEFT)) {
        write_pad(buf, cap, pos, ' ', pad_n);
    }
}

// Width-aware signed emit. Sign character (if any) is part of body length.
static void write_int(char *buf, size_t cap, size_t *pos,
                      int64_t v, int width, unsigned flags) {
    char sign_ch = 0;
    uint64_t abs_v;
    if (v < 0) {
        sign_ch = '-';
        // Unsigned negation handles INT64_MIN correctly.
        abs_v = (uint64_t)(-(v + 1)) + 1;
    } else {
        abs_v = (uint64_t)v;
        if (flags & FLAG_PLUS)       sign_ch = '+';
        else if (flags & FLAG_SPACE) sign_ch = ' ';
    }
    char tmp[32];
    int n = format_uint_digits(abs_v, 10, tmp, (int)sizeof(tmp));
    int body_len = n + (sign_ch ? 1 : 0);
    int pad_n = (width > body_len) ? (width - body_len) : 0;
    int zero_pad_active = (flags & FLAG_ZERO_PAD) && !(flags & FLAG_LEFT);

    if (pad_n > 0 && !(flags & FLAG_LEFT) && !zero_pad_active) {
        // Space-pad before sign + digits.
        write_pad(buf, cap, pos, ' ', pad_n);
    }
    if (sign_ch) write_char(buf, cap, pos, sign_ch);
    if (pad_n > 0 && zero_pad_active) {
        // Zero-pad goes between sign and digits.
        write_pad(buf, cap, pos, '0', pad_n);
    }
    while (n > 0) {
        write_char(buf, cap, pos, tmp[--n]);
    }
    if (pad_n > 0 && (flags & FLAG_LEFT)) {
        write_pad(buf, cap, pos, ' ', pad_n);
    }
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

        // FU26.C: Flags. 0/-/+/space, in any order, zero or more.
        unsigned flags = 0;
        while (1) {
            if (*fmt == '0') { flags |= FLAG_ZERO_PAD; fmt++; continue; }
            if (*fmt == '-') { flags |= FLAG_LEFT;     fmt++; continue; }
            if (*fmt == '+') { flags |= FLAG_PLUS;     fmt++; continue; }
            if (*fmt == ' ') { flags |= FLAG_SPACE;    fmt++; continue; }
            break;
        }

        // FU26.C: Width — digits, or '*' (consumes one va_arg).
        int width = 0;
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            if (w < 0) { flags |= FLAG_LEFT; width = -w; }
            else       { width = w; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        // Optional precision: %.*s, %.5s, etc. Used by %s; ignored elsewhere.
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
                write_int(buf, cap, &pos, va_arg(ap, long), width, flags);
            } else {
                write_int(buf, cap, &pos, va_arg(ap, int), width, flags);
            }
            break;
        case 'u':
            if (is_long) {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned long), 10, width, flags);
            } else {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned int), 10, width, flags);
            }
            break;
        case 'x':
            if (is_long) {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned long), 16, width, flags);
            } else {
                write_uint(buf, cap, &pos, va_arg(ap, unsigned int), 16, width, flags);
            }
            break;
        case 'p':
            // %p ignores width/flags (fixed format).
            write_ptr(buf, cap, &pos, (uintptr_t)va_arg(ap, void *));
            break;
        case 's':
            write_str(buf, cap, &pos, va_arg(ap, const char *),
                      precision, width, flags);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad_n = (width > 1) ? (width - 1) : 0;
            if (pad_n > 0 && !(flags & FLAG_LEFT)) {
                write_pad(buf, cap, &pos, ' ', pad_n);
            }
            write_char(buf, cap, &pos, ch);
            if (pad_n > 0 && (flags & FLAG_LEFT)) {
                write_pad(buf, cap, &pos, ' ', pad_n);
            }
            break;
        }
        case '%':
            write_char(buf, cap, &pos, '%');
            break;
        case '\0':
            goto done;
        default:
            // Unknown conversion — emit literally so the raw format is visible.
            // FU26.C: width/flags were already consumed; va_args are NOT
            // misaligned (which was the FU26.A bug).
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
