// kernel/console/animation.c
//
// Phase 29 Session E — Sprite animation primitive.

#include "animation.h"
#include "console.h"

#include "../log.h"
#include "../sync/spinlock.h"
#include "../mm/kheap.h"
#include "../../arch/x86_64/cpu/tsc.h"

#include <string.h>

extern uint64_t g_tsc_hz;

// Per-console animation table — packed slot array; slot zero is a sentinel
// "not in use" entry.  Table is protected by a single global spinlock; the
// scheduler holds it briefly (only to advance one frame + register a new
// next-tick deadline).
typedef struct {
    console_animation_t slots[CONSOLE_ANIM_MAX];
    uint32_t            active;        // bitset of in-use slots (lazy hint)
} per_console_t;

static per_console_t g_anim_tables[NUM_CONSOLES_MAX];
static spinlock_t    g_anim_lock = SPINLOCK_INITIALIZER("console_anim");
static bool          g_anim_inited = false;

void console_animation_init(void) {
    if (g_anim_inited) return;
    spinlock_acquire(&g_anim_lock);
    if (!g_anim_inited) {
        for (uint32_t i = 0; i < NUM_CONSOLES_MAX; i++) {
            memset(&g_anim_tables[i], 0, sizeof(g_anim_tables[i]));
        }
        g_anim_inited = true;
    }
    spinlock_release(&g_anim_lock);
}

// Find a free slot in `console_id`'s table.  Returns slot id (>=0) or -1.
// Caller must hold g_anim_lock.
static int alloc_slot_locked(uint32_t console_id) {
    per_console_t *pc = &g_anim_tables[console_id];
    for (uint32_t i = 0; i < CONSOLE_ANIM_MAX; i++) {
        if (pc->slots[i].state == ANIM_STATE_IDLE) return (int)i;
    }
    return -1;
}

int console_animation_create(uint32_t console_id, uint32_t sprite_id,
                             const tui_cell_t *keyframes_kernel,
                             uint16_t n_frames,
                             uint32_t duration_per_frame_ms,
                             uint8_t interp_mode) {
    if (console_id >= NUM_CONSOLES_MAX) return -22;       // -EINVAL
    if (!keyframes_kernel) return -22;
    if (n_frames == 0 || n_frames > 64) return -22;
    if (duration_per_frame_ms == 0 || duration_per_frame_ms > 60000) return -22;
    if (sprite_id >= NUM_SPRITES) return -22;
    if (interp_mode != ANIM_INTERP_STEP) return -22;       // v1 STEP-only

    if (!g_anim_inited) console_animation_init();

    // Copy keyframes into kheap.
    size_t bytes = (size_t)n_frames * sizeof(tui_cell_t);
    tui_cell_t *kf = (tui_cell_t *)kmalloc(bytes, SUBSYS_CORE);
    if (!kf) return -12;                                  // -ENOMEM
    memcpy(kf, keyframes_kernel, bytes);

    spinlock_acquire(&g_anim_lock);
    int slot = alloc_slot_locked(console_id);
    if (slot < 0) {
        spinlock_release(&g_anim_lock);
        kfree(kf);
        return -11;                                       // -EAGAIN (table full)
    }

    console_animation_t *a = &g_anim_tables[console_id].slots[slot];
    a->console_id   = console_id;
    a->sprite_id    = sprite_id;
    a->keyframes    = kf;
    a->n_frames     = n_frames;
    a->cur_frame    = 0;
    a->interp_mode  = interp_mode;
    a->state        = ANIM_STATE_RUNNING;

    // Compute tsc-per-frame.  If TSC not calibrated yet, use a coarse
    // approximation (1 ms ≈ 2.4 GHz / 1000 = 2.4 M cycles; under TCG far
    // less, but the test substrate (DEBUG_ANIM_TICK) bypasses the clock
    // anyway).
    if (g_tsc_hz > 0) {
        a->duration_per_frame_tsc =
            (uint32_t)((uint64_t)duration_per_frame_ms * g_tsc_hz / 1000ull);
    } else {
        a->duration_per_frame_tsc = (uint32_t)duration_per_frame_ms * 2400u;
    }
    a->next_tick_tsc = rdtsc() + a->duration_per_frame_tsc;
    g_anim_tables[console_id].active |= (1u << (slot & 31));

    // Install first keyframe immediately into the sprite registry so apps
    // see the initial frame even before the first tick.
    extern int console_sprite_register(uint32_t console_id, uint32_t sprite_id,
                                       const uint8_t bitmap16[16]);
    // tui_cell_t is 16 bytes; we treat its raw bytes as a glyph bitmap so
    // the keyframes can ALSO be used as 8x16 sprites.  For now we copy the
    // bytes verbatim (test cases pre-fill bytes accordingly).
    (void)console_sprite_register(console_id, sprite_id,
                                  (const uint8_t *)&kf[0]);
    spinlock_release(&g_anim_lock);
    return slot;
}

