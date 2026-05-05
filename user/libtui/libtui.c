// user/libtui/libtui.c
//
// Phase 27 Block A (Stage A5) — libtui implementation.
//
// Minimal ncurses-like userspace primitives over the Phase 27 console
// subsystem. See libtui.h for the public surface and the substrate note
// about DEBUG_CONSOLE_WRITE_CELL → SYS_CONSOLE_ATTACH migration in C2.

#include "libtui.h"
#include "../syscalls.h"

// Library-private state. Stage A5 keeps this minimal — Stage C2 grows it
// with cell-VMO mappings + input-channel handles.
typedef struct {
    int32_t  attached_console;     // -1 if not attached
    uint32_t cursor_row;
    uint32_t cursor_col;
    uint8_t  cursor_active;        // 1 once tui_set_cursor has been called
} libtui_state_t;

static libtui_state_t g_state = {
    .attached_console = -1,
    .cursor_row = 0,
    .cursor_col = 0,
    .cursor_active = 0,
};

// 256-color palette. Build lazily on first use to avoid a static
// constructor (we have no .init_array runner in this freestanding
// userspace yet). Mirrors kernel/console/console.c::palette256_build.
static uint32_t g_palette[256];
static int      g_palette_built = 0;

static void palette_build(void) {
    if (g_palette_built) return;
    static const uint32_t vga16[16] = {
        0x00000000u, 0x00800000u, 0x00008000u, 0x00808000u,
        0x00000080u, 0x00800080u, 0x00008080u, 0x00C0C0C0u,
        0x00808080u, 0x00FF0000u, 0x0000FF00u, 0x00FFFF00u,
        0x000000FFu, 0x00FF00FFu, 0x0000FFFFu, 0x00FFFFFFu,
    };
    for (uint32_t i = 0; i < 16; i++) g_palette[i] = vga16[i];
    static const uint8_t cube_levels[6] = { 0, 95, 135, 175, 215, 255 };
    for (uint32_t r = 0; r < 6; r++) {
        for (uint32_t g = 0; g < 6; g++) {
            for (uint32_t b = 0; b < 6; b++) {
                uint32_t idx = 16 + 36*r + 6*g + b;
                g_palette[idx] =
                    ((uint32_t)cube_levels[r] << 16) |
                    ((uint32_t)cube_levels[g] << 8)  |
                    (uint32_t)cube_levels[b];
            }
        }
    }
    for (uint32_t i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + 10 * i);
        g_palette[232 + i] =
            ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }
    g_palette_built = 1;
}

uint32_t tui_palette_lookup(uint8_t idx) {
    if (!g_palette_built) palette_build();
    return g_palette[idx];
}

int tui_init(void) {
    if (!g_palette_built) palette_build();
    return 0;
}

int tui_attach(uint32_t console_id) {
    // Stage A5 substrate: SYS_CONSOLE_ATTACH is -ENOSYS. We track the bound
    // console locally and use DEBUG_CONSOLE_WRITE_CELL on writes. Stage C2
    // swaps this for the real attach path.
    g_state.attached_console = (int32_t)console_id;
    return 0;
}

int tui_write_cell(uint32_t console_id, uint32_t row, uint32_t col,
                   uint32_t codepoint, uint8_t fg, uint8_t bg, uint16_t attrs) {
    long rc = syscall_debug_console_write_cell(console_id, row, col,
                                               codepoint, fg, bg, attrs);
    return (int)rc;
}

int tui_clear(uint32_t console_id, uint8_t fg, uint8_t bg) {
    // Naive O(W*H) loop. Stage C2 with mapped cell-VMO drops this to a
    // single bulk memset.
    // We don't have width/height locally yet; reasonable upper bound is
    // 200x60 (covers 1600x960 fb). Iterate until we see -1 (out of bounds)
    // and stop at first failed write per row.
    for (uint32_t row = 0; row < 60; row++) {
        for (uint32_t col = 0; col < 200; col++) {
            int rc = tui_write_cell(console_id, row, col, ' ', fg, bg, 0);
            if (rc != 0) {
                if (col == 0) return 0;  // hit row bound; success
                break;                    // hit col bound; advance row
            }
        }
    }
    return 0;
}

