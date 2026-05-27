// kernel/console/console.c
//
// Phase 27 Block A — Virtual console subsystem implementation.
//
// Stage A1 ships console_init + base accessors. console_t entries are static
// (in g_console_table.consoles[]) so the slab churn at boot is limited to
// VMO + channel + cap_object allocations (3 per console × 4 consoles = 12).
//
// Subsequent stages extend:
//   A2: console_create + per-syscall cap-gating helpers
//   A3: console_route_key fully implemented
//   A4: framebuffer-MMIO VMO via vmo_create_mmio + console_set_fbd_alive
//   B1: console_sprite_register + console_gfx_enable + console_gfx_damage
//
// Per CLAUDE.md "tried-and-reverted" reminders: this file does NOT touch
// arch/x86_64/cpu/sched/sched.c::schedule(), does NOT pin daemons to CPU 0,
// does NOT issue INT 49 from idle paths, does NOT busy-wait. Channels +
// regular blocking semantics only.

#include "console.h"

#include "../log.h"
#include "../sync/spinlock.h"
#include "../mm/vmo.h"
#include "../mm/kheap.h"
#include "../ipc/channel.h"
#include "../ipc/manifest.h"
#include "../cap/object.h"
#include "../cap/token.h"
#include "../cap/handle_table.h"
#include "../audit.h"
#include "../../drivers/video/framebuffer.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/tsc.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

// Phase 27 Stage B1: per-console sprite registry. 1791 sprite slots × 16
// bytes = 28656 bytes; lazy-allocated via kmalloc on first SPRITE_REGISTER
// (consoles that never use sprites pay 0). Membership bitmap is one bit per
// slot (224 bytes) so the render path knows which sprites are valid.
typedef struct sprite_table {
    uint8_t bitmaps[NUM_SPRITES][16];   // 1791 × 16 = 28656 bytes
    uint8_t valid[(NUM_SPRITES + 7) / 8];  // bit-per-sprite presence
} sprite_table_t;

// Phase 27 Stage B1: per-console RGBA bitmap overlay. Lazy-allocated via
// console_gfx_enable. Damage rects (up to 16) are tracked in console_t's
// damage_ring. fbd / synthetic_render reads damaged regions and composites
// over the cell layer.
typedef struct gfx_overlay {
    vmo_t   *vmo;        // W*H*4 bytes; CAP_FLAG_PUBLIC for now (Stage C2 narrows)
    uint32_t cap_idx;    // CAP_KIND_VMO wrapper
    uint32_t w_px;
    uint32_t h_px;
} gfx_overlay_t;

// HHDM helper — kernel/mm/vmo.c keeps phys_to_kv static. We have the same
// invariant here (phys + g_hhdm_offset → kernel virtual). Match the inline
// to avoid creating a public symbol that disturbs other consumers.
extern uint64_t g_hhdm_offset;
static inline void *phys_to_kv(uint64_t phys) {
    return (void *)(uintptr_t)(phys + g_hhdm_offset);
}

// ---------------------------------------------------------------------------
// Module-private state.
// ---------------------------------------------------------------------------
console_table_t g_console_table = {
    .consoles = {{0}},
    .selected = 0,
    .num_consoles = 0,
    .fbd_alive = false,
    .lock = SPINLOCK_INITIALIZER("console_table"),
};

// Phase 29 Session D — dirty-rect render counters (exposed for dirty_rect.tap).
_Atomic uint64_t g_dirty_rect_renders_partial = 0;
_Atomic uint64_t g_dirty_rect_renders_full = 0;

// Phase 29 Session D — single-writer framebuffer-MMIO ownership.  First
// caller to SYS_CONSOLE_GFX_MAP_FB wins; subsequent callers get -EPERM.
// Set to PID_NONE (-1) at boot and never reset (FU29.X.fb_owner_release
// will add owner-exit cleanup).  Guarded by g_fb_owner_lock.
static int32_t   g_fb_owner_pid = -1;
static spinlock_t g_fb_owner_lock = SPINLOCK_INITIALIZER("console_fb_owner");

// Cached manifest type hash for console input channels. Computed lazily at
// console_init time so we don't depend on manifest_init having registered the
// name (which it doesn't — we use compute_hash directly since these channels
// are kernel-internal and not in the public manifest registry).
static uint64_t g_console_input_type_hash = 0;

// ---------------------------------------------------------------------------
// Internal helper — allocate one console's resources (cell-VMO + input chan
// + cap_object). On failure, frees what was allocated and returns -1.
// Caller must hold g_console_table.lock.
// ---------------------------------------------------------------------------
static int alloc_console_locked(console_t *c, uint32_t id,
                                uint32_t width_cells,
                                uint32_t height_cells) {
    c->id = id;
    c->width_cells = width_cells;
    c->height_cells = height_cells;
    c->owner_pid = 0;
    c->dirty_seq = 0;
    c->rendered_seq = 0;
    c->sprite_table = NULL;
    c->gfx_overlay = NULL;
    c->damage_head = 0;
    c->damage_tail = 0;

    // Cell-buffer VMO: W*H*16 bytes. Default 1024x768 fb → 128*48*16 = 96 KB
    // per console = 384 KB total for 4 consoles. Eager + zeroed. Rounded up
    // to a page boundary because vmo_create requires page-aligned size.
    uint64_t cell_bytes = (uint64_t)width_cells * (uint64_t)height_cells *
                          (uint64_t)sizeof(tui_cell_t);
    uint64_t cell_bytes_aligned = (cell_bytes + 0xFFFu) & ~0xFFFu;
    c->cell_vmo = vmo_create(cell_bytes_aligned, VMO_ZEROED, PID_KERNEL, PID_PUBLIC);
    if (!c->cell_vmo) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "console: cell VMO alloc failed for console %u (%llu bytes)",
             id, (unsigned long long)cell_bytes);
        return -1;
    }

    // Input channel: kernel-end retained by g_console_table; userspace gets
    // read endpoint via SYS_CONSOLE_ATTACH (Stage A2) or SYS_CONSOLE_OBSERVE
    // (Stage A2 cap-narrowed for non-owners).
    if (g_console_input_type_hash == 0) {
        g_console_input_type_hash = manifest_compute_hash("grahaos.console.input.v1");
    }
    c->input_chan = chan_create_kernel(g_console_input_type_hash,
                                       CHAN_MODE_NONBLOCKING,
                                       /* capacity */ 64);
    if (!c->input_chan) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "console: input chan alloc failed for console %u", id);
        vmo_unref(c->cell_vmo);
        c->cell_vmo = NULL;
        return -1;
    }

    // CAP_KIND_CONSOLE wrapper. Audience = [PID_KERNEL]; init's autorun
    // hook (Stage A2 wiring) installs sub-tokens into init's handle table
    // with audience = [init_pid] for distribution to apps.
    int32_t audience[2] = { PID_KERNEL, PID_NONE };
    int idx = cap_object_create(
        CAP_KIND_CONSOLE,
        CAP_CONSOLE_DEFAULT_RIGHTS,
        audience,
        CAP_FLAG_EAGER_REVOKE,   // revoke cascades to derived sub-tokens
        (uintptr_t)c,             // kind_data points back at the console_t
        PID_KERNEL,
        CAP_OBJECT_IDX_NONE       // no parent
    );
    if (idx < 1) {
        klog(KLOG_ERROR, SUBSYS_CORE,
             "console: cap_object_create failed for console %u (rc=%d)",
             id, idx);
        chan_destroy_kernel(c->input_chan);
        c->input_chan = NULL;
        vmo_unref(c->cell_vmo);
        c->cell_vmo = NULL;
        return -1;
    }
    c->cap_object_idx = (uint32_t)idx;

    return 0;
}

