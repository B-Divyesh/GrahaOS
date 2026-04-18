// kernel/vsnprintf.h
// Phase 13: minimal kernel-space formatted output for klog and panic paths.
//
// Scope is deliberately narrow. Supported conversions:
//   %d %ld      signed int / long
//   %u %lu      unsigned int / long
//   %x %lx      unsigned hex
//   %p          pointer, always "0x" + 16 hex digits
//   %s          null-terminated string
//   %.*s        precision-bounded string (klog_write uses this)
//   %c          char
//   %%          literal percent
//
// No width flags, no padding, no floats — keep the binary small and the
// format path re-entrant enough that kpanic can rely on it.
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
