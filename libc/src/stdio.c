// libc/src/stdio.c
// Phase 7c: Standard I/O functions including full printf implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

// Syscall numbers (imported from kernel)
#define SYS_PUTC 1001
#define SYS_GETC 1006

// Forward declarations
static int format_output(char *buf, size_t buf_size, const char *format, va_list args);
static int print_string(char **buf, size_t *remaining, const char *str, int width, int precision, bool left_align);
static int print_number(char **buf, size_t *remaining, long long num, int base, bool uppercase, int width, int precision, char pad_char, bool sign, bool space, bool prefix, bool left_align);
static int print_char(char **buf, size_t *remaining, char c, int width, bool left_align);

// Syscall wrapper
static inline long syscall1(long n, long a1) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief Output a character to stdout
 */
int putchar(int c) {
    syscall1(SYS_PUTC, (long)(unsigned char)c);
    return c;
}

/**
 * @brief Output a string to stdout
 */
int puts(const char *s) {
    if (!s) {
        return EOF;
    }
    // Phase 15b console-atomicity fix: build the string-plus-newline in a
    // stack buffer and emit via one write() — keeps puts() output
    // contiguous on the UART under concurrent writers. Fallback to the
    // old per-char path only if the string is larger than the buffer.
    size_t len = 0;
    while (s[len]) len++;
    if (len + 1 <= 2048) {
        char line[2049];
        for (size_t i = 0; i < len; i++) line[i] = s[i];
        line[len] = '\n';
        (void)write(1, line, len + 1);
        return (int)(len + 1);
    }
    int count = 0;
    while (*s) {
        putchar(*s++);
        count++;
    }
    putchar('\n');
    return count + 1;
}

/**
 * @brief Input a character from stdin
 */
int getchar(void) {
    long result = syscall1(SYS_GETC, 0);
    if (result < 0) {
        return EOF;
    }
    return (int)result;
}

/**
 * @brief Helper: Write character to buffer or stdout
 */
static int write_char(char **buf, size_t *remaining, char c) {
    if (buf && *buf) {
        if (*remaining > 1) {
            **buf = c;
            (*buf)++;
            (*remaining)--;
            return 1;
        }
        return 0;
    } else {
        putchar(c);
        return 1;
    }
}

/**
 * @brief Helper: Write string to buffer or stdout
 */
static int write_string(char **buf, size_t *remaining, const char *str, size_t len) {
    int written = 0;
    for (size_t i = 0; i < len; i++) {
        if (write_char(buf, remaining, str[i])) {
            written++;
        } else {
            break;
        }
    }
    return written;
}

/**
 * @brief Print a character with width formatting
 */
