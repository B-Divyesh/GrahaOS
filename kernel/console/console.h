// kernel/console/console.h
//
// Phase 27 Block A — Virtual console subsystem.
//
// 4 default consoles (max 8) multiplexed onto the single hardware framebuffer
// by the userspace `fbd` compositor. Each console:
//   - has a VMO containing W*H*16 bytes of tui_cell_t (writable by owner,
//     read-only for fbd via SYS_VMO_MAP).
//   - has a kernel-managed input channel; keystrokes flow here when the
//     console is selected.
//   - is wrapped in a CAP_KIND_CONSOLE cap_object with rights ATTACH | INSPECT
//     | OBSERVE | READ | WRITE. Init grants sub-tokens to userspace apps.
//
// Stage A1 ships the substrate: types, console_table, console_init,
// console_get_selected, console_find_by_owner. Subsequent stages wire:
//   A2: SYS_CONSOLE_* syscalls (12 slots, 1101-1112)
//   A3: keyboard Alt+N detection + g_console_switch_chan + key routing
//   A4: fbd compositor + framebuffer-MMIO VMO (slot 0 sentinel)
//   A5: libtui userspace primitives + CP437 font extension
//   B1: sprite_table + gfx_overlay (codepoint 0xE100..0xE7FF + RGBA layer)
//
// Slots 1093/1094 originally proposed in phase-27-tui-framework.yml are
// OBSOLETE (collide with Phase 24 SYS_SNAP_CREATE/RESTORE). Canonical mapping
// is 1101..1112 in specs/phase-27-mega.yml.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../sync/spinlock.h"
#include "../cap/token.h"

// Forward decls — defined in mm/vmo.h, ipc/channel.h.
struct vmo;
struct channel;

// ---------------------------------------------------------------------------
// Limits + constants.
// ---------------------------------------------------------------------------
#define NUM_CONSOLES         4u   // default at boot
#define NUM_CONSOLES_MAX     8u   // max via SYS_CONSOLE_CREATE

// Sprite codepoint range (Block B). Cells with codepoint in [SPRITE_BASE..SPRITE_END]
// reference the per-console sprite registry instead of the font_8x16 glyph table.
#define TUI_SPRITE_BASE   0xE100u
#define TUI_SPRITE_END    0xE7FFu  // inclusive; 1791 sprites maximum
#define NUM_SPRITES       (TUI_SPRITE_END - TUI_SPRITE_BASE + 1u)

// Cell attribute bits (tui_cell_t.attrs).
#define TUI_ATTR_BOLD       0x0001u
#define TUI_ATTR_UNDERLINE  0x0002u
#define TUI_ATTR_INVERSE    0x0004u
#define TUI_ATTR_CURSOR     0x0008u  // render hardware cursor at this cell

// Key-event modifier bits (key_event_t.modifiers).
#define KEY_MOD_CTRL   0x1u
#define KEY_MOD_ALT    0x2u
#define KEY_MOD_SHIFT  0x4u

// CAP_KIND_CONSOLE-specific rights. Defined here in addition to token.h
// (where the bit values live) so callers don't need both headers.
//   RIGHT_ATTACH  = 0x20000  // become console owner
//   RIGHT_OBSERVE = 0x40000  // subscribe to keystrokes from non-owner position
//   (RIGHT_READ + RIGHT_WRITE + RIGHT_INSPECT already defined in token.h)
#ifndef RIGHT_ATTACH
#define RIGHT_ATTACH   0x0000000000020000ULL  // CAP_KIND_CONSOLE
#endif
#ifndef RIGHT_OBSERVE
#define RIGHT_OBSERVE  0x0000000000040000ULL  // CAP_KIND_CONSOLE
#endif

// Default rights granted to newly-created CAP_KIND_CONSOLE objects. Sub-tokens
// derived from this can carry strict subsets (e.g., AI agent gets
// READ|INSPECT|OBSERVE without ATTACH or WRITE).
#define CAP_CONSOLE_DEFAULT_RIGHTS \
    (RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT | RIGHT_ATTACH | RIGHT_OBSERVE | \
     RIGHT_DERIVE | RIGHT_REVOKE)

// ---------------------------------------------------------------------------
// 16-byte cell. Layout stable on-VMO; written by libtui; read by fbd.
// ---------------------------------------------------------------------------
typedef struct tui_cell {
    uint32_t codepoint;   // Unicode 0x20..0x10FFFF, or 0xE100..0xE7FF for sprite cells.
    uint8_t  fg_color;    // 256-color palette index
    uint8_t  bg_color;
    uint16_t attrs;       // TUI_ATTR_* bitfield
    uint8_t  padding[8];  // reserved (24-bit RGB color, wide-char clusters)
} tui_cell_t;
_Static_assert(sizeof(tui_cell_t) == 16, "tui_cell_t must be 16 bytes");