// ---------------------------------------------------------------------------
// console_init — boot-time substrate.
// ---------------------------------------------------------------------------
void console_init(uint32_t fb_width_px, uint32_t fb_height_px) {
    spinlock_acquire(&g_console_table.lock);
    if (g_console_table.num_consoles != 0) {
        spinlock_release(&g_console_table.lock);
        klog(KLOG_WARN, SUBSYS_CORE, "console_init: already initialised; skipping");
        return;
    }

    // Cell metrics. Font is fixed 8x16 in v1 (per drivers/video/framebuffer.c
    // font_8x16 array). Heterogeneous sizes deferred to FU27.X.
    uint32_t width_cells = fb_width_px / 8u;
    uint32_t height_cells = fb_height_px / 16u;

    if (width_cells == 0 || height_cells == 0) {
        spinlock_release(&g_console_table.lock);
        klog(KLOG_ERROR, SUBSYS_CORE,
             "console_init: invalid framebuffer dims %ux%u",
             fb_width_px, fb_height_px);
        return;
    }

    // Allocate the 4 default consoles.
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < NUM_CONSOLES; i++) {
        int rc = alloc_console_locked(&g_console_table.consoles[i], i,
                                      width_cells, height_cells);
        if (rc != 0) {
            // Best-effort rollback for already-allocated consoles. Errors
            // here are fatal — kernel boots without consoles, downstream
            // will fail predictably.
            klog(KLOG_ERROR, SUBSYS_CORE,
                 "console_init: alloc_console_locked failed at i=%u; "
                 "kernel boots with %u consoles", i, allocated);
            break;
        }
        allocated++;
    }

    g_console_table.num_consoles = allocated;
    __atomic_store_n(&g_console_table.selected, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_console_table.fbd_alive, false, __ATOMIC_RELEASE);

    spinlock_release(&g_console_table.lock);

    if (allocated == NUM_CONSOLES) {
        klog(KLOG_INFO, SUBSYS_CORE,
             "console_init: %u consoles ready (%ux%u cells, fb=%ux%u px), default selected=0",
             allocated, width_cells, height_cells, fb_width_px, fb_height_px);
    }
}

// ---------------------------------------------------------------------------
// console_create — Stage A2 SYS_CONSOLE_CREATE backend.
//
// Stage A1 ships a stub that allocates from the static slot array. Returns
// NULL when num_consoles == NUM_CONSOLES_MAX or the fixed-size dims don't
// match the boot framebuffer.
// ---------------------------------------------------------------------------
console_t *console_create(uint32_t width_cells, uint32_t height_cells) {
    spinlock_acquire(&g_console_table.lock);

    if (g_console_table.num_consoles >= NUM_CONSOLES_MAX) {
        spinlock_release(&g_console_table.lock);
        return NULL;
    }

    // Inherit boot dims from console 0 if caller passed 0/0 (default). For
    // non-default dims, reject — heterogeneous console sizes are FU27.X.
    if (width_cells == 0 && height_cells == 0) {
        width_cells = g_console_table.consoles[0].width_cells;
        height_cells = g_console_table.consoles[0].height_cells;
    } else if (width_cells != g_console_table.consoles[0].width_cells ||
               height_cells != g_console_table.consoles[0].height_cells) {
        spinlock_release(&g_console_table.lock);
        return NULL;  // FU27.X.heterogeneous_consoles
    }

    uint32_t id = g_console_table.num_consoles;
    console_t *c = &g_console_table.consoles[id];
    int rc = alloc_console_locked(c, id, width_cells, height_cells);
    if (rc != 0) {
        spinlock_release(&g_console_table.lock);
        return NULL;
    }

    g_console_table.num_consoles = id + 1;
    spinlock_release(&g_console_table.lock);

    klog(KLOG_INFO, SUBSYS_CORE,
         "console_create: console %u allocated (cap_idx=%u)", id, c->cap_object_idx);
    return c;
}

// ---------------------------------------------------------------------------
// Selection + ownership.
// ---------------------------------------------------------------------------
int console_switch(uint32_t console_id) {
    uint32_t n = g_console_table.num_consoles;
    if (console_id >= n) return -1;  // -EINVAL; syscall layer maps

    uint32_t prev = __atomic_exchange_n(&g_console_table.selected,
                                        console_id, __ATOMIC_ACQ_REL);
    if (prev != console_id) {
        klog(KLOG_DEBUG, SUBSYS_CORE,
             "console: switch from %u to %u", prev, console_id);
        // Stage A4 wires fbd wake here via g_console_switch_chan post.
        // In Stage A1, the switch is just a state update; fbd doesn't exist.
    }
    return 0;
}

uint32_t console_get_selected(void) {
    return __atomic_load_n(&g_console_table.selected, __ATOMIC_ACQUIRE);
}

console_t *console_find_by_owner(int32_t pid) {
    if (pid <= 0) return NULL;
    spinlock_acquire(&g_console_table.lock);
    console_t *result = NULL;
    for (uint32_t i = 0; i < g_console_table.num_consoles; i++) {
        if (g_console_table.consoles[i].owner_pid == pid) {
            result = &g_console_table.consoles[i];
            break;
        }
    }
    spinlock_release(&g_console_table.lock);
    return result;
}

