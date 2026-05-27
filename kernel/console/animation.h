// kernel/console/animation.h
//
// Phase 29 Session E — Sprite animation primitive.
//
// SYS_CONSOLE_SPRITE_ANIMATE registers up to N keyframes for a sprite slot.
// A 60Hz scheduler advances each active animation by one frame on its
// next_tick_tsc boundary; when cur_frame reaches n_frames, the animation
// transitions to COMMITTED and stops stepping.
//
// v1 simplifications (per FU29.X.lerp_animations):
//   * keyframes are full tui_cell_t arrays, not deltas
//   * interpolation = STEP only (no LERP; deferred)
//   * per-console table of up to 64 animations
//
// Driven by the existing IRQ0 timer tick (g_timer_ticks++).  No new IRQ
// added — the timer handler calls console_animation_tick_all() once per
// tick (LAPIC timer fires at ~100 Hz; we step at most one frame per call).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "console.h"

#define CONSOLE_ANIM_MAX  64u   // per console; total = 8 consoles * 64 = 512

#define ANIM_STATE_IDLE       0u
#define ANIM_STATE_RUNNING    1u
#define ANIM_STATE_COMMITTED  2u
#define ANIM_STATE_ABORTED    3u

#define ANIM_INTERP_STEP   0u
#define ANIM_INTERP_LERP   1u  // FU29.X: reserved

typedef struct console_animation {
    uint32_t   console_id;
    uint32_t   sprite_id;            // target sprite slot in console's registry
    tui_cell_t *keyframes;           // kmalloc'd array of n_frames cells
    uint16_t   n_frames;
    uint16_t   cur_frame;
    uint64_t   next_tick_tsc;
    uint32_t   duration_per_frame_tsc;
    uint8_t    interp_mode;
    uint8_t    state;
    uint8_t    _pad[2];
} console_animation_t;

// Boot-time init.  Called from console_init.
void console_animation_init(void);

// Register a new animation.  Returns slot id (>= 0) or -EINVAL/-ENOMEM/-EPERM.
//   keyframes_kernel = kernel-side copy of the user's keyframes array
//     (caller is responsible for copying from user before this call).
//   duration_per_frame_ms must be >= 1.
int console_animation_create(uint32_t console_id, uint32_t sprite_id,
                             const tui_cell_t *keyframes_kernel,
                             uint16_t n_frames,
                             uint32_t duration_per_frame_ms,
                             uint8_t interp_mode);

// Step every active animation across all consoles.  Called from the timer
// IRQ handler.  Cheap fast-path when no animations are running.
void console_animation_tick_all(void);

// DEBUG hook: force-tick one animation immediately (ignores wall clock).
// Used by sprite_anim.tap.
int console_animation_debug_tick(uint32_t console_id, uint32_t slot);

// DEBUG hook: read cur_frame.  Returns -1 if slot not active.
int console_animation_debug_get_frame(uint32_t console_id, uint32_t slot);

// DEBUG hook: read state.  Returns -1 if slot invalid.
int console_animation_debug_get_state(uint32_t console_id, uint32_t slot);

// Force-tick whatever the next due animation is on `console_id`.  Returns
// 0 if any animation stepped, 1 if none was due.
int console_animation_tick_one(uint32_t console_id);
