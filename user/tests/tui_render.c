// user/tests/tui_render.c
//
// Phase 27 Block A — Stage A5 gate test for libtui + font extension.
//
// Verifies that:
//   1. tui_draw_box() with U+250C/U+2518 corners produces expected pixel
//      patterns at the box corners after synthetic render.
//   2. 256-color palette index 232 (dim grey, RGB 0x080808) maps to that
//      ARGB at render time.
//   3. 256-color palette index 231 (end of 6x6x6 RGB cube, RGB 0xFFFFFF)
//      maps to white at render time.
//   4. TUI_ATTR_CURSOR inverts fg/bg in the rendered pixel.
//
// Each assertion writes a known cell, presents the console (synthetic
// render via DEBUG_CONSOLE_SYNTHETIC_RENDER), and reads the framebuffer
// pixel via DEBUG_FB_READ_PIXEL. The kernel-side palette mirror lives in
// kernel/console/console.c::palette256_build; libtui mirrors it at
// libtui/libtui.c::palette_build.

#include "../libtap.h"
#include "../syscalls.h"
#include "../libtui/libtui.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    (void)tui_init();
    (void)syscall_console_switch(0);
    (void)tui_attach(0);

    // 1. Box-drawing — top-left corner glyph at (5, 2) (cell coords). The
    // ┌ glyph has col 4 row 8 lit (corner intersection). After synthetic
    // render, framebuffer pixel(2*8 + 4, 5*16 + 8) should be foreground.
    int rc = tui_draw_box(/*console_id*/ 0, /*row*/ 5, /*col*/ 2,
                          /*height*/ 4, /*width*/ 6,
                          /*fg*/ 15, /*bg*/ 0);
    (void)rc;
    (void)tui_present(0);
    uint32_t px_corner = syscall_debug_fb_read_pixel(2*8 + 4, 5*16 + 8);
    if (px_corner != 0x00FFFFFFu) {
        printf("# tl_corner pixel(%u,%u)=0x%08x (expected 0x00FFFFFF)\n",
               (unsigned)(2*8 + 4), (unsigned)(5*16 + 8),
               (unsigned)px_corner);
    }
    TAP_ASSERT(px_corner == 0x00FFFFFFu,
               "1. U+250C ┌ corner pixel matches foreground");

    // 2. Palette idx 232 = dim grey 0x080808.
    rc = tui_write_cell(0, /*row*/ 10, /*col*/ 0,
                        /*codepoint*/ ' ',
                        /*fg*/ 0, /*bg*/ 232, /*attrs*/ 0);
    (void)rc;
    (void)tui_present(0);
    uint32_t px_grey = syscall_debug_fb_read_pixel(0, 10*16 + 0);
    uint32_t expected_grey = tui_palette_lookup(232);
    if (px_grey != expected_grey) {
        printf("# palette[232] pixel=0x%08x expected=0x%08x\n",
               (unsigned)px_grey, (unsigned)expected_grey);
    }
    TAP_ASSERT(px_grey == 0x00080808u && expected_grey == 0x00080808u,
               "2. palette[232] = 0x00080808 (dim grey)");

    // 3. Palette idx 231 = end-of-cube (255,255,255).
    rc = tui_write_cell(0, /*row*/ 11, /*col*/ 0,
                        ' ', /*fg*/ 0, /*bg*/ 231, /*attrs*/ 0);
    (void)rc;
    (void)tui_present(0);
    uint32_t px_cube_white = syscall_debug_fb_read_pixel(0, 11*16 + 0);
    uint32_t expected_white = tui_palette_lookup(231);
    if (px_cube_white != expected_white) {
        printf("# palette[231] pixel=0x%08x expected=0x%08x\n",
               (unsigned)px_cube_white, (unsigned)expected_white);
    }
    TAP_ASSERT(px_cube_white == 0x00FFFFFFu && expected_white == 0x00FFFFFFu,
               "3. palette[231] = 0x00FFFFFF (cube end)");

    // 4. Cursor attribute inverts fg/bg. Cell with fg=0(black), bg=15(white)
    // and CURSOR set should render as black background (since the swap
    // makes original-fg into rendered-bg, i.e. bg=0=black, and the space
    // glyph paints no foreground pixels).
    rc = tui_write_cell(0, /*row*/ 12, /*col*/ 0,
                        ' ', /*fg*/ 0, /*bg*/ 15, /*attrs*/ TUI_ATTR_CURSOR);
    (void)rc;
    (void)tui_present(0);
    uint32_t px_cursor = syscall_debug_fb_read_pixel(0, 12*16 + 0);
    if (px_cursor != 0x00000000u) {
        printf("# cursor pixel=0x%08x (expected 0x00000000 after invert)\n",
               (unsigned)px_cursor);
    }
    TAP_ASSERT(px_cursor == 0x00000000u,
               "4. TUI_ATTR_CURSOR inverts fg/bg");

    tap_done();
    syscall_exit(0);
}
