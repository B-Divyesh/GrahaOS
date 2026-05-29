// user/libtui/libtui.c
//
// Phase 29 Session D — libtui rewritten on top of mapped cell-VMO.
//
// The Stage A5 path used DEBUG_CONSOLE_WRITE_CELL — one syscall per cell.
// Session D wires SYS_CONSOLE_ATTACH for real: tui_attach maps the
// console's cell-VMO via syscall_vmo_map, and tui_write_cell becomes a
// single 16-byte memcpy + dirty mark.  On any attach failure we transparently
// fall back to the DEBUG path so existing code keeps working during the
// progressive cutover.

#include "libtui.h"
#include "../syscalls.h"

// 16-byte cell layout (mirror of kernel/console/console.h tui_cell_t).
typedef struct __attribute__((packed)) tui_cell_u {
    uint32_t codepoint;
    uint8_t  fg_color;
    uint8_t  bg_color;
    uint16_t attrs;
    uint8_t  padding[8];
} tui_cell_u_t;
_Static_assert(sizeof(tui_cell_u_t) == 16, "tui_cell_u_t must be 16 bytes");

// Per-console state.  Session D adds mapped-cell-VMO fast path.
typedef struct {
    int32_t  attached;             // 0/1
    uint32_t width_cells;
    uint32_t height_cells;
    tui_cell_u_t *mapped_cells;    // NULL → fall back to DEBUG path
    uint64_t cell_vmo_token_raw;
    uint64_t input_chan_token_raw;
    // FU29.X.partial_render_clip — accumulated dirty bbox (inclusive cell
    // coords) for mapped-VMO writes, flushed as ONE DEBUG_CONSOLE_MARK_DIRTY
    // in tui_present.  This keeps the write fast path at zero syscalls
    // (Session D's win) while still letting synthetic_render clip.
    uint32_t dirty_min_row, dirty_min_col, dirty_max_row, dirty_max_col;
    uint8_t  dirty_any;
} libtui_console_state_t;

#define LIBTUI_MAX_CONSOLES 8u

typedef struct {
    int32_t  attached_console;     // legacy single-console binding (back-compat)
    uint32_t cursor_row;
    uint32_t cursor_col;
    uint8_t  cursor_active;
    libtui_console_state_t consoles[LIBTUI_MAX_CONSOLES];
} libtui_state_t;

static libtui_state_t g_state = {
    .attached_console = -1,
    .cursor_row = 0,
    .cursor_col = 0,
    .cursor_active = 0,
};

// 256-color palette mirror (lazy-built).
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
    for (uint32_t i = 0; i < LIBTUI_MAX_CONSOLES; i++) {
        g_state.consoles[i].attached = 0;
        g_state.consoles[i].mapped_cells = 0;
        g_state.consoles[i].width_cells = 0;
        g_state.consoles[i].height_cells = 0;
        g_state.consoles[i].cell_vmo_token_raw = 0;
        g_state.consoles[i].input_chan_token_raw = 0;
    }
    return 0;
}

// Default assumption — overridden by tui_attach once we have a real
// SYS_CONSOLE_INSPECT path (Session E).  Picks a layout that fits the
// 1280x800 default Limine framebuffer: 160 cols × 50 rows.
#define LIBTUI_DEFAULT_W 160u
#define LIBTUI_DEFAULT_H 50u