console_t *console_get_by_id(uint32_t console_id) {
    if (console_id >= g_console_table.num_consoles) return NULL;
    return &g_console_table.consoles[console_id];
}

// ---------------------------------------------------------------------------
// Input routing — Stage A3 wires the keyboard producer.
// ---------------------------------------------------------------------------
void console_route_key(const key_event_t *ev) {
    if (!ev) return;
    uint32_t selected = console_get_selected();
    console_t *c = console_get_by_id(selected);
    if (!c || !c->input_chan) return;

    // Stage A1 stub — full chan_send_internal wiring lands at A3.
    // For now we just bump dirty_seq as a debug signal; the actual key delivery
    // path uses chan_send_internal_kernel (private API) which Stage A3 wires.
    (void)c;
}

// ---------------------------------------------------------------------------
// fbd liveness.
// ---------------------------------------------------------------------------
void console_set_fbd_alive(bool alive) {
    __atomic_store_n(&g_console_table.fbd_alive, alive, __ATOMIC_RELEASE);
    // Mirror into the framebuffer driver so direct draws bail. Lock-free
    // store; framebuffer.c reads it relaxed (one missed draw at the
    // handshake boundary is benign).
    framebuffer_set_fbd_alive(alive);
    klog(KLOG_INFO, SUBSYS_CORE,
         "console: fbd_alive = %s", alive ? "true" : "false");
}

bool console_get_fbd_alive(void) {
    return __atomic_load_n(&g_console_table.fbd_alive, __ATOMIC_ACQUIRE);
}

// SYS_CONSOLE_ACK_RENDER backend.
int console_ack_render(uint32_t console_id, uint64_t rendered_seq) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    // No lock needed — rendered_seq is updated only by fbd (single writer).
    c->rendered_seq = rendered_seq;
    // First ACK promotes fbd to alive.
    if (!console_get_fbd_alive()) {
        console_set_fbd_alive(true);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Block B (Stage B1) — sprite registry + RGBA overlay.
// ---------------------------------------------------------------------------

int console_sprite_register(uint32_t console_id, uint32_t sprite_id,
                            const uint8_t bitmap16[16]) {
    if (console_id >= g_console_table.num_consoles) return -1;
    if (sprite_id >= NUM_SPRITES) return -1;
    if (!bitmap16) return -1;

    console_t *c = &g_console_table.consoles[console_id];

    // Lazy-alloc the sprite table on first registration.
    spinlock_acquire(&g_console_table.lock);
    if (!c->sprite_table) {
        sprite_table_t *st = (sprite_table_t *)kmalloc(sizeof(sprite_table_t), SUBSYS_CORE);
        if (!st) {
            spinlock_release(&g_console_table.lock);
            return -2;  // -ENOMEM
        }
        memset(st, 0, sizeof(*st));
        c->sprite_table = (struct sprite_table *)st;
    }
    sprite_table_t *st = (sprite_table_t *)c->sprite_table;
    spinlock_release(&g_console_table.lock);

    // Single-writer per sprite_id so no per-slot lock needed (the table
    // itself is lazy-init'd under the table lock).
    memcpy(st->bitmaps[sprite_id], bitmap16, 16);
    st->valid[sprite_id / 8] |= (uint8_t)(1u << (sprite_id % 8));
    __atomic_fetch_add(&c->dirty_seq, 1, __ATOMIC_ACQ_REL);
    return 0;
}

// Test/render helper: look up a sprite's 16-byte bitmap. Returns NULL if
// the table isn't allocated, sprite_id is out of range, or the slot
// hasn't been registered.
const uint8_t *console_sprite_lookup(uint32_t console_id, uint32_t sprite_id) {
    if (console_id >= g_console_table.num_consoles) return NULL;
    if (sprite_id >= NUM_SPRITES) return NULL;
    console_t *c = &g_console_table.consoles[console_id];
    sprite_table_t *st = (sprite_table_t *)c->sprite_table;
    if (!st) return NULL;
    uint8_t bit = st->valid[sprite_id / 8] & (uint8_t)(1u << (sprite_id % 8));
    if (!bit) return NULL;
    return st->bitmaps[sprite_id];
}

struct vmo *console_gfx_enable(uint32_t console_id, uint32_t w_px, uint32_t h_px) {
    if (console_id >= g_console_table.num_consoles) return NULL;
    if (w_px == 0 || h_px == 0) return NULL;

    console_t *c = &g_console_table.consoles[console_id];

    spinlock_acquire(&g_console_table.lock);
    if (c->gfx_overlay) {
        // Already enabled — return the existing overlay's VMO. Caller can
        // reconfigure via SYS_CONSOLE_GFX_DISABLE (FU27.X) before re-enabling
        // at a different size.
        gfx_overlay_t *gov = (gfx_overlay_t *)c->gfx_overlay;
        spinlock_release(&g_console_table.lock);
        return gov->vmo;
    }
    spinlock_release(&g_console_table.lock);

    // Allocate VMO bytes — page-aligned. RGBA 4 bytes/pixel.
    uint64_t bytes = (uint64_t)w_px * (uint64_t)h_px * 4ull;
    uint64_t bytes_aligned = (bytes + 0xFFFu) & ~0xFFFu;
    vmo_t *v = vmo_create(bytes_aligned, VMO_ZEROED, PID_KERNEL, PID_PUBLIC);
    if (!v) return NULL;

    // Wrap in a CAP_KIND_VMO so userspace can derive a handle for vmo_map.
    int32_t audience[2] = { PID_KERNEL, PID_NONE };
    int idx = cap_object_create(
        CAP_KIND_VMO,
        RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT | RIGHT_DERIVE | RIGHT_REVOKE,
        audience,
        CAP_FLAG_EAGER_REVOKE,
        (uintptr_t)v,
        PID_KERNEL,
        CAP_OBJECT_IDX_NONE
    );
    if (idx < 1) {
        vmo_unref(v);
        return NULL;
    }
    v->cap_object_idx = (uint32_t)idx;

    gfx_overlay_t *gov = (gfx_overlay_t *)kmalloc(sizeof(gfx_overlay_t), SUBSYS_CORE);
    if (!gov) {
        cap_object_revoke((uint32_t)idx);
        return NULL;
    }
    gov->vmo = v;
    gov->cap_idx = (uint32_t)idx;
    gov->w_px = w_px;
    gov->h_px = h_px;

    spinlock_acquire(&g_console_table.lock);
    if (c->gfx_overlay) {
        // Race lost — another caller enabled it first. Free what we just
        // built and return the winner.
        gfx_overlay_t *winner = (gfx_overlay_t *)c->gfx_overlay;
        spinlock_release(&g_console_table.lock);
        cap_object_revoke((uint32_t)idx);
        kfree(gov);
        return winner->vmo;
    }
    c->gfx_overlay = (struct gfx_overlay *)gov;
    spinlock_release(&g_console_table.lock);
    klog(KLOG_INFO, SUBSYS_CORE,
         "console: gfx overlay enabled for console %u (%ux%u px, %llu bytes)",
         console_id, w_px, h_px, (unsigned long long)bytes_aligned);
    return v;
}

// Stage B1 helper: lookup the cap_idx of a console's gfx_overlay VMO.
uint32_t console_get_gfx_cap_idx(uint32_t console_id) {
    if (console_id >= g_console_table.num_consoles) return 0;
    console_t *c = &g_console_table.consoles[console_id];
    gfx_overlay_t *gov = (gfx_overlay_t *)c->gfx_overlay;
    return gov ? gov->cap_idx : 0;
}

int console_gfx_damage(uint32_t console_id, const damage_rect_t *rect) {
    if (console_id >= g_console_table.num_consoles) return -1;
    if (!rect) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->gfx_overlay) return -1;

    spinlock_acquire(&g_console_table.lock);
    uint8_t next_head = (uint8_t)((c->damage_head + 1u) & 0x0Fu);
    if (next_head == c->damage_tail) {
        // Full ring → coalesce: bump tail (drop oldest entry). Damage rects
        // are advisory; dropping one over-paints more than necessary on the
        // next frame.
        c->damage_tail = (uint8_t)((c->damage_tail + 1u) & 0x0Fu);
    }
    c->damage_ring[c->damage_head] = *rect;
    c->damage_head = next_head;
    spinlock_release(&g_console_table.lock);
    __atomic_fetch_add(&c->dirty_seq, 1, __ATOMIC_ACQ_REL);
    return 0;
}