// ---------------------------------------------------------------------------
// Input event posted to a console's input channel.
// ---------------------------------------------------------------------------
typedef struct key_event {
    uint32_t codepoint;   // Unicode codepoint; arrows U+E000..U+E003
    uint32_t modifiers;   // KEY_MOD_* bitfield
    uint64_t timestamp_ns;
} key_event_t;
_Static_assert(sizeof(key_event_t) == 16, "key_event_t must be 16 bytes");

// ---------------------------------------------------------------------------
// Returned by SYS_CONSOLE_CREATE (Stage A2) — userspace receives this.
// ---------------------------------------------------------------------------
typedef struct console_info {
    uint32_t   id;
    uint32_t   width_cells;
    uint32_t   height_cells;
    uint32_t   reserved;
    uint64_t   cell_vmo_handle;     // handle in caller's table (Stage A2 fills)
    uint64_t   input_chan_handle;   // handle in caller's table (Stage A2 fills)
    cap_token_t console_cap;        // CAP_KIND_CONSOLE token
} console_info_t;
_Static_assert(sizeof(console_info_t) == 40, "console_info_t ABI lock");

// ---------------------------------------------------------------------------
// Block B forward decls (sprite registry + RGBA overlay; implemented at B1).
// ---------------------------------------------------------------------------
struct sprite_table;
struct gfx_overlay;

typedef struct damage_rect {
    uint16_t x, y, w, h;
} damage_rect_t;

// ---------------------------------------------------------------------------
// Per-console kernel record.
// ---------------------------------------------------------------------------
typedef struct console {
    uint32_t        id;                   // 0..3 default
    uint32_t        width_cells;
    uint32_t        height_cells;
    int32_t         owner_pid;            // 0 = detached
    struct vmo     *cell_vmo;             // cell buffer; W*H*16 bytes
    struct channel *input_chan;           // keyboard delivery for this console
    uint32_t        cap_object_idx;       // CAP_KIND_CONSOLE; sub-tokens derive from this
    uint32_t        _pad32;
    _Atomic uint64_t dirty_seq;           // bumped on cell write; fbd compares to rendered_seq
    uint64_t        rendered_seq;         // fbd's last successfully composited seq
    // Block B (Stage B1) — both lazy-allocated.
    struct sprite_table *sprite_table;    // NULL until first SPRITE_REGISTER
    struct gfx_overlay  *gfx_overlay;     // NULL until first GFX_ENABLE
    damage_rect_t   damage_ring[16];      // overlay damage ring
    uint8_t         damage_head;
    uint8_t         damage_tail;
    uint8_t         _pad_b[6];
} console_t;

// ---------------------------------------------------------------------------
// Console table (singleton) holding all consoles + the selected ID + fbd
// liveness. Statically allocated — no slab churn at boot.
// ---------------------------------------------------------------------------
typedef struct console_table {
    console_t          consoles[NUM_CONSOLES_MAX];
    _Atomic uint32_t   selected;       // currently-displayed console ID
    uint32_t           num_consoles;   // currently allocated; default 4
    _Atomic bool       fbd_alive;      // set true after fbd's first ACK_RENDER
    uint8_t            _pad[7];
    spinlock_t         lock;           // protects array during create/delete
} console_table_t;

// Access to the singleton (read-only externs; callers should use the helpers
// below where possible).
extern console_table_t g_console_table;

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Boot-time init. Allocates the 4 default consoles, their cell-buffer VMOs,
// their input channels, and per-console CAP_KIND_CONSOLE objects. Called from
// kernel/main.c after cap_system_init() and before audit_init().
//
// The hardware framebuffer is NOT touched here — fbd takes ownership at
// userspace startup (Stage A4). Until fbd publishes its first ACK_RENDER,
// kernel klog continues to draw directly via drivers/video/framebuffer.c.
void console_init(uint32_t fb_width_px, uint32_t fb_height_px);

// Allocate a new virtual console (used by SYS_CONSOLE_CREATE in Stage A2).
// Returns the new console_t* or NULL on failure (slab full / quota hit).
console_t *console_create(uint32_t width_cells, uint32_t height_cells);

// ---------------------------------------------------------------------------
// Selection + ownership.
// ---------------------------------------------------------------------------

