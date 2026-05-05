#ifndef _INTTYPES_H
#define _INTTYPES_H

/*
 * Minimal C99 inttypes.h — printf/scanf format-string macros for the
 * fixed-width int types from <stdint.h>. Added for FU27.WASM Stage D0;
 * vendored wasm3 includes <inttypes.h> at the top of wasm3.h for
 * PRIu32/PRIi32/PRIu64/PRIi64.
 *
 * x86-64 SYSV: int=32, long=64. So:
 *   int32_t  = int          → "d"/"i", "u"
 *   int64_t  = long         → "ld"/"li", "lu"
 *   intmax_t = long
 *   intptr_t = long
 *
 * Only the conversion-specifier macros wasm3 (and m3_config_platforms.h)
 * actually reference are defined here. Adding the rest is mechanical when
 * we need them.
 */

#include <stdint.h>

/* d / i / o / u / x / X variants for the four widths. */
#define PRId8    "d"
#define PRIi8    "i"
#define PRIu8    "u"
#define PRIo8    "o"
#define PRIx8    "x"
#define PRIX8    "X"

#define PRId16   "d"
#define PRIi16   "i"
#define PRIu16   "u"
#define PRIo16   "o"
#define PRIx16   "x"
#define PRIX16   "X"

#define PRId32   "d"
#define PRIi32   "i"
#define PRIu32   "u"
#define PRIo32   "o"
#define PRIx32   "x"
#define PRIX32   "X"

#define PRId64   "ld"
#define PRIi64   "li"
#define PRIu64   "lu"
#define PRIo64   "lo"
#define PRIx64   "lx"
#define PRIX64   "lX"

/* Pointer / max-int / size-aligned variants. */
#define PRIdMAX  "ld"
#define PRIiMAX  "li"
#define PRIuMAX  "lu"
#define PRIoMAX  "lo"
#define PRIxMAX  "lx"
#define PRIXMAX  "lX"

#define PRIdPTR  "ld"
#define PRIiPTR  "li"
#define PRIuPTR  "lu"
#define PRIoPTR  "lo"
#define PRIxPTR  "lx"
#define PRIXPTR  "lX"

/* Scanf variants — same widths, included for completeness. */
#define SCNd8    "hhd"
#define SCNi8    "hhi"
#define SCNu8    "hhu"
#define SCNd16   "hd"
#define SCNi16   "hi"
#define SCNu16   "hu"
#define SCNd32   "d"
#define SCNi32   "i"
#define SCNu32   "u"
#define SCNd64   "ld"
#define SCNi64   "li"
#define SCNu64   "lu"

/* Note: wasm3's m3_config_platforms.h overrides some of these (e.g. "llu"
   for PRIu64 on platforms where long is 32 bits). On our x86-64 long is
   64 bits, so "lu" is correct — and m3_config_platforms.h's `# ifndef
   PRIu64` guard keeps us safe. */

#endif /* _INTTYPES_H */
