// kernel/vsnprintf.h
// Phase 13: minimal kernel-space formatted output for klog and panic paths.
// FU26.C (Phase 26 closeout): width + flags parser added. Float still off.
//
// Supported conversions:
//   %d %ld      signed int / long
//   %u %lu      unsigned int / long
//   %x %lx      unsigned hex
//   %p          pointer, always "0x" + 16 hex digits (width ignored)
//   %s          null-terminated string
//   %.*s        precision-bounded string (klog_write uses this)
//   %c          char
//   %%          literal percent
//
// FU26.C width/flags subset:
//   %5d         right-align in field of width 5 (space pad)
//   %-5d        left-align in field of width 5 (space pad on right)
//   %04x        zero-padded 4-digit hex
//   %08x        zero-padded 8-digit hex
//   %02u        zero-padded 2-digit unsigned (used by rtc.c timestamps)
//   %+d         force sign on non-negative
//   % d         space prefix on non-negative
//   %*d         width consumed from va_arg (negative width = left-align)
//   any combination (e.g. %-+5d, %05ld, %-10s)
//
// No floats, no length specifiers beyond `l/ll` (both 64-bit), no `%n`.
// Width and flags before unrecognised conversion type still consume the
// va_args correctly, so subsequent slots don't slip (FU26.A trap fixed).
#pragma once

#include <stdarg.h>
#include <stddef.h>

// Format into a bounded buffer. Truncates at cap-1 and always
// null-terminates (when cap > 0). Returns the number of bytes the
// full formatted output would have consumed (excluding the null
// terminator), same convention as libc vsnprintf.
int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
