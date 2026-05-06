// user/tui_anim.c
//
// FU27.X.tui_demo_apps: simple sprite-overlay animation demo. NOT in
// gate; manual verification via `gash> tui_anim`. Exercises:
//   - SYS_CONSOLE_GFX_ENABLE (allocates RGBA overlay VMO)
//   - DEBUG_CONSOLE_GFX_FILL (writes pixel data into the overlay)
//   - SYS_CONSOLE_GFX_DAMAGE (marks rect dirty so composite picks it up)
//   - DEBUG_CONSOLE_SYNTHETIC_RENDER (kernel-side cells+overlay composite)
//
// Animation: 16x16 px box that bounces around inside a 320x200 overlay
// region. Demonstrates the FU27.X.alpha_blend code path (S1.B): the
// box's color cycles through alpha levels so the user can see the
// blending against the underlying cell text.

#include "syscalls.h"
#include "libtui/libtui.h"

#include <stdint.h>

#define CID         0u
#define OV_W        320u
#define OV_H        200u
#define BOX_SIZE    16u

// Tiny user-side spin-loop to slow the animation down enough that a human
// can see it. Each iteration is roughly a syscall-and-back; ~50000 iters
// gives a perceptible frame interval.
static void spin(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {
        asm volatile("pause" ::: "memory");
    }
}

void _start(void) {
    if (tui_init() < 0) syscall_exit(1);
    if (tui_attach(CID) < 0) syscall_exit(2);

    // Background: write a status message so the box bounces over text.
    (void)tui_clear(CID, 7, 0);
    (void)tui_print(CID, 1, 2, 15, 0, 0,
                    "tui_anim - sprite overlay test (FU27.X.alpha_blend)");
    (void)tui_print(CID, 3, 2, 11, 0, 0,
                    "Box bounces; alpha cycles so blending is visible.");
    (void)tui_present(CID);

    // Enable the gfx overlay region.
    long cap = syscall_console_gfx_enable(CID, OV_W, OV_H);
    if (cap <= 0) syscall_exit(3);

    // Bouncing box state.
    int32_t x = 16, y = 32;
    int32_t vx = 4, vy = 3;
    uint32_t frame = 0;

    // 240-frame run (~enough to demo the bounce + alpha cycle visibly,
    // then we exit cleanly so gash regains the prompt).
    while (frame < 240) {
        // Clear previous box position (paint transparent over old area).
        // Using alpha=0 for a no-op composite; instead we redraw the
        // background by re-presenting cells (cheap in synthetic-render).
        // Simpler: paint the previous box-area with bg color (0x00000000).
        if (frame > 0) {
            (void)syscall_debug_console_gfx_fill(CID,
                (uint32_t)x, (uint32_t)y, BOX_SIZE, BOX_SIZE,
                0x00000000u);
            (void)syscall_console_gfx_damage(CID,
                (uint32_t)x, (uint32_t)y, BOX_SIZE, BOX_SIZE);
        }

        // Advance.
        x += vx;
        y += vy;
        if (x <= 0 || (uint32_t)(x + BOX_SIZE) >= OV_W) vx = -vx;
        if (y <= 0 || (uint32_t)(y + BOX_SIZE) >= OV_H) vy = -vy;
        if (x < 0) x = 0;
        if (y < 0) y = 0;

        // Cycle alpha across frames so the blending behaviour is
        // observable. Hue cycles too so the visual is more interesting.
        uint8_t a = (uint8_t)(64 + (frame & 0x7F));
        uint8_t r = (uint8_t)((frame * 5) & 0xFF);
        uint8_t g = (uint8_t)(0xFF - r);
        uint8_t b = 0x80;
        uint32_t color = ((uint32_t)a << 24) | ((uint32_t)r << 16)
                       | ((uint32_t)g << 8) | (uint32_t)b;

        (void)syscall_debug_console_gfx_fill(CID,
            (uint32_t)x, (uint32_t)y, BOX_SIZE, BOX_SIZE, color);
        (void)syscall_console_gfx_damage(CID,
            (uint32_t)x, (uint32_t)y, BOX_SIZE, BOX_SIZE);
        (void)syscall_debug_console_synthetic_render(CID);

        spin(120000);
        frame++;
    }

    // Final clear so the prompt isn't obscured.
    (void)syscall_debug_console_gfx_fill(CID, 0, 0, OV_W, OV_H, 0x00000000u);
    (void)syscall_console_gfx_damage(CID, 0, 0, OV_W, OV_H);

    // FU27.X.tui_demo_apps polish: q-to-quit so user can read the final
    // frame instead of returning instantly to gash. Blocking getc loops
    // until the user types 'q' (any other keypress is consumed silently).
    (void)tui_print(CID, 5, 2, 11, 0, 0, "Animation done.  Press 'q' to exit.");
    (void)tui_present(CID);
    while (syscall_getc() != 'q') { /* swallow non-q keys */ }

    syscall_exit(0);
}