static int print_char(char **buf, size_t *remaining, char c, int width, bool left_align) {
    int written = 0;

    if (!left_align && width > 1) {
        for (int i = 1; i < width; i++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    written += write_char(buf, remaining, c);

    if (left_align && width > 1) {
        for (int i = 1; i < width; i++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    return written;
}

/**
 * @brief Print a string with width and precision
 */
static int print_string(char **buf, size_t *remaining, const char *str, int width, int precision, bool left_align) {
    if (!str) {
        str = "(null)";
    }

    int len = strlen(str);
    if (precision >= 0 && precision < len) {
        len = precision;
    }

    int written = 0;
    int padding = (width > len) ? (width - len) : 0;

    if (!left_align) {
        for (int i = 0; i < padding; i++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    written += write_string(buf, remaining, str, len);

    if (left_align) {
        for (int i = 0; i < padding; i++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    return written;
}

/**
 * @brief Print a number with formatting
 */
static int print_number(char **buf, size_t *remaining, long long num, int base, bool uppercase,
                       int width, int precision, char pad_char, bool sign, bool space, bool prefix, bool left_align) {
    char digits[64];
    int i = 0;
    bool negative = false;
    unsigned long long unum;

    // Handle negative numbers
    if (num < 0 && base == 10) {
        negative = true;
        unum = -num;
    } else {
        unum = (unsigned long long)num;
    }

    // Convert to string
    const char *digit_chars = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (unum == 0) {
        digits[i++] = '0';
    } else {
        while (unum > 0) {
            digits[i++] = digit_chars[unum % base];
            unum /= base;
        }
    }

    // Apply precision (minimum digits)
    while (i < precision) {
        digits[i++] = '0';
    }

    // Calculate total width needed
    int num_len = i;
    int prefix_len = 0;

    if (negative || sign || space) {
        prefix_len = 1;
    } else if (prefix && base == 16) {
        prefix_len = 2;  // "0x"
    } else if (prefix && base == 8 && digits[i-1] != '0') {
        prefix_len = 1;  // "0"
    }

    int total_len = num_len + prefix_len;
    int padding = (width > total_len) ? (width - total_len) : 0;

    int written = 0;

    // Left padding (if not left-aligned and not zero-padded)
    if (!left_align && pad_char != '0') {
        for (int j = 0; j < padding; j++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    // Print prefix
    if (negative) {
        written += write_char(buf, remaining, '-');
    } else if (sign) {
        written += write_char(buf, remaining, '+');
    } else if (space) {
        written += write_char(buf, remaining, ' ');
    } else if (prefix && base == 16) {
        written += write_char(buf, remaining, '0');
        written += write_char(buf, remaining, uppercase ? 'X' : 'x');
    } else if (prefix && base == 8 && digits[i-1] != '0') {
        written += write_char(buf, remaining, '0');
    }

    // Zero padding (only if not left-aligned)
    if (!left_align && pad_char == '0') {
        for (int j = 0; j < padding; j++) {
            written += write_char(buf, remaining, '0');
        }
    }

    // Print digits (in reverse order)
    while (i > 0) {
        written += write_char(buf, remaining, digits[--i]);
    }

    // Right padding (if left-aligned)
    if (left_align) {
        for (int j = 0; j < padding; j++) {
            written += write_char(buf, remaining, ' ');
        }
    }

    return written;
}

/**
 * @brief Core printf formatter
 */
static int format_output(char *buf, size_t buf_size, const char *format, va_list args) {
    char *buf_ptr = buf;
    size_t remaining = buf_size;
    int written = 0;

    while (*format) {
        if (*format != '%') {
            written += write_char(buf ? &buf_ptr : NULL, buf ? &remaining : NULL, *format);
            format++;
            continue;
        }

        format++; // Skip '%'

        // Parse flags
        bool left_align = false;
        bool force_sign = false;
        bool space_sign = false;
        bool prefix = false;
        char pad_char = ' ';

        while (*format == '-' || *format == '+' || *format == ' ' || *format == '#' || *format == '0') {
            if (*format == '-') left_align = true;
            if (*format == '+') force_sign = true;
            if (*format == ' ') space_sign = true;
            if (*format == '#') prefix = true;
            if (*format == '0') pad_char = '0';
            format++;
        }

        // Parse width
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        // Parse precision
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        // Parse length modifiers
        int length = 0; // 0=int, 1=long, 2=long long
        if (*format == 'l') {
            format++;
            if (*format == 'l') {
                format++;
                length = 2;
            } else {
                length = 1;
            }
        } else if (*format == 'h') {
            format++;
            if (*format == 'h') {
                format++;
            }
        } else if (*format == 'z' || *format == 't') {
            format++;
            length = 1;
        }

        // Parse conversion specifier
        switch (*format) {
            case 'd':
            case 'i': {
                long long num;
                if (length == 2) num = va_arg(args, long long);
                else if (length == 1) num = va_arg(args, long);
                else num = va_arg(args, int);
                written += print_number(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       num, 10, false, width, precision, pad_char, force_sign, space_sign, false, left_align);
                break;
            }
            case 'u': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += print_number(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       (long long)num, 10, false, width, precision, pad_char, false, false, false, left_align);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += print_number(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       (long long)num, 16, *format == 'X', width, precision, pad_char, false, false, prefix, left_align);
                break;
            }
            case 'o': {
                unsigned long long num;
                if (length == 2) num = va_arg(args, unsigned long long);
                else if (length == 1) num = va_arg(args, unsigned long);
                else num = va_arg(args, unsigned int);
                written += print_number(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       (long long)num, 8, false, width, precision, pad_char, false, false, prefix, left_align);
                break;
            }
            case 'p': {
                void *ptr = va_arg(args, void *);
                written += print_number(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       (long long)(uintptr_t)ptr, 16, false, width, precision, pad_char, false, false, true, left_align);
                break;
            }
            case 's': {
                const char *str = va_arg(args, const char *);
                written += print_string(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                       str, width, precision, left_align);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                written += print_char(buf ? &buf_ptr : NULL, buf ? &remaining : NULL,
                                     c, width, left_align);
                break;
            }
            case '%': {
                written += write_char(buf ? &buf_ptr : NULL, buf ? &remaining : NULL, '%');
                break;
            }
            default:
                written += write_char(buf ? &buf_ptr : NULL, buf ? &remaining : NULL, *format);
                break;
        }

        format++;
    }

    // Null terminate if writing to buffer
    if (buf && buf_size > 0) {
        *buf_ptr = '\0';
    }

    return written;
}

/**
 * @brief Formatted output to stdout
 */
// Phase 15b console-atomicity fix: printf used to call putchar per byte
// (SYS_PUTC per char), which meant concurrent processes' output interleaved
// on the shared console. Instead, format into a stack buffer and emit with
// ONE syscall_write — the kernel's per-SYS_WRITE serial lock now keeps each
// printf call contiguous on the UART. 2 KiB covers virtually all practical
// output; excess is truncated (same behaviour as snprintf under-size).
int printf(const char *format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    int would_have = format_output(line, sizeof(line), format, args);
    va_end(args);
    int n = (would_have < 0) ? 0 :
            ((size_t)would_have >= sizeof(line) ? (int)sizeof(line) - 1 : would_have);
    if (n > 0) {
        (void)write(1, line, (size_t)n);
    }
    return would_have;
}

/**
 * @brief Formatted output to buffer
 */
int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = format_output(str, SIZE_MAX, format, args);
    va_end(args);
    return written;
}

/**
 * @brief Formatted output to buffer with size limit
 */
int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = format_output(str, size, format, args);
    va_end(args);
    return written;
}

/**
 * @brief Variadic formatted output to stdout
 */
int vprintf(const char *format, va_list ap) {
    char line[2048];
    int would_have = format_output(line, sizeof(line), format, ap);
    int n = (would_have < 0) ? 0 :
            ((size_t)would_have >= sizeof(line) ? (int)sizeof(line) - 1 : would_have);
    if (n > 0) {
        (void)write(1, line, (size_t)n);
    }
    return would_have;
}

/**
 * @brief Variadic formatted output to buffer
 */
int vsprintf(char *str, const char *format, va_list ap) {
    return format_output(str, SIZE_MAX, format, ap);
}

/**
 * @brief Variadic formatted output to buffer with size limit
 */
int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    return format_output(str, size, format, ap);
}