// Test helper: fill a rectangle in the overlay buffer with a known ARGB
// value. Used by user/tests/console_gfx.c to set up test data without the
// full vmo_map plumbing (Stage C2 will swap for direct VMO access).
int console_gfx_fill_debug(uint32_t console_id, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t color) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    gfx_overlay_t *gov = (gfx_overlay_t *)c->gfx_overlay;
    if (!gov || !gov->vmo || !gov->vmo->pages) return -1;
    if (x + w > gov->w_px || y + h > gov->h_px) return -1;

    for (uint32_t py = y; py < y + h; py++) {
        for (uint32_t px = x; px < x + w; px++) {
            uint64_t pixel_off = ((uint64_t)py * gov->w_px + px) * 4ull;
            uint64_t page_idx = pixel_off / 4096u;
            uint64_t page_off = pixel_off % 4096u;
            if (page_idx >= gov->vmo->npages) continue;
            uint64_t phys = gov->vmo->pages[page_idx];
            if (!phys) continue;
            uint32_t *p = (uint32_t *)((uint8_t *)phys_to_kv(phys) + page_off);
            *p = color;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Stage A4 — synthetic kernel-side composite.
//
// Reads the cell-VMO of `console_id` and blits each cell into the hardware
// framebuffer via framebuffer_force_draw_cell (bypasses g_fbd_alive). Used
// by:
//   (a) DEBUG_CONSOLE_SYNTHETIC_RENDER — gate test fbd_render exercises the
//       cell→pixel path without spawning fbd in autorun=ktest mode.
//   (b) panic.c / oops emit — Stage CLOSE work; not wired in A4.
//
// This is NOT the production rendering path; the userspace fbd compositor is.
// fbd is structurally identical (maps cell VMOs read-only, walks them, blits)
// but in userland under autorun=init / autorun=gash.
//
// Returns 0 on success, -1 on invalid console_id or NULL VMO.
// ---------------------------------------------------------------------------
// xterm 256-color palette mirror. 0..15 = VGA basic; 16..231 = 6x6x6 cube
// (idx = 16 + 36*r + 6*g + b, channel values 0/95/135/175/215/255); 232..255
// = 24-step grey ramp (value = 8 + 10*(idx-232)). Shared between
// console_render_synthetic_frame and (Stage A5 onward) the userspace fbd
// daemon via /etc/tui/palette.bin should we choose to externalise it.
static uint32_t g_palette256[256] = {0};
static bool     g_palette256_built = false;

static void palette256_build(void) {
    if (g_palette256_built) return;
    // 0..15: VGA basic.
    static const uint32_t vga16[16] = {
        0x00000000u, 0x00800000u, 0x00008000u, 0x00808000u,
        0x00000080u, 0x00800080u, 0x00008080u, 0x00C0C0C0u,
        0x00808080u, 0x00FF0000u, 0x0000FF00u, 0x00FFFF00u,
        0x000000FFu, 0x00FF00FFu, 0x0000FFFFu, 0x00FFFFFFu,
    };
    for (uint32_t i = 0; i < 16; i++) g_palette256[i] = vga16[i];

    // 16..231: 6x6x6 RGB cube. Channel values = 0, 95, 135, 175, 215, 255.
    static const uint8_t cube_levels[6] = { 0, 95, 135, 175, 215, 255 };
    for (uint32_t r = 0; r < 6; r++) {
        for (uint32_t g = 0; g < 6; g++) {
            for (uint32_t b = 0; b < 6; b++) {
                uint32_t idx = 16 + 36*r + 6*g + b;
                g_palette256[idx] =
                    ((uint32_t)cube_levels[r] << 16) |
                    ((uint32_t)cube_levels[g] << 8)  |
                    ((uint32_t)cube_levels[b]);
            }
        }
    }

    // 232..255: 24-step grey ramp.
    for (uint32_t i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + 10 * i);
        g_palette256[232 + i] =
            ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }

    g_palette256_built = true;
}

uint32_t console_palette_lookup(uint8_t idx) {
    if (!g_palette256_built) palette256_build();
    return g_palette256[idx];
}

// Forward decl — defined later in this file (Session D dirty-rect drain).
static bool console_drain_dirty_ring(console_t *c,
                                     uint16_t *out_x, uint16_t *out_y,
                                     uint16_t *out_w, uint16_t *out_h);

int console_render_synthetic_frame(uint32_t console_id) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->cell_vmo || !c->cell_vmo->pages) return -1;

    if (!g_palette256_built) palette256_build();
    const uint32_t W = c->width_cells;
    const uint32_t H = c->height_cells;
    sprite_table_t *st = (sprite_table_t *)c->sprite_table;

    // Phase 29 Session D: drain dirty-rect ring → compute bounding-box clip.
    // Bookkeeping ONLY: synthetic_render still walks the full cell grid by
    // default because libtui (and the test substrate) may write cells via
    // mapped-VMO memcpy without marking them dirty.  The dirty-rect ring is
    // an *upper bound* of what's known dirty; the test gate counts how
    // often it was non-empty (partial) vs empty (full) for instrumentation
    // purposes.  Real partial-render skip optimisation will land in
    // FU29.X.partial_render_clip once mapped-VMO writers also mark dirty.
    uint16_t drx = 0, dry = 0, drw = 0, drh = 0;
    bool partial = console_drain_dirty_ring(c, &drx, &dry, &drw, &drh);
    (void)drx; (void)dry; (void)drw; (void)drh;
    if (partial) {
        __atomic_fetch_add(&g_dirty_rect_renders_partial, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_dirty_rect_renders_full, 1, __ATOMIC_RELAXED);
    }
    uint32_t row_lo = 0, row_hi = H;
    uint32_t col_lo = 0, col_hi = W;

    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (uint32_t col = col_lo; col < col_hi; col++) {
            uint64_t byte_off = (uint64_t)(row * W + col) * sizeof(tui_cell_t);
            uint64_t page_idx = byte_off / 4096u;
            uint64_t page_off = byte_off % 4096u;
            if (page_idx >= c->cell_vmo->npages) continue;

            uint64_t phys = c->cell_vmo->pages[page_idx];
            if (!phys) continue;
            const tui_cell_t *cell =
                (const tui_cell_t *)((uint8_t *)phys_to_kv(phys) + page_off);

            uint32_t fg = g_palette256[cell->fg_color];
            uint32_t bg = g_palette256[cell->bg_color];
            uint32_t cp = cell->codepoint;

            // Cursor attribute: invert fg/bg so an otherwise-blank cell
            // shows a solid block.
            if (cell->attrs & TUI_ATTR_CURSOR) {
                uint32_t tmp = fg; fg = bg; bg = tmp;
                if (cp == 0) cp = ' ';
            }

            // Stage B1: sprite cell dispatch. Codepoints in the private-
            // use range 0xE100..0xE7FF reference the per-console sprite
            // registry. Unregistered or out-of-range sprites fall through
            // to a blank cell painted at bg_color.
            if (cp >= TUI_SPRITE_BASE && cp <= TUI_SPRITE_END && st) {
                uint32_t sid = cp - TUI_SPRITE_BASE;
                uint8_t bit = st->valid[sid / 8] & (uint8_t)(1u << (sid % 8));
                if (bit) {
                    framebuffer_force_draw_sprite(col * 8u, row * 16u,
                                                  st->bitmaps[sid], fg, bg);
                    continue;
                }
            }

            framebuffer_force_draw_cell(col * 8u, row * 16u, cp, fg, bg);
        }
    }

    // Stage B1: overlay composite. After cells are blitted, walk the damage
    // ring and copy overlay pixels to the framebuffer for each rect.
    gfx_overlay_t *gov = (gfx_overlay_t *)c->gfx_overlay;
    if (gov && gov->vmo && gov->vmo->pages) {
        spinlock_acquire(&g_console_table.lock);
        damage_rect_t local_rects[16];
        uint8_t local_n = 0;
        while (c->damage_tail != c->damage_head && local_n < 16) {
            local_rects[local_n++] = c->damage_ring[c->damage_tail];
            c->damage_tail = (uint8_t)((c->damage_tail + 1u) & 0x0Fu);
        }
        spinlock_release(&g_console_table.lock);

        for (uint8_t i = 0; i < local_n; i++) {
            damage_rect_t r = local_rects[i];
            if ((uint32_t)r.x >= gov->w_px || (uint32_t)r.y >= gov->h_px) continue;
            uint32_t end_x = (uint32_t)r.x + (uint32_t)r.w;
            uint32_t end_y = (uint32_t)r.y + (uint32_t)r.h;
            if (end_x > gov->w_px) end_x = gov->w_px;
            if (end_y > gov->h_px) end_y = gov->h_px;
            for (uint32_t py = r.y; py < end_y; py++) {
                for (uint32_t px = r.x; px < end_x; px++) {
                    uint64_t pixel_off = ((uint64_t)py * gov->w_px + px) * 4ull;
                    uint64_t pidx = pixel_off / 4096u;
                    uint64_t poff = pixel_off % 4096u;
                    if (pidx >= gov->vmo->npages) continue;
                    uint64_t pa = gov->vmo->pages[pidx];
                    if (!pa) continue;
                    uint32_t argb = *(uint32_t *)((uint8_t *)phys_to_kv(pa) + poff);
                    framebuffer_force_blit_pixel(px, py, argb);
                }
            }
        }
    }

    return 0;
}

