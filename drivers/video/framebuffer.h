/* drivers/video/framebuffer.h */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../kernel/sync/spinlock.h"
// Forward declaration
struct limine_framebuffer_request;

// --- Color Definitions ---
#define COLOR_BLACK      0x00000000
#define COLOR_WHITE      0x00FFFFFF
#define COLOR_RED        0x00FF0000
#define COLOR_GREEN      0x0000FF00
#define COLOR_BLUE       0x000000FF
#define COLOR_CYAN       0x0000FFFF
#define COLOR_MAGENTA    0x00FF00FF
#define COLOR_YELLOW     0x00FFFF00
#define COLOR_GRAHA_BLUE 0xFF0066CC
#define COLOR_DARK_GRAY  0x00404040
#define COLOR_LIGHT_GRAY 0x00C0C0C0

// --- Public Functions ---

/**
 * @brief Initializes the framebuffer driver
 * @param fb_request The framebuffer request from Limine
 * @return true if successful, false otherwise
 */
bool framebuffer_init(volatile struct limine_framebuffer_request *fb_request);

// Phase 14: deferred CAN registration. Called from kmain after slab
// and hw caps are ready.
void framebuffer_register_cap(void);

/**
 * @brief Gets the framebuffer width
 * @return Width in pixels
 */
uint32_t framebuffer_get_width(void);

/**
 * @brief Gets the framebuffer height
 * @return Height in pixels
 */
uint32_t framebuffer_get_height(void);

/**
 * @brief Draws a single pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color 32-bit ARGB color
 */
void framebuffer_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/**
 * @brief Draws a filled rectangle
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color Fill color
 */
void framebuffer_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

/**
 * @brief Draws a rectangle outline
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color Outline color
 */
void framebuffer_draw_rect_outline(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

/**
 * @brief Draws a single character (8x16 font)
 * @param c Character to draw
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param fg_color Foreground color
 * @param bg_color Background color
 */
 void framebuffer_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color);

/**
 * @brief Draws a null-terminated string
 * @param str String to draw
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param fg_color Foreground color
 * @param bg_color Background color
 */
void framebuffer_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);


// Add this function declaration
void framebuffer_draw_hex(uint64_t value, int x, int y, uint32_t fg_color, uint32_t bg_color);

/**
 * @brief Clears the entire screen
 * @param color Fill color
 */
void framebuffer_clear(uint32_t color);

void framebuffer_draw_string_safe(const char *str, uint32_t x, uint32_t y,
                                  uint32_t fg_color, uint32_t bg_color);

/**
 * Phase 16 CAN callbacks. activate reclaims the display and draws a banner;
 * deactivate memsets to black and gates subsequent draws. is_active + read_pixel
 * are test hooks surfaced via SYS_DEBUG.
 */
int fb_activate(void);
int fb_deactivate(void);
bool framebuffer_is_active(void);
uint32_t framebuffer_read_pixel(uint32_t x, uint32_t y);

/* Phase 27 Stage A4 — composite-mode handshake.
 *
 * framebuffer_set_fbd_alive(true) is called from console_set_fbd_alive() once
 * the userspace fbd compositor has performed its first SYS_CONSOLE_ACK_RENDER.
 * From that point onward every klog-driven framebuffer_draw_* call becomes a
 * no-op (the FB is owned by fbd; serial still mirrors klog). The
 * g_panic_in_progress flag overrides this — kernel oops/panic always wins.
 *
 * framebuffer_force_draw_cell() bypasses both flags and is used by:
 *   (1) console_render_synthetic_frame() — kernel-side composite for the
 *       gate test fbd_render under autorun=ktest, where fbd is not spawned.
 *   (2) Future panic.c paths that want to overlay diagnostics on top of
 *       fbd's last frame without losing it. */
void     framebuffer_set_fbd_alive(bool alive);
bool     framebuffer_get_fbd_alive(void);
void     framebuffer_set_panic_in_progress(bool in_progress);
// FU29.H TUI: interactive kernel console claims the display (debug draws bypass).
void     framebuffer_set_console_owns_display(bool owns);
uint32_t framebuffer_get_pitch(void);
void     framebuffer_force_draw_cell(uint32_t pixel_x, uint32_t pixel_y,
                                     uint32_t codepoint,
                                     uint32_t fg_color, uint32_t bg_color);

// Phase 27 Stage B1 — render an arbitrary 8x16 1bpp glyph at (pixel_x,
// pixel_y) using fg_color/bg_color. Used by console_render_synthetic_frame
// for sprite-cell codepoints (0xE100..0xE7FF) where the glyph isn't in the
// kernel font tables. Bypasses g_fbd_alive (always draws).
void framebuffer_force_draw_sprite(uint32_t pixel_x, uint32_t pixel_y,
                                   const uint8_t glyph[16],
                                   uint32_t fg_color, uint32_t bg_color);

// Phase 27 Stage B1 + FU27.X.alpha_blend — composite an RGBA pixel onto
// the framebuffer at (pixel_x, pixel_y). Bypasses g_fbd_alive. Used by
// overlay damage-rect composite path. Source pixel format is 0xAARRGGBB.
// Fast paths for src_a == 0xFF (opaque write, no read) and src_a == 0
// (transparent, no-op). Otherwise reads dst pixel and performs the
// standard 8-bit alpha blend per channel.
void framebuffer_force_blit_pixel(uint32_t pixel_x, uint32_t pixel_y,
                                  uint32_t argb);

// Phase 29 Session D — back-compute the framebuffer's physical address
// (Limine HHDM virt - g_hhdm_offset). Returns 0 if framebuffer_init has
// not been called yet. Used by SYS_CONSOLE_GFX_MAP_FB to mint a
// VMO_MMIO-backed handle for userspace direct blit.
uint64_t framebuffer_get_phys_address(void);