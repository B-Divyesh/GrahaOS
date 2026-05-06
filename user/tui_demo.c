// user/tui_demo.c
//
// FU27.X.tui_demo_apps: simple boxed-windows demo for libtui. NOT in
// gate; manual verification via `gash> tui_demo`. Exercises:
//   - tui_init / tui_attach / tui_clear
//   - tui_draw_box (single-line) + tui_draw_box_double (double-line)
//   - tui_print with 256-color palette
//   - Heavy-line variants from FU27.X.font_full (S1.C subset)
//   - Block elements (▀ ▄ █ ░ ▒ ▓)
//   - tui_present (synthetic render via kernel composite path)
//
// Layout: outer double-line frame (entire console), inner single-line
// title box, body region with 16-color palette swatch + heavy-line
// inner card.

#include "syscalls.h"
#include "libtui/libtui.h"

#include <stdint.h>

#define COLS  80
#define ROWS  25
#define CID   0u

// xterm 256-color palette indices for the demo.
#define COL_BG          0   // black
#define COL_FRAME       11  // bright yellow
#define COL_TITLE       15  // bright white
#define COL_TEXT        7   // light grey
#define COL_HEAVY       9   // bright red
#define COL_BLOCK_OK    10  // bright green
#define COL_BLOCK_WARN  11  // bright yellow
#define COL_BLOCK_ERR   9   // bright red

void _start(void) {
    if (tui_init() < 0) syscall_exit(1);
    if (tui_attach(CID) < 0) syscall_exit(2);
    (void)tui_clear(CID, COL_TEXT, COL_BG);

    // Outer double-line frame around the entire console.
    (void)tui_draw_box_double(CID, 0, 0, ROWS, COLS,
                              COL_FRAME, COL_BG);

    // Title bar (single-line box across top, row 1..3).
    (void)tui_draw_box(CID, 1, 2, 3, COLS - 4,
                       COL_TITLE, COL_BG);
    (void)tui_print(CID, 2, 4, COL_TITLE, COL_BG, 0,
                    "GrahaOS libtui demo  -  Phase 27 / FU27.X polish");

    // Body subtitle.
    (void)tui_print(CID, 5, 4, COL_TEXT, COL_BG, 0,
                    "Layout exercise: boxes + colored palette + block elements.");

    // 16-color palette swatch (rows 7-8). Each cell is 2 columns wide
    // for visibility; uses U+2588 full block.
    (void)tui_print(CID, 7, 4, COL_TEXT, COL_BG, 0,
                    "Palette:");
    for (uint8_t i = 0; i < 16; i++) {
        uint32_t c = 14 + (uint32_t)i * 4;
        (void)tui_write_cell(CID, 7, c,     TUI_BLOCK_FULL, i, COL_BG, 0);
        (void)tui_write_cell(CID, 7, c + 1, TUI_BLOCK_FULL, i, COL_BG, 0);
    }

    // Heavy-line inner card (FU27.X.font_full subset). Uses U+250F/2513/
    // 2517/251B corners + U+2501/2503 sides.
    uint32_t hr = 11, hc = 6;
    (void)tui_write_cell(CID, hr,     hc,      0x250F, COL_HEAVY, COL_BG, 0);
    (void)tui_write_cell(CID, hr,     hc + 30, 0x2513, COL_HEAVY, COL_BG, 0);
    (void)tui_write_cell(CID, hr + 6, hc,      0x2517, COL_HEAVY, COL_BG, 0);
    (void)tui_write_cell(CID, hr + 6, hc + 30, 0x251B, COL_HEAVY, COL_BG, 0);
    for (uint32_t c = hc + 1; c < hc + 30; c++) {
        (void)tui_write_cell(CID, hr,     c, 0x2501, COL_HEAVY, COL_BG, 0);
        (void)tui_write_cell(CID, hr + 6, c, 0x2501, COL_HEAVY, COL_BG, 0);
    }
    for (uint32_t r = hr + 1; r < hr + 6; r++) {
        (void)tui_write_cell(CID, r, hc,      0x2503, COL_HEAVY, COL_BG, 0);
        (void)tui_write_cell(CID, r, hc + 30, 0x2503, COL_HEAVY, COL_BG, 0);
    }
    (void)tui_print(CID, hr + 1, hc + 2, COL_TITLE, COL_BG, 0,
                    "Heavy-line card");
    (void)tui_print(CID, hr + 3, hc + 2, COL_TEXT, COL_BG, 0,
                    "Shade samples:");
    (void)tui_write_cell(CID, hr + 3, hc + 17, TUI_BLOCK_LIGHT,  COL_BLOCK_OK,   COL_BG, 0);
    (void)tui_write_cell(CID, hr + 3, hc + 19, TUI_BLOCK_MEDIUM, COL_BLOCK_WARN, COL_BG, 0);
    (void)tui_write_cell(CID, hr + 3, hc + 21, TUI_BLOCK_DARK,   COL_BLOCK_ERR,  COL_BG, 0);
    (void)tui_write_cell(CID, hr + 3, hc + 23, TUI_BLOCK_FULL,   COL_TITLE,      COL_BG, 0);

    // Footer.
    (void)tui_print(CID, ROWS - 2, 4, COL_TEXT, COL_BG, 0,
                    "Press Alt+1..Alt+4 to switch consoles.");
    (void)tui_print(CID, ROWS - 2, 46, COL_TEXT, COL_BG, 0,
                    "Press 'q' to exit.");

    (void)tui_present(CID);

    // FU27.X.tui_demo_apps polish: block on input so user can examine the
    // demo before control returns to gash.
    while (syscall_getc() != 'q') { /* swallow non-q keys */ }

    syscall_exit(0);
}