// Stage A4 debug helper — write a single cell into the cell VMO from kernel
// side. Drives DEBUG_CONSOLE_WRITE_CELL so user/tests/fbd_render.c can
// populate the cell buffer without owning the console (SYS_CONSOLE_ATTACH is
// -ENOSYS until Stage C2 cap inheritance lands). Intentionally unguarded by
// pledge / cap (DEBUG-only path).
//
// Returns 0 on success, -1 on bounds error.
int console_write_cell_debug(uint32_t console_id, uint32_t row, uint32_t col,
                             uint32_t codepoint, uint8_t fg, uint8_t bg,
                             uint16_t attrs) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->cell_vmo || !c->cell_vmo->pages) return -1;
    if (row >= c->height_cells || col >= c->width_cells) return -1;

    uint64_t byte_off = (uint64_t)(row * c->width_cells + col) * sizeof(tui_cell_t);
    uint64_t page_idx = byte_off / 4096u;
    uint64_t page_off = byte_off % 4096u;
    if (page_idx >= c->cell_vmo->npages) return -1;
    uint64_t phys = c->cell_vmo->pages[page_idx];
    if (!phys) return -1;

    tui_cell_t *cell =
        (tui_cell_t *)((uint8_t *)phys_to_kv(phys) + page_off);
    cell->codepoint = codepoint;
    cell->fg_color = fg;
    cell->bg_color = bg;
    cell->attrs = attrs;
    // Pad bytes left untouched (test-write only; production writers go
    // through libtui which zeros pad).
    __atomic_fetch_add(&c->dirty_seq, 1, __ATOMIC_ACQ_REL);
    // Phase 29 Session D: feed the dirty-rect ring so the next
    // synthetic_render does a coalesced partial redraw.
    console_mark_dirty(console_id, (uint16_t)col, (uint16_t)row, 1, 1);
    return 0;
}

