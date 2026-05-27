// user/libtui/libtui.h
//
// Phase 27 Block A (Stage A5) — libtui userspace TUI primitives.
//
// ncurses-like API for writing to virtual consoles. Apps:
//   1. Call tui_init(console_id) to bind to a console.
//   2. Use tui_clear, tui_write_cell, tui_draw_box, etc. to populate cells.
//   3. Call tui_present() to push the buffer to the kernel and trigger a
//      composite into the framebuffer.
//
// **Stage A5 substrate note**: until Stage C2 lands cap inheritance + real
// SYS_CONSOLE_ATTACH cell-VMO mapping, libtui writes cells through the
// DEBUG_CONSOLE_WRITE_CELL syscall (one syscall per cell write). Stage C2
// will swap this for a single mmap of the cell-VMO + direct memory writes,
// dropping per-cell syscall overhead to zero. The libtui API stays stable.
//
// 256-color xterm-compat palette indices:
//   0..15    VGA basic (black, dark/bright primary + secondary)
//   16..231  6x6x6 RGB cube (channel values 0/95/135/175/215/255)
//   232..255 24-step grey ramp (8 + 10*(idx-232))
#pragma once

#include <stdint.h>

// Cell attribute bits — mirror kernel/console/console.h.
#define TUI_ATTR_BOLD       0x0001u
#define TUI_ATTR_UNDERLINE  0x0002u
#define TUI_ATTR_INVERSE    0x0004u
#define TUI_ATTR_CURSOR     0x0008u

// Sprite codepoint range — mirror kernel/console/console.h. Cells with
// codepoint in [TUI_SPRITE_BASE..TUI_SPRITE_END] reference the per-console
// sprite registry instead of font_8x16. 1791 sprite slots maximum.
#define TUI_SPRITE_BASE     0xE100u
#define TUI_SPRITE_END      0xE7FFu

// Common Unicode box-drawing codepoints supported by Stage A5 font extension.
#define TUI_BOX_HORIZ       0x2500u  // ─
#define TUI_BOX_VERT        0x2502u  // │
#define TUI_BOX_TL          0x250Cu  // ┌
#define TUI_BOX_TR          0x2510u  // ┐
#define TUI_BOX_BL          0x2514u  // └
#define TUI_BOX_BR          0x2518u  // ┘
#define TUI_BOX_T_LEFT      0x251Cu  // ├
#define TUI_BOX_T_RIGHT     0x2524u  // ┤
#define TUI_BOX_T_TOP       0x252Cu  // ┬
#define TUI_BOX_T_BOTTOM    0x2534u  // ┴
#define TUI_BOX_CROSS       0x253Cu  // ┼

// Double-line box glyphs (Stage CLOSE banner polish).
#define TUI_BOX2_HORIZ      0x2550u  // ═
#define TUI_BOX2_VERT       0x2551u  // ║
#define TUI_BOX2_TL         0x2554u  // ╔
#define TUI_BOX2_TR         0x2557u  // ╗
#define TUI_BOX2_BL         0x255Au  // ╚
#define TUI_BOX2_BR         0x255Du  // ╝
#define TUI_BOX2_T_LEFT     0x2560u  // ╠
#define TUI_BOX2_T_RIGHT    0x2563u  // ╣

// Block elements + shading.
#define TUI_BLOCK_UPPER     0x2580u  // ▀
#define TUI_BLOCK_LOWER     0x2584u  // ▄
#define TUI_BLOCK_FULL      0x2588u  // █
#define TUI_BLOCK_LIGHT     0x2591u  // ░
#define TUI_BLOCK_MEDIUM    0x2592u  // ▒
#define TUI_BLOCK_DARK      0x2593u  // ▓

// Initialise libtui state for the calling process. Currently a no-op stub;
// Stage C2 will lazy-init cell-VMO mappings here.
int tui_init(void);

// Bind to `console_id`. Stage C2 swaps the substrate for a real
// SYS_CONSOLE_ATTACH that returns mapped cell-VMO + input-channel handle.
int tui_attach(uint32_t console_id);

// Clear the console (write space-glyph cells in fg_color/bg_color).
int tui_clear(uint32_t console_id, uint8_t fg, uint8_t bg);

// Write a single cell.
int tui_write_cell(uint32_t console_id, uint32_t row, uint32_t col,
                   uint32_t codepoint, uint8_t fg, uint8_t bg, uint16_t attrs);

// Write a NUL-terminated UTF-8 string starting at (row, col) in
// `console_id`. Codepoints outside the ASCII / box-drawing subset render
// as blank cells (FU27.X.font_full extends).
int tui_print(uint32_t console_id, uint32_t row, uint32_t col,
              uint8_t fg, uint8_t bg, uint16_t attrs, const char *s);

// Draw a single-line box at (row, col) with the given height + width using
// U+250C/U+2510/U+2514/U+2518 corners + U+2500/U+2502 sides.
int tui_draw_box(uint32_t console_id, uint32_t row, uint32_t col,
                 uint32_t height, uint32_t width, uint8_t fg, uint8_t bg);

// Same as tui_draw_box but using double-line glyphs (╔ ╗ ╚ ╝ ═ ║) for a
// heavier, more prominent border. Phase 27 Stage CLOSE banner polish.
int tui_draw_box_double(uint32_t console_id, uint32_t row, uint32_t col,
                        uint32_t height, uint32_t width, uint8_t fg, uint8_t bg);

// Fill a horizontal rule across `width` cells starting at (row, col) using
// the given codepoint (typically a block or shade glyph).
int tui_fill_hrule(uint32_t console_id, uint32_t row, uint32_t col,
                   uint32_t width, uint32_t codepoint,
                   uint8_t fg, uint8_t bg);

// Set the cursor position (writes TUI_ATTR_CURSOR on the cell at (row, col)
// and clears CURSOR on the previous position). NULL ok for first call.
int tui_set_cursor(uint32_t console_id, uint32_t row, uint32_t col);

// Push the cell buffer to the framebuffer. Stage A5 routes through
// DEBUG_CONSOLE_SYNTHETIC_RENDER (kernel-side composite). Stage A6+ may
// extend with userspace blitting via mapped framebuffer-MMIO VMO.
int tui_present(uint32_t console_id);

// Look up a 256-color palette entry. Returns ARGB (0x00RRGGBB form).
// Mirror of kernel/console/console.c::console_palette_lookup so apps can
// pre-compute colors before writing cells.
uint32_t tui_palette_lookup(uint8_t idx);

// ---------------------------------------------------------------------------
// Phase 29 Session D — input + vsync + framebuffer-MMIO.
// ---------------------------------------------------------------------------
struct input_event_u;
struct fb_dims_u;

// Drain console's input chan non-blocking.  Returns count copied (high bit
// 62 set if more remain queued).  Wraps SYS_CONSOLE_READ_INPUT.
long tui_read_input(uint32_t console_id, struct input_event_u *out,
                    uint32_t max_events);

// Block until the next 60Hz tick or max_wait_ns expires.  Returns 0 on
// tick, -ETIME on timeout.  Wraps SYS_CONSOLE_VSYNC_WAIT.
int tui_vsync_wait(uint64_t max_wait_ns);

// Map the hardware framebuffer into the caller's address space.  Writes the
// mapped pointer to *out_addr and the dims (pitch / width / height /
// size_bytes) to *out_dims.  Returns 0 on success; -EPERM if not the FB
// owner.  fbd-only path.
int tui_map_framebuffer(void **out_addr, struct fb_dims_u *out_dims);
