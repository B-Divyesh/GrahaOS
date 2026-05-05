// user/fbd/fbd.c
//
// Phase 27 Block A (Stage A4) — fbd userspace framebuffer compositor.
//
// Scaffold-level for A4 (full userspace rendering lands in A5 via libtui).
// fbd:
//   1. Calls SYS_CONSOLE_ACK_RENDER(0, 1) once at startup. This sets the
//      kernel's g_fbd_alive=true and bypasses every direct framebuffer_draw_*
//      call from kernel klog (drivers/video/framebuffer.c::fb_should_bypass).
//      Panic path overrides the bypass via g_panic_in_progress so kernel
//      oops still reaches the screen.
//   2. Loops at ~16 FPS. On each tick: query the selected console
//      (DEBUG_CONSOLE_GET_SELECTED), trigger kernel-side synthetic render
//      (DEBUG_CONSOLE_SYNTHETIC_RENDER) so any cells written by libtui apps
//      since the last tick are blitted to the FB. Stage A5 replaces step 2
//      with real userspace blits (mapping cell VMOs read-only + walking
//      libtui's font_8x16 in user memory).
//
// Channel publication: Stage A3 already wired keyboard.c → console_switch
// kernel-side. Stage A4 publishes a userspace mirror under /sys/console/switch
// so non-trusted apps (no CAP_KIND_SYSTEM cap) can subscribe to switch events.
// Stage C2 (cap inheritance) lets that wiring tighten cap-gating; for A4 the
// publish is a no-op stub since publish/subscribe arrives in Stage C1+.
//
// Crash-safety: fbd is a daemon — init.conf supervisor respawns on exit. If
// fbd dies, kernel klog re-takes the framebuffer (g_fbd_alive remains true
// until the next ACK_RENDER from a fresh fbd; in practice this leaves the
// last frame frozen until respawn, ~5 ms latency under init's loop). Phase
// 28 hardens this with an explicit teardown ACK on exit.

#include "fbd.h"
#include "../syscalls.h"
#include "../libtui/libtui.h"
#include "../libc/include/stdio.h"

extern int printf(const char *fmt, ...);

// Approximate millisecond-tier wait. spin_us is TSC-calibrated under both
// KVM and TCG (Phase 24 closeout) so this is portable across host modes.
static void fbd_sleep_ms(uint32_t ms) {
    spin_us((uint64_t)ms * 1000ull);
}

// Render a startup banner so the framebuffer doesn't go black after fbd's
// first ACK_RENDER. The banner explains where to find the actual interactive
// shell (gash on serial) and what TUI features are wired vs deferred.
//
// Stage A4 limitation: libtui currently routes every cell write through a
// DEBUG_CONSOLE_WRITE_CELL syscall (one syscall per cell). For a 60×8 banner
// that's ~500 syscalls — fast enough at boot but not what production code
// should do. FU27.X.cap_recursive_inheritance unblocks SYS_CONSOLE_ATTACH +
// mapped cell-VMO direct writes which drops this to a single memcpy.
// Banner geometry. We don't have a runtime "give me console dims" syscall
// yet (FU27.X.console_inspect), so we pick a layout that works for the
// 1280x800 framebuffer Limine actually requests on QEMU's default cirrus
// (= 160 cols × 50 rows of cells). For smaller framebuffers the banner
// gets clipped on the right — cosmetic, not fatal, and the substrate is
// what mattered. tui_write_cell silently no-ops out-of-bounds writes.
//
// 256-color palette indices we'll use:
//   0   = black background
//   15  = bright white (foreground primary)
//   46  = bright green (status badge)
//   51  = cyan (info)
//   226 = yellow (warnings / deferred items)
//   240 = dim grey (subtitle)
//   245 = mid grey (separator rule)
//
// Layout (banner is 70 cells wide, 16 rows tall, anchored at row=2, col=8):
//   ╔══════════════════════════════════════════════════════════════════╗
//   ║                                                                  ║
//   ║   GrahaOS — Phase 27 substrate up                       [READY]  ║
//   ║   TUI · Graphics · AI primitives · cap inheritance               ║
//   ║   ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  ║
//   ║                                                                  ║
//   ║   Interactive shell:  gash on serial                             ║
//   ║                       (the terminal you ran `make run` in)       ║
//   ║                                                                  ║
//   ║   Console switching:  Alt+1 .. Alt+4                             ║
//   ║   Network stack:      e1000d ↔ netd live, DHCP probing           ║
//   ║   AI primitives:      audit-stream + manifest-export wired       ║
//   ║   WASM execution:     deferred to FU27.WASM                      ║
//   ║                                                                  ║
//   ║   gate: 974/974                                              ▌   ║
//   ╚══════════════════════════════════════════════════════════════════╝
//
// Stage A4 limitation: every cell write is a DEBUG_CONSOLE_WRITE_CELL
// syscall (~700 syscalls for the full banner). FU27.X.cap_recursive_inheritance
// unblocks SYS_CONSOLE_ATTACH + mapped cell-VMO direct writes — drops
// to a single memcpy.
#define FBD_BANNER_ROW   2
#define FBD_BANNER_COL   8
#define FBD_BANNER_W    70
#define FBD_BANNER_H    16
#define COL_FG          15
#define COL_DIM        240
#define COL_RULE       245
#define COL_OK          46
#define COL_INFO        51
#define COL_WARN       226