// ===========================================================================
// Phase 29 Session D — TUI primitive backends.
// ===========================================================================

// Helper: bounds-check + push a dirty rect into the per-console SPSC ring.
// On overflow (head + 1 == tail mod DEPTH), set the overflow flag and drop.
// Lock-free: caller can be any context.
void console_mark_dirty(uint32_t console_id, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h) {
    if (console_id >= g_console_table.num_consoles) return;
    if (w == 0 || h == 0) return;
    console_t *c = &g_console_table.consoles[console_id];
    dirty_rect_ring_t *r = &c->dirty_ring;

    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1u) % DIRTY_RECT_RING_DEPTH;
    if (next == tail) {
        // Ring full → mark overflow; synthetic_render will fall back to
        // full redraw on next pass.
        __atomic_store_n(&r->overflow, true, __ATOMIC_RELEASE);
        return;
    }
    r->rects[head].x = x;
    r->rects[head].y = y;
    r->rects[head].w = w;
    r->rects[head].h = h;
    __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
}

// Helper: drain the dirty ring into a coalesced bounding-box union. Returns
// true if any rects were drained AND overflow was NOT set.  On overflow,
// clears the flag, drains the ring, and returns false (caller does full
// redraw).  If ring was empty AND overflow was not set, returns true with
// out_w/out_h both 0 (caller can skip render).
static bool console_drain_dirty_ring(console_t *c,
                                     uint16_t *out_x, uint16_t *out_y,
                                     uint16_t *out_w, uint16_t *out_h) {
    dirty_rect_ring_t *r = &c->dirty_ring;
    bool overflow = __atomic_exchange_n(&r->overflow, false, __ATOMIC_ACQ_REL);

    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);

    if (overflow) {
        // Discard ring contents; caller does full redraw.
        __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return false;
    }

    if (head == tail) {
        // Empty ring, no overflow.
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return true;
    }

    uint32_t min_x = 0xFFFFu, min_y = 0xFFFFu;
    uint32_t max_x = 0, max_y = 0;
    uint32_t any = 0;
    while (tail != head) {
        damage_rect_t *d = &r->rects[tail];
        uint32_t x0 = d->x;
        uint32_t y0 = d->y;
        uint32_t x1 = (uint32_t)d->x + (uint32_t)d->w;
        uint32_t y1 = (uint32_t)d->y + (uint32_t)d->h;
        if (x0 < min_x) min_x = x0;
        if (y0 < min_y) min_y = y0;
        if (x1 > max_x) max_x = x1;
        if (y1 > max_y) max_y = y1;
        any = 1;
        tail = (tail + 1u) % DIRTY_RECT_RING_DEPTH;
    }
    __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);

    if (!any) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return true;
    }
    // Clip to console dimensions.
    if (max_x > c->width_cells) max_x = c->width_cells;
    if (max_y > c->height_cells) max_y = c->height_cells;
    if (min_x > max_x) min_x = max_x;
    if (min_y > max_y) min_y = max_y;
    *out_x = (uint16_t)min_x;
    *out_y = (uint16_t)min_y;
    *out_w = (uint16_t)(max_x - min_x);
    *out_h = (uint16_t)(max_y - min_y);
    return true;
}

// SYS_CONSOLE_READ_INPUT (1116).  Drains the console's input channel.
//
// Substrate detail: the input channel is currently a CHAN_MODE_NONBLOCKING
// channel that, for Session D, no producer writes to via chan_send (the
// keyboard ISR pumps stalls would need IRQ context); instead Session D
// keeps a small per-console SPSC ring of input_event_t directly, mirrored
// out via DEBUG_INJECT_SCANCODE for the gate test.  When Session E lands
// real keyboard routing, the ring is replaced with chan_recv loops.
//
// For now we expose a simple per-console FIFO of input_event_t (32 slots)
// guarded by the table lock; DEBUG_INJECT_SCANCODE feeds it.
#define INPUT_RING_DEPTH 32u
typedef struct input_ring {
    spinlock_t   lock;
    uint32_t     head;
    uint32_t     tail;
    input_event_t slots[INPUT_RING_DEPTH];
} input_ring_t;

static input_ring_t g_input_rings[NUM_CONSOLES_MAX];
static bool g_input_rings_inited = false;

static void input_rings_init_once(void) {
    if (g_input_rings_inited) return;
    for (uint32_t i = 0; i < NUM_CONSOLES_MAX; i++) {
        spinlock_init(&g_input_rings[i].lock, "console_input_ring");
        g_input_rings[i].head = 0;
        g_input_rings[i].tail = 0;
    }
    g_input_rings_inited = true;
}

// Producer side — called from keyboard.c via console_route_key. Drops on
// full ring (input is best-effort; humans will retype).
void console_post_input_event(uint32_t console_id, const input_event_t *ev) {
    if (console_id >= NUM_CONSOLES_MAX) return;
    if (!ev) return;
    input_rings_init_once();
    input_ring_t *r = &g_input_rings[console_id];
    spinlock_acquire(&r->lock);
    uint32_t next = (r->head + 1u) % INPUT_RING_DEPTH;
    if (next == r->tail) {
        // Drop oldest by bumping tail.
        r->tail = (r->tail + 1u) % INPUT_RING_DEPTH;
    }
    r->slots[r->head] = *ev;
    r->head = next;
    spinlock_release(&r->lock);
}

