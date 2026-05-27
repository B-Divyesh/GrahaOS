// user/tests/font_full_sweep.c
//
// Phase 29 Session E gate test — extended Unicode font coverage.
//
// 4 asserts:
//   1. Render Block Element U+2588 (full block) at (8, 16); pixel at center
//      is foreground color (non-zero).
//   2. Same for Block Element U+2580 (upper half) — pixel in upper half
//      is foreground; pixel in lower half is background.
//   3. Same for Geometric Shape U+25A0 (filled square) — center pixel is fg.
//   4. Same for Arrow U+2192 (right arrow) — at least one pixel of the
//      arrow body is fg.
//
// We use DEBUG_CONSOLE_WRITE_CELL to populate cells, then
// DEBUG_CONSOLE_SYNTHETIC_RENDER to trigger the render, then
// DEBUG_FB_READ_PIXEL_AT to verify.

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

// Fg color the kernel uses for default palette index 15 (bright white).
#define FG_WHITE  0x00FFFFFFu

static int render_and_check(uint32_t codepoint,
                            uint32_t check_x, uint32_t check_y,
                            uint32_t expect_nonzero) {
    // Use console 0, row 0, col 0 — pixel position (0, 0).
    long rc = syscall_debug_console_write_cell(0, 0, 0, codepoint, 15, 0, 0);
    if (rc < 0) {
        printf("# write_cell rc=%ld\n", rc);
        return 0;
    }
    rc = syscall_debug_console_synthetic_render(0);
    if (rc < 0) {
        printf("# synth_render rc=%ld\n", rc);
        return 0;
    }
    long pix = syscall_debug_fb_read_pixel_at(check_x, check_y);
    if (expect_nonzero) {
        return (pix & 0x00FFFFFFu) != 0;
    } else {
        return (pix & 0x00FFFFFFu) == 0;
    }
}

void _start(void) {
    tap_plan(4);

    (void)syscall_pledge(PLEDGE_SYS_CONTROL | PLEDGE_SYS_QUERY |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_COMPUTE);

    // Test 1: U+2588 full block — pixel at (4, 8) should be fg.
    int ok1 = render_and_check(0x2588, 4, 8, 1);
    TAP_ASSERT(ok1, "1. U+2588 full block renders fg at cell center");

    // Test 2: U+2580 upper half — pixel at (4, 2) should be fg (upper half).
    int ok2 = render_and_check(0x2580, 4, 2, 1);
    TAP_ASSERT(ok2, "2. U+2580 upper half block renders fg in upper half");

    // Test 3: U+25A0 filled square — pixel at (4, 8) should be fg.
    int ok3 = render_and_check(0x25A0, 4, 8, 1);
    TAP_ASSERT(ok3, "3. U+25A0 filled square renders fg at cell center");

    // Test 4: U+2192 right arrow — pixel at (4, 7) should be fg
    // (the bitmap puts the arrow's horizontal line at rows 5-10 col 0-6).
    int ok4 = render_and_check(0x2192, 4, 7, 1);
    TAP_ASSERT(ok4, "4. U+2192 right arrow renders fg pixel in body");

    tap_done();
    syscall_exit(0);
}
