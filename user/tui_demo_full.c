// user/tui_demo_full.c
//
// Phase 29 Session E — TUI showcase demo.  Exercises every primitive added
// in Session D + E so the manual_verification can confirm visual behaviour.
//
// Demonstrates:
//   - libtui cell-VMO mapped writes
//   - tui_draw_box / tui_print
//   - SYS_CONSOLE_BEGIN_TX / COMMIT_TX (no-flicker redraw)
//   - sprite animation (4-frame bouncing pattern)
//   - PS/2 mouse event drain + cursor display
//   - 'q' to exit
//
// Run from gash:
//   gsh> tui_demo_full
// Use Alt+1..Alt+4 to switch between consoles (Phase 27 substrate).

#include "libtui/libtui.h"
#include "syscalls.h"

extern int printf(const char *fmt, ...);

static void draw_banner(uint32_t cid) {
    tui_draw_box(cid, 1, 2, 5, 60, /*fg=*/15, /*bg=*/0);
    tui_print(cid, 2, 4, 15, 0, 0, "GrahaOS Phase 29 — TUI Showcase");
    tui_print(cid, 3, 4, 7,  0, 0, "Session E: animation + mouse + atomic TX");
    tui_print(cid, 4, 4, 11, 0, 0, "Press 'q' to exit");
}

// Draw a "shaded card" via TX-then-commit so the user never sees a partial
// state.
static void draw_card_atomic(uint32_t cid) {
    int tx = tui_begin_tx(cid);
    if (tx < 1) {
        // TX unavailable; fall through with a direct draw.
        tui_draw_box_double(cid, 8, 2, 8, 50, 14, 0);
        tui_print(cid, 9, 4, 14, 0, 0, "[direct draw — TX unavailable]");
        return;
    }
    tui_draw_box_double(cid, 8, 2, 8, 50, 14, 0);
    tui_print(cid, 9,  4, 14, 0, 0, "+- Atomic Card --------------------+");
    tui_print(cid, 10, 4, 7,  0, 0, "  Written under SYS_CONSOLE_BEGIN_TX");
    tui_print(cid, 11, 4, 7,  0, 0, "  Committed via SYS_CONSOLE_COMMIT_TX");
    tui_print(cid, 12, 4, 7,  0, 0, "  No flicker, no torn frames.");
    (void)tui_commit_tx((uint32_t)tx);
}

// Register a 4-frame bouncing dot animation in sprite slot 200.  The kernel
// timer steps it on its own (60Hz cadence).
static void start_anim(uint32_t cid) {
    // Each "keyframe" is 16 bytes (8x16 1bpp glyph + tui_cell shape).
    static const uint8_t frames[4][16] = {
        // dot top-left
        {0xC0, 0xC0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        // dot top-right
        {0x03, 0x03, 0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        // dot bottom-right
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x03, 0x03},
        // dot bottom-left
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0xC0, 0xC0},
    };
    (void)tui_sprite_animate(cid, /*sprite_id=*/ 200, frames, 4,
                             /*dur_ms=*/ 250, /*interp=*/ 0);
    // Drop a sprite cell into row 18, col 6 referencing slot 200.
    (void)tui_write_cell(cid, 18, 6, 0xE100 + 200, 11, 0, 0);
}

void _start(void) {
    tui_init();
    uint32_t cid = 0;
    tui_attach(cid);
    tui_clear(cid, 7, 0);
    draw_banner(cid);
    draw_card_atomic(cid);
    start_anim(cid);
    tui_present(cid);

    // Event loop: read input + mouse, redraw cursor cell, watch for 'q'.
    uint32_t cursor_r = 5, cursor_c = 5;
    for (;;) {
        input_event_u_t evs[32];
        long n = tui_read_input(cid, (struct input_event_u *)evs, 32);
        long count = n & 0x3FFFFFFFFFFFFFFFLL;
        for (long i = 0; i < count; i++) {
            if (evs[i].kind == 0) {
                // keyboard scancode
                if (evs[i].key == 0x10) {  // scancode 0x10 = 'q'
                    tui_print(cid, 22, 2, 9, 0, 0, "Exiting...");
                    tui_present(cid);
                    syscall_exit(0);
                }
            } else if (evs[i].kind == 2) {
                // motion — update cursor cell
                cursor_c = (uint32_t)(cursor_c + evs[i].x_or_dx);
                cursor_r = (uint32_t)(cursor_r - evs[i].y_or_dy);
                if (cursor_c > 60) cursor_c = 60;
                if (cursor_r > 24) cursor_r = 24;
            } else if (evs[i].kind == 1) {
                // button — toggle a UI element via atomic TX.
                int tx = tui_begin_tx(cid);
                if (tx >= 1) {
                    tui_print(cid, 20, 4, 13, 0, 0, "[click] button pressed");
                    (void)tui_commit_tx((uint32_t)tx);
                }
            }
        }
        tui_set_cursor(cid, cursor_r, cursor_c);
        tui_present(cid);
        (void)tui_vsync_wait(16 * 1000 * 1000ull);  // ~16 ms
    }
}