// Atomically switch the displayed console. Returns 0 / -EINVAL.
int console_switch(uint32_t console_id);

// Read currently-displayed console ID. Lock-free.
uint32_t console_get_selected(void);

// Linear scan for the console attached to `pid`. Returns NULL if none.
console_t *console_find_by_owner(int32_t pid);

// Lookup by ID (bounds-checked). Returns NULL if invalid.
console_t *console_get_by_id(uint32_t console_id);

// ---------------------------------------------------------------------------
// Input routing (Stage A3 wires this from keyboard.c).
// ---------------------------------------------------------------------------

// Post a key event to the selected console's input channel. NOOP if no
// console is selected, no owner is attached, or the channel is full
// (drop-on-overflow; AUDIT_CHAN_FULL emitted by chan_send if relevant).
void console_route_key(const key_event_t *ev);

// ---------------------------------------------------------------------------
// fbd liveness handshake (Stage A4).
// ---------------------------------------------------------------------------

// Set/clear fbd_alive flag. Called from SYS_CONSOLE_ACK_RENDER once fbd has
// performed at least one composite pass. While fbd_alive == true,
// drivers/video/framebuffer.c's direct draws bail (unless g_panic_in_progress
// is also true; panic always wins).
void console_set_fbd_alive(bool alive);
bool console_get_fbd_alive(void);

// SYS_CONSOLE_ACK_RENDER backend: validate console_id, atomically bump
// rendered_seq, set fbd_alive=true on first successful call.
// Returns 0 / -1 (-EINVAL on bad id).
int console_ack_render(uint32_t console_id, uint64_t rendered_seq);

// ---------------------------------------------------------------------------
// Stage A4 synthetic render + debug-write helpers.
// ---------------------------------------------------------------------------

// Kernel-side composite. Reads the cell-VMO of `console_id` and blits every
// cell to the hardware framebuffer via framebuffer_force_draw_cell (bypasses
// g_fbd_alive). Used by DEBUG_CONSOLE_SYNTHETIC_RENDER for the gate test
// fbd_render under autorun=ktest where fbd isn't spawned. Returns 0 on
// success, -1 on invalid console_id.
int console_render_synthetic_frame(uint32_t console_id);

// DEBUG-only: write one cell into the cell-VMO of `console_id`. Bypasses cap
// gating (SYS_CONSOLE_ATTACH is -ENOSYS until Stage C2). Used by the gate
// test fbd_render to populate cells before triggering a synthetic render.
// Returns 0 on success, -1 on out-of-bounds.
int console_write_cell_debug(uint32_t console_id, uint32_t row, uint32_t col,
                             uint32_t codepoint, uint8_t fg, uint8_t bg,
                             uint16_t attrs);

// Stage A5: 256-color xterm-compat palette accessor. Index 0..15 = VGA,
// 16..231 = 6x6x6 RGB cube, 232..255 = 24-step grey. Used by
// console_render_synthetic_frame and exported for libtui userspace mirror.
uint32_t console_palette_lookup(uint8_t idx);

// Stage B1 helpers — sprite registry + RGBA overlay introspection.
// Returns the registered sprite's 16-byte bitmap, or NULL if not registered.
const uint8_t *console_sprite_lookup(uint32_t console_id, uint32_t sprite_id);

// Returns the cap_object_idx of the gfx overlay VMO (so userspace can
// vmo_map it). Returns 0 if not enabled.
uint32_t console_get_gfx_cap_idx(uint32_t console_id);

// Stage B1 test helper — fill rect of overlay buffer with ARGB color.
// Production path is to vmo_map and write directly; this helper exists so
// gate tests don't need the full vmo_map plumbing (Stage C2 tightens).
int console_gfx_fill_debug(uint32_t console_id, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t color);

// ---------------------------------------------------------------------------
// Block B forward (Stage B1 implements).
// ---------------------------------------------------------------------------

// Register a sprite into the per-console sprite registry. Lazy-allocates the
// sprite table on first call. Bitmap is exactly 16 bytes (8x16, 1bpp).
int console_sprite_register(uint32_t console_id, uint32_t sprite_id,
                            const uint8_t bitmap16[16]);

// Enable RGBA overlay for the console. Lazy-allocates a W*H*4 VMO. Returns
// the VMO* on success or NULL on failure / already enabled.
struct vmo *console_gfx_enable(uint32_t console_id, uint32_t w_px, uint32_t h_px);

// Append a damage rect to the console's overlay damage ring.
int console_gfx_damage(uint32_t console_id, const damage_rect_t *rect);
