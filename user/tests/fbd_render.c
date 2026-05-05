// user/tests/fbd_render.c
//
// Phase 27 Block A — Stage A4 gate test for the cell→pixel render path.
//
// Validates the kernel-side substrate that fbd uses (Stage A5 will replace
// this with a userspace blit via libtui+font). Under autorun=ktest, fbd is
// NOT spawned; instead the test:
//   1. Switches to console 0 and writes a known cell ('A' white-on-black at
//      (row=0, col=0)) via DEBUG_CONSOLE_WRITE_CELL.
//   2. Triggers DEBUG_CONSOLE_SYNTHETIC_RENDER on console 0. Kernel walks
//      the cell-VMO and calls framebuffer_force_draw_cell for each cell;
//      framebuffer_force_draw_cell bypasses g_fbd_alive so this works
//      without fbd running.
//   3. Reads back framebuffer pixels via DEBUG_FB_READ_PIXEL at known
//      foreground/background pixel positions for the 'A' glyph and verifies
//      they match the expected ARGB values.
//
// The 'A' glyph (drivers/video/framebuffer.c font_8x16[33]) row pattern:
//   row 3 = 0x18 = 0b00011000  → cols 3 and 4 of the cell are foreground
//   row 0 = 0x00                → all background
//
// We use:
//   fg = palette index 15 (white = 0x00FFFFFF)
//   bg = palette index 0  (black = 0x00000000)
// Palette table lives in kernel/console/console.c::console_render_synthetic_frame.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(6);

    // Baseline: be on console 0 so synthetic render targets the right buffer.
    syscall_console_switch(0);
    uint32_t sel = syscall_debug_console_get_selected();
    if (sel != 0) printf("# baseline: selected != 0 (got %u)\n", sel);
    TAP_ASSERT(sel == 0, "1. console 0 is selected after console_switch(0)");

    // Write 'A' at row 0, col 0 with fg=white(15), bg=black(0).
    long rc = syscall_debug_console_write_cell(/*console_id*/ 0,
                                                /*row*/ 0, /*col*/ 0,
                                                /*codepoint*/ 'A',
                                                /*fg*/ 15, /*bg*/ 0,
                                                /*attrs*/ 0);
    if (rc != 0) printf("# write_cell rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "2. write_cell(console=0, row=0, col=0, 'A') returns 0");

    // Trigger kernel-side composite. Renders all cells of console 0 into the
    // framebuffer via framebuffer_force_draw_cell (bypasses g_fbd_alive).
    rc = syscall_debug_console_synthetic_render(0);
    if (rc != 0) printf("# synthetic_render rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "3. synthetic_render(0) returns 0");

    // Verify pixel at the very top-left of the 'A' cell is the BACKGROUND
    // color (black). 'A' glyph row 0 = 0x00 → no foreground pixels in row 0.
    uint32_t px_bg = syscall_debug_fb_read_pixel(0, 0);
    if (px_bg != 0x00000000u) printf("# px(0,0)=0x%08x (expected 0x00000000)\n",
                                     (unsigned)px_bg);
    TAP_ASSERT(px_bg == 0x00000000u, "4. pixel(0,0) is background (black)");

    // Verify pixel at (3, 3) is the FOREGROUND color (white).
    // 'A' glyph row 3 = 0x18 = 0b00011000 → col 3 is set (bit 0x10 of 0x80>>3).
    uint32_t px_fg = syscall_debug_fb_read_pixel(3, 3);
    if (px_fg != 0x00FFFFFFu) printf("# px(3,3)=0x%08x (expected 0x00FFFFFF)\n",
                                     (unsigned)px_fg);
    TAP_ASSERT(px_fg == 0x00FFFFFFu, "5. pixel(3,3) is foreground (white)");

    // Verify pixel at (0, 3) is BACKGROUND (col 0 of row 3 has bit clear:
    // 0x80 & 0x18 = 0). Confirms the glyph mask is honored, not just a flat
    // fill at fg color across rows that have ANY pixels lit.
    uint32_t px_bg2 = syscall_debug_fb_read_pixel(0, 3);
    if (px_bg2 != 0x00000000u) printf("# px(0,3)=0x%08x (expected 0x00000000)\n",
                                      (unsigned)px_bg2);
    TAP_ASSERT(px_bg2 == 0x00000000u, "6. pixel(0,3) is background (mask honored)");

    tap_done();
    syscall_exit(0);
}
