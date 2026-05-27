// arch/x86_64/drivers/keyboard/mouse.h
//
// Phase 29 Session E — PS/2 mouse driver.
//
// Standard 8042 controller auxiliary-port protocol.  The PS/2 keyboard
// controller multiplexes a primary (keyboard, IRQ 1) and secondary (mouse,
// IRQ 12) device.  Mouse packets are 3 bytes (we ignore the 4-byte intelli
// mouse extension for v1):
//   byte 0: status — left/right/middle button bits + Y/X overflow bits +
//           Y/X sign bits + always-1 bit 3
//   byte 1: dx (signed, sign carried in status bit 4)
//   byte 2: dy (signed, sign carried in status bit 5)
//
// We collect packets via an FSM (state 0..2) and push input_event_t records
// into the selected console's input ring as either kind=1 (button event) or
// kind=2 (motion event).  Drops are throttled and counted; the audit writer
// runs at most once per second so a stuck mouse doesn't flood audit.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Boot-time init.  Called from kernel/main.c after console_init.
void console_mouse_init(void);

// IRQ 12 handler.  Invoked from arch/x86_64/cpu/interrupts.c case 44.
void mouse_irq_handler(void);

// Test-only: inject a synthetic mouse event.  kind=1 button or kind=2 motion.
//   action: for button kind, 0=press / 1=release; for motion, ignored.
//   dx/dy: motion delta (signed 8-bit).
//   button: button bit (0=left,1=right,2=middle).
void mouse_debug_inject(uint8_t kind, uint8_t action,
                        int16_t dx, int16_t dy, uint8_t button);

// Test-only: is the cursor sprite currently visible?
bool mouse_cursor_visible(uint32_t console_id);

// Cursor position accessors.  console_render_synthetic_frame composites
// the cursor sprite at (cursor_x, cursor_y) cell coords if visible.
void mouse_get_cursor(uint32_t *out_x, uint32_t *out_y);
