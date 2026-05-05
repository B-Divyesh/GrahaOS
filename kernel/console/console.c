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
#include "../../drivers/video/framebuffer.h"

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

int console_render_synthetic_frame(uint32_t console_id) {
    if (console_id >= g_console_table.num_consoles) return -1;
    console_t *c = &g_console_table.consoles[console_id];
    if (!c->cell_vmo || !c->cell_vmo->pages) return -1;

    if (!g_palette256_built) palette256_build();
    const uint32_t W = c->width_cells;
    const uint32_t H = c->height_cells;
    sprite_table_t *st = (sprite_table_t *)c->sprite_table;

    for (uint32_t row = 0; row < H; row++) {
        for (uint32_t col = 0; col < W; col++) {
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
    return 0;
}