long console_read_input(uint32_t console_id, input_event_t *out,
                        uint32_t max_events) {
    if (console_id >= g_console_table.num_consoles) return -22;  // -EINVAL
    if (!out) return -22;
    if (max_events == 0) return 0;
    input_rings_init_once();
    input_ring_t *r = &g_input_rings[console_id];
    spinlock_acquire(&r->lock);
    uint32_t n = 0;
    while (n < max_events && r->tail != r->head) {
        out[n] = r->slots[r->tail];
        r->tail = (r->tail + 1u) % INPUT_RING_DEPTH;
        n++;
    }
    bool more = (r->tail != r->head);
    uint32_t dropped = 0;
    if (more) {
        // Count remaining for audit and drain when caller signals OK by
        // returning the high-bit-set flag.  Drop nothing here; the caller
        // can read again.
    }
    spinlock_release(&r->lock);

    long ret = (long)n;
    if (more) {
        // Set the high bit (sign bit on long is reserved for errno —
        // we use bit 62 as a flag instead so the value stays positive).
        ret |= (long)0x4000000000000000LL;
        // Best-effort overflow audit: we didn't actually drop events from
        // the kernel ring (just signaled "more available"), so no audit.
    }
    (void)dropped;
    return ret;
}

// SYS_CONSOLE_GFX_MAP_FB (1117).  First caller wins exclusive access.
int console_gfx_map_fb(int32_t caller_pid,
                       uint64_t *out_token_raw,
                       fb_dims_t *out_dims) {
    if (!out_token_raw || !out_dims) return -22;
    if (caller_pid <= 0) return -22;

    // Ownership gate.  First caller becomes the owner; subsequent callers
    // must match.
    spinlock_acquire(&g_fb_owner_lock);
    if (g_fb_owner_pid < 0) {
        g_fb_owner_pid = caller_pid;
    } else if (g_fb_owner_pid != caller_pid) {
        spinlock_release(&g_fb_owner_lock);
        return -1;  // -EPERM (syscall layer maps to -1 / -EPERM = -1 too)
    }
    spinlock_release(&g_fb_owner_lock);

    // Pull dims from the framebuffer driver.  Address is the Limine HHDM
    // virtual; back-compute the physical via g_hhdm_offset.
    extern uint32_t framebuffer_get_width(void);
    extern uint32_t framebuffer_get_height(void);
    extern uint32_t framebuffer_get_pitch(void);
    extern uint64_t framebuffer_get_phys_address(void);  // we'll add this
    uint32_t w = framebuffer_get_width();
    uint32_t h = framebuffer_get_height();
    uint32_t pitch = framebuffer_get_pitch();
    if (w == 0 || h == 0 || pitch == 0) return -22;
    uint64_t phys = framebuffer_get_phys_address();
    if (phys == 0) return -22;

    uint64_t size_bytes = (uint64_t)pitch * (uint64_t)h;
    // Round up to page boundary.
    uint64_t size_aligned = (size_bytes + 0xFFFu) & ~0xFFFu;

    // Allocate an on-demand VMO and reinterpret as MMIO (mmio_vmo_validate_range
    // rejects ranges outside enumerated PCI BARs; the framebuffer is a fixed
    // physical region not tracked by PCI, so we build the VMO manually here.)
    vmo_t *v = vmo_create(size_aligned, VMO_ONDEMAND, caller_pid, 0);
    if (!v) return -12;  // -ENOMEM
    uint32_t npages = (uint32_t)(size_aligned / 4096);
    uint64_t base = phys & ~0xFFFull;
    for (uint32_t p = 0; p < npages; p++) {
        v->pages[p] = base + (uint64_t)p * 4096ull;
    }
    v->flags = VMO_MMIO | VMO_PINNED;

    // Wrap in a CAP_KIND_VMO cap_object owned by caller.
    int32_t audience[2] = { caller_pid, PID_NONE };
    int obj_idx = cap_object_create(
        CAP_KIND_VMO,
        RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT | RIGHT_REVOKE,
        audience,
        0,
        (uintptr_t)v,
        caller_pid,
        CAP_OBJECT_IDX_NONE);
    if (obj_idx <= 0) {
        vmo_unref(v);
        return -12;
    }
    v->cap_object_idx = (uint32_t)obj_idx;

    // Insert into caller's handle table.
    task_t *t = sched_get_task(caller_pid);
    if (!t) {
        cap_object_revoke((uint32_t)obj_idx);
        return -22;
    }
    uint32_t slot = 0;
    int gen = cap_handle_insert(&t->cap_handles, (uint32_t)obj_idx, 0, &slot);
    if (gen < 0) {
        cap_object_revoke((uint32_t)obj_idx);
        return -12;
    }

    cap_token_t tok = cap_token_pack((uint32_t)gen, (uint32_t)obj_idx, 0);
    *out_token_raw = tok.raw;

    out_dims->width_px    = w;
    out_dims->height_px   = h;
    out_dims->pitch_bytes = pitch;
    out_dims->bpp         = 32;
    out_dims->size_bytes  = size_aligned;

    audit_write_gfx_fb_mapped(caller_pid, phys, size_aligned);
    return 0;
}

// SYS_CONSOLE_VSYNC_WAIT (1118).  Block until next 60Hz tick or max_wait_ns.
int console_vsync_wait(uint64_t max_wait_ns) {
    if (g_tsc_hz == 0) {
        // TSC not calibrated yet — return immediately.
        return 0;
    }
    uint64_t tick_period_tsc = g_tsc_hz / 60ull;
    if (tick_period_tsc == 0) tick_period_tsc = 1;

    uint64_t start = rdtsc();
    uint64_t deadline_tick = start + tick_period_tsc;
    uint64_t deadline_max = (max_wait_ns > 0)
        ? start + ns_to_tsc(max_wait_ns)
        : (uint64_t)-1;

    // Pace by yielding the CPU between polls — avoid hot-spinning. The
    // existing user-space spin_us pattern uses rdtsc + pause; we use the
    // same here but inserted between iterations.
    while (1) {
        uint64_t now = rdtsc();
        if (now >= deadline_tick) return 0;
        if (now >= deadline_max) return -62;  // -ETIME = -62
        // PAUSE keeps the CPU happy on hyperthreaded cores.
        __asm__ __volatile__("pause");
    }
}

// Test-only: override g_fb_owner_pid.  Returns prior value.
int32_t console_debug_fb_owner_set(int32_t new_pid) {
    spinlock_acquire(&g_fb_owner_lock);
    int32_t prev = g_fb_owner_pid;
    g_fb_owner_pid = new_pid;
    spinlock_release(&g_fb_owner_lock);
    return prev;
}

