// kernel/net/klib.c
// Phase 9b: Kernel string/format library for Mongoose
// Adapted from libc/src/string.c and libc/src/stdio.c
// NOTE: memcpy, memset, memmove, memcmp, strlen are already global in kernel/main.c

#include "klib.h"
#include "kmalloc.h"
#include <stdint.h>
#include <stdbool.h>

// Use kernel's serial for abort
extern void serial_write(const char *str);

// errno stub for Mongoose
int mg_errno = 0;

// ===== STRING FUNCTIONS =====

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    if (ch == '\0') return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    if (ch == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    extern size_t strlen(const char *);
    if (*needle == '\0') return (char *)haystack;
    size_t needle_len = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle) {
            if (strncmp(haystack, needle, needle_len) == 0)
                return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    if (n == (size_t)-1) *d = '\0';
    return dest;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t val = (uint8_t)c;
    while (n--) {
        if (*p == val) return (void *)p;
        p++;
    }
    return NULL;
}

char *strdup(const char *s) {
    extern size_t strlen(const char *);
    extern void *memcpy(void *, const void *, size_t);
    size_t len = strlen(s) + 1;
    char *dup = kmalloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

// ===== FORMAT FUNCTIONS =====
// Adapted from libc/src/stdio.c (buffer-only path, no syscall I/O)

static int write_char_buf(char **buf, size_t *remaining, char c) {
    if (*remaining > 1) {
        **buf = c;
        (*buf)++;
        (*remaining)--;
        return 1;
    }
    return 0;
}

static int write_string_buf(char **buf, size_t *remaining, const char *str, size_t len) {
    int written = 0;
    for (size_t i = 0; i < len; i++) {
        if (write_char_buf(buf, remaining, str[i]))
            written++;
        else
            break;
    }
    return written;
}

static int fmt_char(char **buf, size_t *remaining, char c, int width, bool left_align) {
    int written = 0;
    if (!left_align && width > 1)
        for (int i = 1; i < width; i++)
            written += write_char_buf(buf, remaining, ' ');
    written += write_char_buf(buf, remaining, c);
    if (left_align && width > 1)
        for (int i = 1; i < width; i++)
            written += write_char_buf(buf, remaining, ' ');
    return written;
}

static int fmt_string(char **buf, size_t *remaining, const char *str, int width, int precision, bool left_align) {
    extern size_t strlen(const char *);
    if (!str) str = "(null)";
    int len = (int)strlen(str);
    if (precision >= 0 && precision < len) len = precision;
    int written = 0;
    int padding = (width > len) ? (width - len) : 0;
    if (!left_align)
        for (int i = 0; i < padding; i++)
            written += write_char_buf(buf, remaining, ' ');
    written += write_string_buf(buf, remaining, str, (size_t)len);
    if (left_align)
        for (int i = 0; i < padding; i++)
            written += write_char_buf(buf, remaining, ' ');
    return written;
}

static int fmt_number(char **buf, size_t *remaining, long long num, int base,
                      bool uppercase, int width, int precision, char pad_char,
                      bool sign, bool space, bool prefix, bool left_align) {
    char digits[64];
    int i = 0;
    bool negative = false;
    unsigned long long unum;

    if (num < 0 && base == 10) {
        negative = true;
        unum = (unsigned long long)(-(num + 1)) + 1;
    } else {
        unum = (unsigned long long)num;
    }

    const char *digit_chars = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (unum == 0) {
        digits[i++] = '0';
    } else {
        while (unum > 0) {
            digits[i++] = digit_chars[unum % (unsigned)base];
            unum /= (unsigned)base;
        }
    }

    while (i < precision) digits[i++] = '0';

    int num_len = i;
    int prefix_len = 0;
    if (negative || sign || space) prefix_len = 1;
    else if (prefix && base == 16) prefix_len = 2;
    else if (prefix && base == 8 && digits[i - 1] != '0') prefix_len = 1;

    int total_len = num_len + prefix_len;
    int padding = (width > total_len) ? (width - total_len) : 0;
    int written = 0;

    if (!left_align && pad_char != '0')
        for (int j = 0; j < padding; j++)
            written += write_char_buf(buf, remaining, ' ');

    if (negative) written += write_char_buf(buf, remaining, '-');
    else if (sign) written += write_char_buf(buf, remaining, '+');
    else if (space) written += write_char_buf(buf, remaining, ' ');
    else if (prefix && base == 16) {
        written += write_char_buf(buf, remaining, '0');
        written += write_char_buf(buf, remaining, uppercase ? 'X' : 'x');
    } else if (prefix && base == 8 && digits[i - 1] != '0')
        written += write_char_buf(buf, remaining, '0');

    if (!left_align && pad_char == '0')
        for (int j = 0; j < padding; j++)
            written += write_char_buf(buf, remaining, '0');

    while (i > 0) written += write_char_buf(buf, remaining, digits[--i]);

    if (left_align)
        for (int j = 0; j < padding; j++)
            written += write_char_buf(buf, remaining, ' ');

    return written;
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    char *buf_ptr = str;
    size_t remaining = size;
    int written = 0;

    while (*format) {
        if (*format != '%') {
            written += write_char_buf(&buf_ptr, &remaining, *format);
            format++;
            continue;
        }
        format++;  // skip '%'

        // Flags
        bool left_align = false, force_sign = false, space_sign = false;
        bool hash_prefix = false;
        char pad_char = ' ';
        while (*format == '-' || *format == '+' || *format == ' ' || *format == '#' || *format == '0') {
            if (*format == '-') left_align = true;
            if (*format == '+') force_sign = true;
            if (*format == ' ') space_sign = true;
            if (*format == '#') hash_prefix = true;
            if (*format == '0') pad_char = '0';
            format++;
        }

        // Width
        int width = 0;
        if (*format == '*') {
            width = va_arg(args, int);
            if (width < 0) { left_align = true; width = -width; }
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }
        }

        // Precision
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            if (*format == '*') {
                precision = va_arg(args, int);
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
        }

        // Length modifier
        int length = 0;
        if (*format == 'l') {
            format++;
            if (*format == 'l') { format++; length = 2; }
            else length = 1;
        } else if (*format == 'h') {
            format++;
            if (*format == 'h') format++;
        } else if (*format == 'z' || *format == 't') {
            format++;
            length = 1;
        }

        // Conversion
        switch (*format) {
            case 'd': case 'i': {
                long long num;
                if (length == 2) num = va_arg(args, long long);
                else if (length == 1) num = va_arg(args, long);
                else num = va_arg(args, int);
                written += fmt_number(&buf_ptr, &remaining, num, 10, false,
                                      width, precision, pad_char, force_sign, space_sign, false, left_align);
                break;
            }
            case 'u': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += fmt_number(&buf_ptr, &remaining, (long long)num, 10, false,
                                      width, precision, pad_char, false, false, false, left_align);
                break;
            }
            case 'x': case 'X': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += fmt_number(&buf_ptr, &remaining, (long long)num, 16,
                                      *format == 'X', width, precision, pad_char,
                                      false, false, hash_prefix, left_align);
                break;
            }
            case 'o': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += fmt_number(&buf_ptr, &remaining, (long long)num, 8, false,
                                      width, precision, pad_char, false, false, hash_prefix, left_align);
                break;
            }
            case 'p': {
                void *ptr = va_arg(args, void *);
                written += fmt_number(&buf_ptr, &remaining, (long long)(uintptr_t)ptr,
                                      16, false, width, precision, pad_char,
                                      false, false, true, left_align);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                written += fmt_string(&buf_ptr, &remaining, s, width, precision, left_align);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                written += fmt_char(&buf_ptr, &remaining, c, width, left_align);
                break;
            }
            case '%':
                written += write_char_buf(&buf_ptr, &remaining, '%');
                break;
            default:
                written += write_char_buf(&buf_ptr, &remaining, *format);
                break;
        }
        format++;
    }

    if (str && size > 0) *buf_ptr = '\0';
    return written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(str, size, format, args);
    va_end(args);
    return written;
}

// ===== CONVERSION FUNCTIONS =====

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return result * sign;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long result = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return result;
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

// ===== MISC =====

void abort(void) {
    serial_write("[ABORT] kernel abort called!\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}