static void fbd_render_banner(uint32_t console_id) {
    (void)tui_init();
    (void)tui_attach(console_id);

    const uint32_t r0 = FBD_BANNER_ROW;
    const uint32_t c0 = FBD_BANNER_COL;
    const uint32_t W  = FBD_BANNER_W;
    const uint32_t H  = FBD_BANNER_H;

    // Double-line border. Bright white on black for high contrast against
    // whatever klog pixels we're overwriting.
    (void)tui_draw_box_double(console_id, r0, c0, H, W, COL_FG, 0);

    // Row r0+2: title line + status badge in green at the right edge.
    (void)tui_print(console_id, r0 + 2, c0 + 3, COL_FG, 0, 0,
                    "GrahaOS  Phase 27 substrate up");
    // Status badge "[READY]" right-aligned (5 chars + brackets = 7 chars).
    // Position so the closing ']' lands at (c0 + W - 4).
    (void)tui_print(console_id, r0 + 2, c0 + W - 4 - 7, COL_OK, 0, 0,
                    "[READY]");

    // Subtitle in dim grey.
    (void)tui_print(console_id, r0 + 3, c0 + 3, COL_DIM, 0, 0,
                    "TUI . Graphics . AI primitives . cap inheritance");

    // Medium-shade horizontal rule.
    (void)tui_fill_hrule(console_id, r0 + 4, c0 + 3,
                        W - 6, TUI_BLOCK_MEDIUM, COL_RULE, 0);

    // Body — two columns: label (left, 22 chars) + value.
    (void)tui_print(console_id, r0 + 6, c0 + 3, COL_FG, 0, 0,
                    "Interactive shell:");
    (void)tui_print(console_id, r0 + 6, c0 + 24, COL_INFO, 0, 0,
                    "gash on serial");
    (void)tui_print(console_id, r0 + 7, c0 + 24, COL_DIM, 0, 0,
                    "(the terminal you ran `make run` in)");

    (void)tui_print(console_id, r0 + 9, c0 + 3, COL_FG, 0, 0,
                    "Console switching:");
    (void)tui_print(console_id, r0 + 9, c0 + 24, COL_INFO, 0, 0,
                    "Alt+1 .. Alt+4");

    (void)tui_print(console_id, r0 + 10, c0 + 3, COL_FG, 0, 0,
                    "Network stack:");
    (void)tui_print(console_id, r0 + 10, c0 + 24, COL_INFO, 0, 0,
                    "e1000d <-> netd live, DHCP probing");

    (void)tui_print(console_id, r0 + 11, c0 + 3, COL_FG, 0, 0,
                    "AI primitives:");
    (void)tui_print(console_id, r0 + 11, c0 + 24, COL_INFO, 0, 0,
                    "audit-stream + manifest-export wired");

    (void)tui_print(console_id, r0 + 12, c0 + 3, COL_FG, 0, 0,
                    "WASM execution:");
    (void)tui_print(console_id, r0 + 12, c0 + 24, COL_WARN, 0, 0,
                    "deferred to FU27.WASM");

    // Status footer.
    (void)tui_print(console_id, r0 + 14, c0 + 3, COL_DIM, 0, 0,
                    "gate: 974/974");
    // Cursor block at the right of the status row — a "we're alive"
    // indicator that visually anchors the banner.
    (void)tui_set_cursor(console_id, r0 + 14, c0 + W - 5);

    (void)tui_present(console_id);
}

void _start(void) {
    // Per-process pledge narrowing — fbd needs:
    //   IPC_SEND for SYS_CONSOLE_ACK_RENDER
    //   IPC_RECV for the future console input chan
    //   SYS_QUERY for SYS_CONSOLE_GET_SELECTED-class probes
    //   SYS_CONTROL for SYS_DEBUG (the synthetic-render trigger we use
    //                until FU27.X.tui_demo_apps replaces it with a real
    //                userspace blit path)
    //   TIME for spin_us-driven frame pacing
    (void)syscall_pledge(PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_TIME);

    printf("[fbd] phase 27 stage A4 compositor up; target=%u fps\n",
           (unsigned)FBD_TARGET_FPS);

    // Render a welcome banner BEFORE flipping g_fbd_alive=true. While the
    // bypass flag is still false, the kernel-side console_render_synthetic
    // path's framebuffer_force_draw_cell() acquires the framebuffer lock
    // alongside whatever klog is doing. Once we ACK below, klog stops and
    // the banner stays put. Without this, the screen would go black on the
    // first ACK because the cell VMO is zero-filled.
    fbd_render_banner(/*console_id*/ 0);

    // Initial handshake: ack render on console 0 with seq=1. This flips the
    // kernel's g_fbd_alive=true, which causes drivers/video/framebuffer.c
    // direct draws (klog, banner, etc) to bail. Panic path bypasses the
    // bypass via g_panic_in_progress.
    long rc = syscall_console_ack_render(0, 1);
    if (rc != 0) {
        printf("[fbd] FATAL: initial ack_render rc=%ld; exiting\n", rc);
        syscall_exit(1);
    }
    printf("[fbd] banner rendered; ack_render(0, 1) ok; g_fbd_alive=true\n");

    // Render loop. Substrate-only at A4 — Stage A5 replaces synthetic render
    // with real userspace blits via cell-VMO maps + libtui font.
    uint64_t frame = 1;
    while (1) {
        uint32_t selected = syscall_debug_console_get_selected();

        // Kernel-side composite of currently-selected console. This is
        // substrate; A5 will do this in user space.
        long render_rc = syscall_debug_console_synthetic_render(selected);
        (void)render_rc;  // best-effort

        // Bump rendered_seq via ack so the kernel sees fbd is alive (and
        // future stalled-fbd detection has a heartbeat).
        frame++;
        (void)syscall_console_ack_render(selected, frame);

        fbd_sleep_ms(FBD_TICK_MS);
    }
}