int tui_attach(uint32_t console_id) {
    if (console_id >= LIBTUI_MAX_CONSOLES) return -1;
    g_state.attached_console = (int32_t)console_id;
    libtui_console_state_t *cs = &g_state.consoles[console_id];
    if (cs->attached && cs->mapped_cells) return 0;  // already attached

    // Try the real SYS_CONSOLE_ATTACH path.  cap_token_raw=0 → substrate
    // "trusted" mode (Session D); Session E narrows.
    uint64_t cell_tok = 0, input_tok = 0;
    long rc = syscall_console_attach_full(console_id, /*cap*/ 0,
                                          &cell_tok, &input_tok);
    if (rc != 0) {
        // Fall back to DEBUG path — keep attached=1 so writes still flow.
        cs->attached = 1;
        cs->width_cells  = LIBTUI_DEFAULT_W;
        cs->height_cells = LIBTUI_DEFAULT_H;
        cs->mapped_cells = 0;
        return 0;
    }

    cs->cell_vmo_token_raw  = cell_tok;
    cs->input_chan_token_raw = input_tok;

    // Map the cell VMO read-write.  Size: assume worst-case 256*64*16 =
    // 256 KiB rounded up to page; actual cells = width*height*16, but the
    // VMO is page-aligned so larger map is harmless (extra pages just are
    // unused tail).  Sizing 64 KiB covers up to 200x20 or 100x40.
    cap_token_u_t cell_token; cell_token.raw = cell_tok;
    uint64_t map_size = 0x10000ull;  // 64 KiB; covers 200x20 / 100x40
    long va = syscall_vmo_map(cell_token, 0, 0, map_size,
                              PROT_READ | PROT_WRITE);
    if (va <= 0) {
        // Map failed — keep DEBUG fallback.
        cs->attached = 1;
        cs->width_cells  = LIBTUI_DEFAULT_W;
        cs->height_cells = LIBTUI_DEFAULT_H;
        cs->mapped_cells = 0;
        return 0;
    }

    cs->attached       = 1;
    cs->width_cells    = LIBTUI_DEFAULT_W;
    cs->height_cells   = LIBTUI_DEFAULT_H;
    cs->mapped_cells   = (tui_cell_u_t *)(uintptr_t)va;
    return 0;
}

// FU29.X.partial_render_clip — expand a console's accumulated dirty bbox to
// include cell (row,col).  Userspace-only (no syscall); flushed in tui_present.
static inline void libtui_dirty_cell(libtui_console_state_t *cs,
                                     uint32_t row, uint32_t col) {
    if (!cs->dirty_any) {
        cs->dirty_min_row = cs->dirty_max_row = row;
        cs->dirty_min_col = cs->dirty_max_col = col;
        cs->dirty_any = 1;
        return;
    }
    if (row < cs->dirty_min_row) cs->dirty_min_row = row;
    if (row > cs->dirty_max_row) cs->dirty_max_row = row;
    if (col < cs->dirty_min_col) cs->dirty_min_col = col;
    if (col > cs->dirty_max_col) cs->dirty_max_col = col;
}

int tui_write_cell(uint32_t console_id, uint32_t row, uint32_t col,
                   uint32_t codepoint, uint8_t fg, uint8_t bg, uint16_t attrs) {
    if (console_id >= LIBTUI_MAX_CONSOLES) {
        long rc = syscall_debug_console_write_cell(console_id, row, col,
                                                   codepoint, fg, bg, attrs);
        return (int)rc;
    }
    libtui_console_state_t *cs = &g_state.consoles[console_id];
    if (cs->mapped_cells && row < cs->height_cells && col < cs->width_cells) {
        // Fast path — direct VMO write.
        tui_cell_u_t *p = &cs->mapped_cells[row * cs->width_cells + col];
        p->codepoint = codepoint;
        p->fg_color  = fg;
        p->bg_color  = bg;
        p->attrs     = attrs;
        for (int i = 0; i < 8; i++) p->padding[i] = 0;
        libtui_dirty_cell(cs, row, col);
        return 0;
    }
    // Fallback — single-syscall DEBUG path.
    long rc = syscall_debug_console_write_cell(console_id, row, col,
                                               codepoint, fg, bg, attrs);
    return (int)rc;
}