int tui_print(uint32_t console_id, uint32_t row, uint32_t col,
              uint8_t fg, uint8_t bg, uint16_t attrs, const char *s) {
    if (!s) return -1;
    uint32_t cur_col = col;
    while (*s) {
        // ASCII fast-path. UTF-8 multibyte is FU27.X (full Unicode rendering).
        uint8_t b = (uint8_t)*s;
        if (b == '\n') {
            row++;
            cur_col = col;
            s++;
            continue;
        }
        if (b >= 0x80) {
            // skip continuation bytes for now; render '?' once.
            int rc = tui_write_cell(console_id, row, cur_col++,
                                    '?', fg, bg, attrs);
            if (rc != 0) return rc;
            // skip rest of UTF-8 sequence
            s++;
            while (*s && (((uint8_t)*s) & 0xC0u) == 0x80u) s++;
            continue;
        }
        int rc = tui_write_cell(console_id, row, cur_col++,
                                (uint32_t)b, fg, bg, attrs);
        if (rc != 0) return rc;
        s++;
    }
    return 0;
}

int tui_draw_box(uint32_t console_id, uint32_t row, uint32_t col,
                 uint32_t height, uint32_t width, uint8_t fg, uint8_t bg) {
    if (height < 2 || width < 2) return -1;

    // Corners.
    int rc = tui_write_cell(console_id, row, col,
                            TUI_BOX_TL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row, col + width - 1,
                        TUI_BOX_TR, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col,
                        TUI_BOX_BL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col + width - 1,
                        TUI_BOX_BR, fg, bg, 0);
    if (rc != 0) return rc;

    // Top + bottom edges.
    for (uint32_t cc = col + 1; cc < col + width - 1; cc++) {
        (void)tui_write_cell(console_id, row, cc,
                             TUI_BOX_HORIZ, fg, bg, 0);
        (void)tui_write_cell(console_id, row + height - 1, cc,
                             TUI_BOX_HORIZ, fg, bg, 0);
    }

    // Left + right edges.
    for (uint32_t rr = row + 1; rr < row + height - 1; rr++) {
        (void)tui_write_cell(console_id, rr, col,
                             TUI_BOX_VERT, fg, bg, 0);
        (void)tui_write_cell(console_id, rr, col + width - 1,
                             TUI_BOX_VERT, fg, bg, 0);
    }
    return 0;
}

int tui_draw_box_double(uint32_t console_id, uint32_t row, uint32_t col,
                        uint32_t height, uint32_t width, uint8_t fg, uint8_t bg) {
    if (height < 2 || width < 2) return -1;
    int rc = tui_write_cell(console_id, row, col,
                            TUI_BOX2_TL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row, col + width - 1,
                        TUI_BOX2_TR, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col,
                        TUI_BOX2_BL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col + width - 1,
                        TUI_BOX2_BR, fg, bg, 0);
    if (rc != 0) return rc;
    for (uint32_t cc = col + 1; cc < col + width - 1; cc++) {
        (void)tui_write_cell(console_id, row, cc,
                             TUI_BOX2_HORIZ, fg, bg, 0);
        (void)tui_write_cell(console_id, row + height - 1, cc,
                             TUI_BOX2_HORIZ, fg, bg, 0);
    }
    for (uint32_t rr = row + 1; rr < row + height - 1; rr++) {
        (void)tui_write_cell(console_id, rr, col,
                             TUI_BOX2_VERT, fg, bg, 0);
        (void)tui_write_cell(console_id, rr, col + width - 1,
                             TUI_BOX2_VERT, fg, bg, 0);
    }
    return 0;
}

int tui_fill_hrule(uint32_t console_id, uint32_t row, uint32_t col,
                   uint32_t width, uint32_t codepoint,
                   uint8_t fg, uint8_t bg) {
    for (uint32_t i = 0; i < width; i++) {
        int rc = tui_write_cell(console_id, row, col + i,
                                codepoint, fg, bg, 0);
        if (rc != 0) return rc;
    }
    return 0;
}

int tui_set_cursor(uint32_t console_id, uint32_t row, uint32_t col) {
    // Clear the previous cursor cell (if any) by writing it with no
    // CURSOR attr; then set CURSOR on the new cell. We can't read the old
    // cell back yet (DEBUG_CONSOLE_READ_CELL is FU27.X), so the old cell's
    // codepoint/fg/bg get reset to ' '/15/0 — Stage C2 will fix this when
    // the cell-VMO mapping arrives.
    if (g_state.cursor_active) {
        (void)tui_write_cell(console_id, g_state.cursor_row,
                             g_state.cursor_col, ' ', 15, 0, 0);
    }
    int rc = tui_write_cell(console_id, row, col,
                            ' ', 15, 0, TUI_ATTR_CURSOR);
    if (rc != 0) return rc;
    g_state.cursor_row = row;
    g_state.cursor_col = col;
    g_state.cursor_active = 1;
    return 0;
}

int tui_present(uint32_t console_id) {
    long rc = syscall_debug_console_synthetic_render(console_id);
    return (int)rc;
}
