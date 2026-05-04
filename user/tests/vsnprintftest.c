// user/tests/vsnprintftest.c
//
// FU26.C: gate test for the kernel vsnprintf parser. Each TAP_ASSERT calls
// ksnprintf via SYS_DEBUG / DEBUG_VSNPRINTF and compares the output to the
// expected string. Covers width, flags, precision, and the va_arg-alignment
// invariant (the FU26.A trap).
//
// Pre-FU26.C:
//   "%04x" of 0xab    →  "%0%4ab"  (default branch emits %0%4 literally)
//   "%5d"  of 42      →  "%542"    (same)
// Post-FU26.C:
//   "%04x" of 0xab    →  "00ab"
//   "%5d"  of 42      →  "   42"

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern int   strcmp(const char *, const char *);

static int run_fmt_eq(const char *label, const char *fmt,
                      uint64_t a1, uint64_t a2,
                      const char *expected) {
    char out[256];
    memset(out, 0xAA, sizeof(out));   // sentinel — kernel must overwrite
    long n = syscall_debug_vsnprintf(fmt, a1, a2, out);
    int ok = (n >= 0) && (strcmp(out, expected) == 0);
    if (!ok) {
        printf("# %s: fmt=%s expected=\"%s\" got=\"%s\" n=%ld\n",
               label, fmt, expected, out, n);
    }
    return ok;
}

void _start(void) {
    tap_plan(15);

    // 1: plain %d non-negative.
    TAP_ASSERT(run_fmt_eq("plain_d", "%d", 42, 0, "42"),
               "1. %d of 42 -> \"42\"");

    // 2: plain %d negative.
    TAP_ASSERT(run_fmt_eq("plain_d_neg", "%d", (uint64_t)(int64_t)-7, 0, "-7"),
               "2. %d of -7 -> \"-7\"");

    // 3: %u of large unsigned.
    TAP_ASSERT(run_fmt_eq("plain_u", "%u", 0xFFFFFFFFull, 0, "4294967295"),
               "3. %u of 0xFFFFFFFF -> \"4294967295\"");

    // 4: plain %x.
    TAP_ASSERT(run_fmt_eq("plain_x", "%x", 0xab, 0, "ab"),
               "4. %x of 0xab -> \"ab\"");

    // 5: width + zero-pad — the FU26.A workaround case.
    TAP_ASSERT(run_fmt_eq("zeropad_x", "%04x", 0xab, 0, "00ab"),
               "5. %04x of 0xab -> \"00ab\" (width + zero-pad)");

    // 6: width + space-pad (right align).
    TAP_ASSERT(run_fmt_eq("spacepad_d", "%5d", 42, 0, "   42"),
               "6. %5d of 42 -> \"   42\" (width + space-pad)");

    // 7: width + left align.
    TAP_ASSERT(run_fmt_eq("leftalign_d", "%-5d", 42, 0, "42   "),
               "7. %-5d of 42 -> \"42   \" (left align)");

    // 8: width met by content (no padding emitted).
    TAP_ASSERT(run_fmt_eq("width_met", "%08x", 0xdeadbeef, 0, "deadbeef"),
               "8. %08x of 0xdeadbeef -> \"deadbeef\" (width met)");

    // 9: width exceeds content (zero-pad fills the gap).
    TAP_ASSERT(run_fmt_eq("width_over", "%012x", 0xdeadbeef, 0, "0000deadbeef"),
               "9. %012x of 0xdeadbeef -> \"0000deadbeef\"");

    // 10: plain %s.
    TAP_ASSERT(run_fmt_eq("plain_s", "%s", (uint64_t)(uintptr_t)"hi", 0, "hi"),
               "10. %s of \"hi\" -> \"hi\"");

    // 11: %s with precision (existing path; no width regression).
    TAP_ASSERT(run_fmt_eq("prec_s", "%.2s",
                          (uint64_t)(uintptr_t)"hello", 0, "he"),
               "11. %.2s of \"hello\" -> \"he\"");

    // 12: %s with width + left align.
    TAP_ASSERT(run_fmt_eq("leftpad_s", "%-10s",
                          (uint64_t)(uintptr_t)"hi", 0, "hi        "),
               "12. %-10s of \"hi\" -> \"hi        \" (10 chars total)");

    // 13: literal %%.
    TAP_ASSERT(run_fmt_eq("literal_pct", "[%%]", 0, 0, "[%]"),
               "13. \"[%%]\" -> \"[%]\"");

    // 14: %c.
    TAP_ASSERT(run_fmt_eq("char", "%c", (uint64_t)'A', 0, "A"),
               "14. %c of 'A' -> \"A\"");

    // 15: mixed %d %s — verifies va_args don't slip a slot when format
    //     contains both a width-spec call and a pointer call. This is
    //     the regression test for FU26.A.
    TAP_ASSERT(run_fmt_eq("mixed", "n=%04x str=%s",
                          0xab, (uint64_t)(uintptr_t)"ok",
                          "n=00ab str=ok"),
               "15. \"n=%04x str=%s\" + (0xab, \"ok\") -> \"n=00ab str=ok\"");

    tap_done();
    syscall_exit(0);
}