int tui_clear(uint32_t console_id, uint8_t fg, uint8_t bg) {
    // Fast path if mapped.
    if (console_id < LIBTUI_MAX_CONSOLES &&
        g_state.consoles[console_id].mapped_cells) {
        libtui_console_state_t *cs = &g_state.consoles[console_id];
        for (uint32_t r = 0; r < cs->height_cells; r++) {
            for (uint32_t c = 0; c < cs->width_cells; c++) {
                tui_cell_u_t *p = &cs->mapped_cells[r * cs->width_cells + c];
                p->codepoint = ' ';
                p->fg_color  = fg;
                p->bg_color  = bg;
                p->attrs     = 0;
                for (int i = 0; i < 8; i++) p->padding[i] = 0;
            }
        }
        // Whole frame changed.
        cs->dirty_min_row = 0;
        cs->dirty_min_col = 0;
        cs->dirty_max_row = cs->height_cells ? cs->height_cells - 1u : 0u;
        cs->dirty_max_col = cs->width_cells ? cs->width_cells - 1u : 0u;
        cs->dirty_any = 1;
        return 0;
    }
    // Fallback — DEBUG-write loop.
    for (uint32_t row = 0; row < 60; row++) {
        for (uint32_t col = 0; col < 200; col++) {
            int rc = tui_write_cell(console_id, row, col, ' ', fg, bg, 0);
            if (rc != 0) {
                if (col == 0) return 0;
                break;
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
        uint8_t b = (uint8_t)*s;
        if (b == '\n') {
            row++;
            cur_col = col;
            s++;
            continue;
        }
        if (b >= 0x80) {
            int rc = tui_write_cell(console_id, row, cur_col++,
                                    '?', fg, bg, attrs);
            if (rc != 0) return rc;
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
    int rc = tui_write_cell(console_id, row, col, TUI_BOX_TL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row, col + width - 1, TUI_BOX_TR, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col, TUI_BOX_BL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col + width - 1, TUI_BOX_BR, fg, bg, 0);
    if (rc != 0) return rc;
    for (uint32_t cc = col + 1; cc < col + width - 1; cc++) {
        (void)tui_write_cell(console_id, row, cc, TUI_BOX_HORIZ, fg, bg, 0);
        (void)tui_write_cell(console_id, row + height - 1, cc, TUI_BOX_HORIZ, fg, bg, 0);
    }
    for (uint32_t rr = row + 1; rr < row + height - 1; rr++) {
        (void)tui_write_cell(console_id, rr, col, TUI_BOX_VERT, fg, bg, 0);
        (void)tui_write_cell(console_id, rr, col + width - 1, TUI_BOX_VERT, fg, bg, 0);
    }
    return 0;
}

int tui_draw_box_double(uint32_t console_id, uint32_t row, uint32_t col,
                        uint32_t height, uint32_t width, uint8_t fg, uint8_t bg) {
    if (height < 2 || width < 2) return -1;
    int rc = tui_write_cell(console_id, row, col, TUI_BOX2_TL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row, col + width - 1, TUI_BOX2_TR, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col, TUI_BOX2_BL, fg, bg, 0);
    if (rc != 0) return rc;
    rc = tui_write_cell(console_id, row + height - 1, col + width - 1, TUI_BOX2_BR, fg, bg, 0);
    if (rc != 0) return rc;
    for (uint32_t cc = col + 1; cc < col + width - 1; cc++) {
        (void)tui_write_cell(console_id, row, cc, TUI_BOX2_HORIZ, fg, bg, 0);
        (void)tui_write_cell(console_id, row + height - 1, cc, TUI_BOX2_HORIZ, fg, bg, 0);
    }
    for (uint32_t rr = row + 1; rr < row + height - 1; rr++) {
        (void)tui_write_cell(console_id, rr, col, TUI_BOX2_VERT, fg, bg, 0);
        (void)tui_write_cell(console_id, rr, col + width - 1, TUI_BOX2_VERT, fg, bg, 0);
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
    // FU29.X.partial_render_clip — flush the accumulated dirty bbox from
    // mapped-VMO writes into the kernel ring (ONE syscall per present, not
    // per cell) so synthetic_render clips to exactly what changed.  Writes
    // through the DEBUG path already mark dirty kernel-side, so this only
    // covers the mapped fast path.
    if (console_id < LIBTUI_MAX_CONSOLES) {
        libtui_console_state_t *cs = &g_state.consoles[console_id];
        if (cs->dirty_any) {
            uint16_t x = (uint16_t)cs->dirty_min_col;
            uint16_t y = (uint16_t)cs->dirty_min_row;
            uint16_t w = (uint16_t)(cs->dirty_max_col - cs->dirty_min_col + 1u);
            uint16_t h = (uint16_t)(cs->dirty_max_row - cs->dirty_min_row + 1u);
            (void)syscall_console_mark_dirty_cells(console_id, x, y, w, h);
            cs->dirty_any = 0;
        }
    }
    // Session D: when the cell-VMO is mapped, the kernel still owns the
    // pixel composite — we just need to trigger it.  syscall_console_ack_render
    // would be appropriate when fbd is alive; for now keep the existing
    // synthetic-render trigger so kernel klog stops trampling our cells.
    long rc = syscall_debug_console_synthetic_render(console_id);
    return (int)rc;
}

// ---------------------------------------------------------------------------
// Phase 29 Session D wrappers — input, vsync, framebuffer-MMIO.
// ---------------------------------------------------------------------------

long tui_read_input(uint32_t console_id, struct input_event_u *out,
                    uint32_t max_events) {
    return syscall_console_read_input(console_id,
                                       (input_event_u_t *)out, max_events);
}

int tui_vsync_wait(uint64_t max_wait_ns) {
    return (int)syscall_console_vsync_wait(max_wait_ns);
}

int tui_map_framebuffer(void **out_addr, struct fb_dims_u *out_dims) {
    if (!out_addr || !out_dims) return -1;
    uint64_t handle_raw = 0;
    long rc = syscall_console_gfx_map_fb(&handle_raw, (fb_dims_u_t *)out_dims);
    if (rc != 0) return (int)rc;
    cap_token_u_t tok; tok.raw = handle_raw;
    long va = syscall_vmo_map(tok, 0, 0,
                              ((fb_dims_u_t *)out_dims)->size_bytes,
                              PROT_READ | PROT_WRITE);
    if (va <= 0) return -1;
    *out_addr = (void *)(uintptr_t)va;
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 29 Session E.
// ---------------------------------------------------------------------------

int tui_sprite_animate(uint32_t console_id, uint32_t sprite_id,
                       const void *keyframes, uint16_t n_frames,
                       uint32_t duration_ms_per_frame,
                       uint8_t interp_mode) {
    (void)interp_mode;  // v1 STEP-only
    return (int)syscall_console_sprite_animate(console_id, sprite_id,
                                               keyframes, n_frames,
                                               duration_ms_per_frame);
}

int tui_begin_tx(uint32_t console_id) {
    return (int)syscall_console_begin_tx(console_id);
}

int tui_commit_tx(uint32_t tx_handle) {
    return (int)syscall_console_commit_tx(tx_handle);
}

int tui_abort_tx(uint32_t tx_handle) {
    return (int)syscall_console_abort_tx(tx_handle);
}

long tui_read_mouse(uint32_t console_id, struct input_event_u *out,
                    uint32_t max_events) {
    if (!out || max_events == 0) return -1;
    // Drain into a local bounce; filter by kind on the way out.
    input_event_u_t bounce[64];
    if (max_events > 64) max_events = 64;
    long rc = syscall_console_read_input(console_id, bounce, max_events);
    if (rc < 0) return rc;
    long count = rc & 0x3FFFFFFFFFFFFFFFLL;  // mask off the MORE flag
    long out_n = 0;
    input_event_u_t *uout = (input_event_u_t *)out;
    for (long i = 0; i < count; i++) {
        if (bounce[i].kind == 1 || bounce[i].kind == 2) {
            uout[out_n++] = bounce[i];
        }
    }
    return out_n;
}