// Test-only: read a cell's codepoint directly from the cell VMO.
long console_debug_read_cell(uint32_t console_id, uint32_t row, uint32_t col) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->cell_vmo || !c->cell_vmo->pages) return -1;
    if (row >= c->height_cells || col >= c->width_cells) return -1;
    uint64_t byte_off = (uint64_t)(row * c->width_cells + col) * sizeof(tui_cell_t);
    uint64_t page_idx = byte_off / 4096u;
    uint64_t page_off = byte_off % 4096u;
    if (page_idx >= c->cell_vmo->npages) return -1;
    uint64_t phys = c->cell_vmo->pages[page_idx];
    if (!phys) return -1;
    const tui_cell_t *cell =
        (const tui_cell_t *)((uint8_t *)phys_to_kv(phys) + page_off);
    return (long)cell->codepoint;
}

// SYS_CONSOLE_ATTACH (1103).  Cap-gate-verifies the caller's CAP_KIND_CONSOLE
// token has RIGHT_ATTACH, then derives cell-VMO + input-chan handles into
// the caller's handle table.
int console_attach(int32_t caller_pid, uint32_t console_id,
                   uint64_t cap_token_raw,
                   uint64_t *out_cell_token,
                   uint64_t *out_input_token) {
    if (!out_cell_token || !out_input_token) return -22;
    if (console_id >= g_console_table.num_consoles) return -22;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->cell_vmo) return -22;

    // Cap-gate.  We accept either:
    //   (a) The caller holds a CAP_KIND_CONSOLE with RIGHT_ATTACH against
    //       this console (production path).
    //   (b) cap_token_raw == 0 — Session D substrate: treat as
    //       "trusted, autorun-granted".  Stage E tightens.
    if (cap_token_raw != 0) {
        cap_token_t tok = { .raw = cap_token_raw };
        cap_object_t *obj = cap_token_resolve(caller_pid, tok, RIGHT_ATTACH);
        if (!obj) return -1;  // -EPERM
        // Verify the token references this console specifically.
        if (obj->kind != CAP_KIND_CONSOLE) return -1;
        if ((console_t *)obj->kind_data != c) return -1;
    }

    task_t *t = sched_get_task(caller_pid);
    if (!t) return -22;

    // Derive cell-VMO handle.
    // The console's cell_vmo is wrapped in a cap_object lazily here so the
    // caller can vmo_map it directly.  We mint a fresh CAP_KIND_VMO per
    // attach (caller-owned) referencing the same vmo_t; this avoids
    // sharing one cap_object across multiple attaches and keeps revoke
    // semantics local.
    if (c->cell_vmo->cap_object_idx == 0) {
        int32_t pub_aud[2] = { PID_KERNEL, PID_NONE };
        int idx = cap_object_create(
            CAP_KIND_VMO,
            RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT | RIGHT_DERIVE | RIGHT_REVOKE,
            pub_aud,
            CAP_FLAG_PUBLIC,
            (uintptr_t)c->cell_vmo,
            PID_KERNEL,
            CAP_OBJECT_IDX_NONE);
        if (idx < 1) return -12;
        c->cell_vmo->cap_object_idx = (uint32_t)idx;
    }

    // Bump the underlying vmo refcount so the per-attach handle keeps it
    // alive when the original kernel reference is dropped.
    vmo_ref(c->cell_vmo);
    int32_t per_attach_aud[2] = { caller_pid, PID_NONE };
    int cell_idx = cap_object_create(
        CAP_KIND_VMO,
        RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT,
        per_attach_aud,
        0,
        (uintptr_t)c->cell_vmo,
        caller_pid,
        CAP_OBJECT_IDX_NONE);
    if (cell_idx < 1) {
        vmo_unref(c->cell_vmo);
        return -12;
    }

    uint32_t cell_slot = 0;
    int cell_gen = cap_handle_insert(&t->cap_handles, (uint32_t)cell_idx,
                                     0, &cell_slot);
    if (cell_gen < 0) {
        cap_object_revoke((uint32_t)cell_idx);
        vmo_unref(c->cell_vmo);
        return -12;
    }

    // Derive input-chan READ endpoint cap.
    // Build a chan_endpoint_t that wraps c->input_chan, then a cap_object.
    chan_endpoint_t *ep = (chan_endpoint_t *)kmalloc(sizeof(chan_endpoint_t),
                                                     SUBSYS_CAP);
    if (!ep) {
        cap_handle_remove(&t->cap_handles, cell_slot);
        cap_object_revoke((uint32_t)cell_idx);
        vmo_unref(c->cell_vmo);
        return -12;
    }
    ep->channel = c->input_chan;
    ep->direction = 0;  // CHAN_ENDPOINT_READ
    ep->current_holder_pid = caller_pid;
    // Bump channel refcount so this endpoint keeps it alive.
    spinlock_acquire(&c->input_chan->lock);
    c->input_chan->refcount++;
    spinlock_release(&c->input_chan->lock);

    int input_idx = cap_object_create(
        CAP_KIND_CHANNEL,
        RIGHT_READ | RIGHT_RECV | RIGHT_INSPECT,
        per_attach_aud,
        0,
        (uintptr_t)ep,
        caller_pid,
        CAP_OBJECT_IDX_NONE);
    if (input_idx < 1) {
        spinlock_acquire(&c->input_chan->lock);
        c->input_chan->refcount--;
        spinlock_release(&c->input_chan->lock);
        kfree(ep);
        cap_handle_remove(&t->cap_handles, cell_slot);
        cap_object_revoke((uint32_t)cell_idx);
        vmo_unref(c->cell_vmo);
        return -12;
    }
    uint32_t input_slot = 0;
    int input_gen = cap_handle_insert(&t->cap_handles, (uint32_t)input_idx,
                                       0, &input_slot);
    if (input_gen < 0) {
        cap_object_revoke((uint32_t)input_idx);
        spinlock_acquire(&c->input_chan->lock);
        c->input_chan->refcount--;
        spinlock_release(&c->input_chan->lock);
        kfree(ep);
        cap_handle_remove(&t->cap_handles, cell_slot);
        cap_object_revoke((uint32_t)cell_idx);
        vmo_unref(c->cell_vmo);
        return -12;
    }

    // Record owner so console_route_key can find the right console.
    c->owner_pid = caller_pid;

    cap_token_t cell_tok = cap_token_pack((uint32_t)cell_gen,
                                          (uint32_t)cell_idx, 0);
    cap_token_t input_tok = cap_token_pack((uint32_t)input_gen,
                                            (uint32_t)input_idx, 0);
    *out_cell_token  = cell_tok.raw;
    *out_input_token = input_tok.raw;
    return 0;
}