// Step one frame on `a`.  Assumes lock held by caller.
static void step_one_locked(console_animation_t *a) {
    if (a->state != ANIM_STATE_RUNNING) return;
    a->cur_frame++;
    if (a->cur_frame >= a->n_frames) {
        a->state = ANIM_STATE_COMMITTED;
        return;
    }
    // Push next frame's bytes into the sprite registry.
    extern int console_sprite_register(uint32_t console_id, uint32_t sprite_id,
                                       const uint8_t bitmap16[16]);
    (void)console_sprite_register(a->console_id, a->sprite_id,
                                  (const uint8_t *)&a->keyframes[a->cur_frame]);
    a->next_tick_tsc += a->duration_per_frame_tsc;
}

void console_animation_tick_all(void) {
    if (!g_anim_inited) return;
    // Best-effort: cheap-skip if no console has any active animation by
    // checking the global flag indirectly (we walk all tables anyway).
    uint64_t now = rdtsc();
    spinlock_acquire(&g_anim_lock);
    for (uint32_t cid = 0; cid < NUM_CONSOLES_MAX; cid++) {
        per_console_t *pc = &g_anim_tables[cid];
        if (pc->active == 0) continue;
        for (uint32_t i = 0; i < CONSOLE_ANIM_MAX; i++) {
            console_animation_t *a = &pc->slots[i];
            if (a->state != ANIM_STATE_RUNNING) continue;
            if (now >= a->next_tick_tsc) {
                step_one_locked(a);
            }
        }
    }
    spinlock_release(&g_anim_lock);
}

// Force-tick: for DEBUG_ANIM_TICK we ignore real-time deadlines and step
// every RUNNING animation on the given console exactly once.  Used by the
// gate test to deterministically advance.
int console_animation_tick_one(uint32_t console_id) {
    if (console_id >= NUM_CONSOLES_MAX) return -22;
    spinlock_acquire(&g_anim_lock);
    per_console_t *pc = &g_anim_tables[console_id];
    int stepped = 1;
    for (uint32_t i = 0; i < CONSOLE_ANIM_MAX; i++) {
        console_animation_t *a = &pc->slots[i];
        if (a->state != ANIM_STATE_RUNNING) continue;
        step_one_locked(a);
        stepped = 0;
    }
    spinlock_release(&g_anim_lock);
    return stepped;
}

int console_animation_debug_tick(uint32_t console_id, uint32_t slot) {
    if (console_id >= NUM_CONSOLES_MAX) return -22;
    if (slot >= CONSOLE_ANIM_MAX) return -22;
    spinlock_acquire(&g_anim_lock);
    console_animation_t *a = &g_anim_tables[console_id].slots[slot];
    if (a->state != ANIM_STATE_RUNNING) {
        spinlock_release(&g_anim_lock);
        return -22;
    }
    step_one_locked(a);
    spinlock_release(&g_anim_lock);
    return 0;
}

int console_animation_debug_get_frame(uint32_t console_id, uint32_t slot) {
    if (console_id >= NUM_CONSOLES_MAX) return -1;
    if (slot >= CONSOLE_ANIM_MAX) return -1;
    spinlock_acquire(&g_anim_lock);
    console_animation_t *a = &g_anim_tables[console_id].slots[slot];
    int f = -1;
    if (a->state == ANIM_STATE_RUNNING || a->state == ANIM_STATE_COMMITTED) {
        f = (int)a->cur_frame;
    }
    spinlock_release(&g_anim_lock);
    return f;
}

int console_animation_debug_get_state(uint32_t console_id, uint32_t slot) {
    if (console_id >= NUM_CONSOLES_MAX) return -1;
    if (slot >= CONSOLE_ANIM_MAX) return -1;
    spinlock_acquire(&g_anim_lock);
    int s = (int)g_anim_tables[console_id].slots[slot].state;
    spinlock_release(&g_anim_lock);
    return s;
}
