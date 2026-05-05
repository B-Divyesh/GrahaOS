// user/fbd/fbd.h
//
// Phase 27 Block A (Stage A4) — fbd userspace framebuffer compositor.
//
// fbd owns the hardware framebuffer once console_set_fbd_alive(true) flips.
// It maps each console's cell-VMO read-only, composites the selected
// console's cells into the framebuffer, and acks the kernel via
// SYS_CONSOLE_ACK_RENDER. Subsequent stages extend it:
//   A5: real userspace rendering via libtui font_8x16 + cell-VMO maps.
//   B1: layered compositor (cells + sprite cells + RGBA overlay).
//   D1: handoff slot for wasmd_worker overlay rendering.
//
// Stage A4 ships a minimal scaffold that:
//   (1) acks an initial render to flip g_fbd_alive=true (kernel klog stops
//       drawing the framebuffer; serial mirrors stay).
//   (2) triggers kernel-side synthetic render (DEBUG_CONSOLE_SYNTHETIC_RENDER)
//       periodically for currently-selected console — this is substrate so
//       cells written by future libtui apps actually reach the screen under
//       autorun=init. Stage A5 replaces (2) with real userspace blits.
//
// fbd is NOT spawned under autorun=ktest; the gate test fbd_render.tap
// invokes the same synthetic render path directly via DEBUG syscalls. fbd
// IS spawned under autorun=init via etc/init.conf.
#pragma once

#include <stdint.h>

// Compositor target frame rate. 60 Hz is overkill for TUI; 16 FPS is enough
// to feel responsive without burning CPU. Stage A5 may bump or expose via
// /etc/fbd.conf.
#define FBD_TARGET_FPS    16
#define FBD_TICK_MS       (1000u / FBD_TARGET_FPS)

// Channel name fbd publishes for console-switch broadcast (Stage A3 wires
// the kernel-side producer; Stage A4 fbd is the consumer/republisher).
#define FBD_SWITCH_CHAN   "/sys/console/switch"
