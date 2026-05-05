// user/tests/console_gfx.c
//
// Phase 27 Block B — Stage B1 RGBA bitmap overlay gate test.
//
// Verifies:
//   1. SYS_CONSOLE_GFX_ENABLE allocates an overlay VMO and returns cap_idx.
//   2. DEBUG_CONSOLE_GFX_FILL writes pixel data into the overlay buffer.
//   3. SYS_CONSOLE_GFX_DAMAGE marks a rect dirty.
//   4. After synthetic render, the overlay rect's pixels match what was
//      written (composite path works).
//   5. Pixels OUTSIDE the damage rect are NOT updated by the overlay
//      (only damaged regions composite).

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    (void)syscall_console_switch(0);

    // 1. Enable overlay 64x64 px.
    long cap_idx = syscall_console_gfx_enable(/*console_id*/ 0,
                                              /*w_px*/ 64, /*h_px*/ 64);
    if (cap_idx <= 0) printf("# gfx_enable cap_idx=%ld\n", cap_idx);
    TAP_ASSERT(cap_idx > 0, "1. gfx_enable returns positive cap_idx");

    // 2. Fill 32x32 rect at (0,0) with red (0x00FF0000).
    long rc = syscall_debug_console_gfx_fill(/*console_id*/ 0,
                                             /*x*/ 0, /*y*/ 0,
                                             /*w*/ 32, /*h*/ 32,
                                             /*color*/ 0x00FF0000u);
    if (rc != 0) printf("# gfx_fill rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "2. gfx_fill writes pixel data");

    // 3. Damage the 32x32 rect we just filled.
    rc = syscall_console_gfx_damage(/*console_id*/ 0,
                                    /*x*/ 0, /*y*/ 0,
                                    /*w*/ 32, /*h*/ 32);
    if (rc != 0) printf("# gfx_damage rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "3. gfx_damage(0,0,32,32) returns 0");

    // First clear the cells region we want to verify the overlay overrides.
    // Write a known cell value at (row=0, col=0) which would put bg color
    // at pixel (0..7, 0..15). After synthetic render, the OVERLAY composite
    // (which runs AFTER cells) overwrites it with red.
    (void)syscall_debug_console_write_cell(0, 0, 0, ' ', 0, 0, 0);

    // 4. Synthetic render. Cells render first, then overlay composites
    // over the damaged rect.
    rc = syscall_debug_console_synthetic_render(0);
    (void)rc;
    uint32_t px_in = syscall_debug_fb_read_pixel(8, 8);
    if (px_in != 0x00FF0000u) printf("# px_in=0x%08x (expected red 0x00FF0000)\n",
                                     (unsigned)px_in);
    TAP_ASSERT(px_in == 0x00FF0000u,
               "4. overlay pixel within damage rect = filled color");

    // 5. Pixel OUTSIDE the 32x32 damage rect should NOT be red. Cell at
    // (row=0, col=5) has bg=0 (black), so pixel(40, 0) (well outside the
    // damage rect at (0,0)-(32,32)) should be black (cell bg) — not red.
    // First write the cell so its bg is deterministic.
    (void)syscall_debug_console_write_cell(0, 0, 5, ' ', 0, 0, 0);
    rc = syscall_debug_console_synthetic_render(0);
    (void)rc;
    uint32_t px_out = syscall_debug_fb_read_pixel(40, 0);
    if (px_out == 0x00FF0000u) {
        printf("# px_out=0x%08x — overlay leaked outside damage rect\n",
               (unsigned)px_out);
    }
    TAP_ASSERT(px_out != 0x00FF0000u,
               "5. pixel outside damage rect not affected by overlay");

    tap_done();
    syscall_exit(0);
}
