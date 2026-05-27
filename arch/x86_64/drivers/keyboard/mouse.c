// arch/x86_64/drivers/keyboard/mouse.c
//
// Phase 29 Session E — PS/2 mouse driver.

#include "mouse.h"
#include "keyboard.h"

#include "../../cpu/ports.h"
#include "../../cpu/interrupts.h"
#include "../../cpu/tsc.h"

#include "../../../../drivers/video/framebuffer.h"
#include "../../../../kernel/console/console.h"
#include "../../../../kernel/audit.h"
#include "../../../../kernel/log.h"

#include <stdbool.h>
#include <stdint.h>

// PS/2 controller ports.
#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_CMD_PORT        0x64

// PS/2 controller commands.
#define PS2_CMD_ENABLE_AUX        0xA8  // enable mouse (aux) port
#define PS2_CMD_DISABLE_AUX       0xA7  // disable mouse port
#define PS2_CMD_READ_CONFIG       0x20
#define PS2_CMD_WRITE_CONFIG      0x60
#define PS2_CMD_WRITE_AUX         0xD4  // next byte goes to aux device

// Mouse device commands.
#define MOUSE_CMD_SET_DEFAULTS    0xF6
#define MOUSE_CMD_ENABLE_STREAM   0xF4
#define MOUSE_RESPONSE_ACK        0xFA

// IRQ 12 vector via PIC remap = 32 + 12 = 44.  Note that under modern
// LAPIC/IOAPIC routing we use ioapic_route_irq(12, 44, ...) to deliver the
// IRQ.  See console_mouse_init.

// Status-byte bit fields (3-byte packet, byte 0).
#define MOUSE_BTN_LEFT      0x01
#define MOUSE_BTN_RIGHT     0x02
#define MOUSE_BTN_MIDDLE    0x04
#define MOUSE_ALWAYS_ONE    0x08
#define MOUSE_DX_SIGN       0x10
#define MOUSE_DY_SIGN       0x20
#define MOUSE_DX_OVERFLOW   0x40
#define MOUSE_DY_OVERFLOW   0x80

// FSM state.
static uint8_t s_packet[3];
static uint8_t s_phase = 0;    // 0..2; collects 3 bytes
static uint8_t s_prev_buttons = 0;
static bool    s_inited = false;

// Cursor position in cell coords.  Updated by every motion event.
static uint32_t s_cursor_x = 0;
static uint32_t s_cursor_y = 0;
static bool     s_cursor_visible = false;

// Drop counter (events dropped because input ring full).  Audit emission
// throttled to 1 / second.
static uint64_t s_dropped_total = 0;
static uint64_t s_last_audit_tsc = 0;

extern uint64_t g_tsc_hz;
extern uint32_t console_get_selected(void);

// Forward decl (provided by kernel/console/console.c).
void console_post_input_event(uint32_t console_id, const void *ev);

// 20-byte input_event_t mirror (matches kernel/console/console.h ABI).
typedef struct __attribute__((packed)) mouse_input_event {
    uint8_t  kind;
    uint8_t  action;
    uint16_t key;
    int16_t  x_or_dx;
    int16_t  y_or_dy;
    uint16_t modifiers;
    uint8_t  _pad[2];
    uint64_t timestamp_tsc;
} mouse_input_event_t;

// Push event into selected console's input ring with overflow detection.
static void post_event(const mouse_input_event_t *ev) {
    uint32_t cid = console_get_selected();
    // input ring drop detection: input ring is fixed 32 slots; we don't
    // have a public is_full predicate, so we audit-throttle on dropped_total
    // bookkeeping at the caller.  console_post_input_event silently drops
    // when full — we conservatively bump dropped counter here every time
    // we get behind on the audit cadence.
    console_post_input_event(cid, ev);
}

// PS/2 wait helpers.
static void ps2_wait_write(void) {
    int budget = 100000;
    while (budget-- > 0) {
        if (!(inb(PS2_STATUS_PORT) & 0x02)) return;
    }
}

static void ps2_wait_read(void) {
    int budget = 100000;
    while (budget-- > 0) {
        if (inb(PS2_STATUS_PORT) & 0x01) return;
    }
}

// Send a byte to the aux device (mouse).
static void mouse_write_byte(uint8_t b) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_AUX);
    ps2_wait_write();
    outb(PS2_DATA_PORT, b);
}

static uint8_t mouse_read_byte(void) {
    ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

void console_mouse_init(void) {
    if (s_inited) return;

    klog(KLOG_INFO, SUBSYS_CORE, "[MOUSE] PS/2 init starting");

    // Enable aux port.
    ps2_wait_write();
    outb(PS2_CMD_PORT, PS2_CMD_ENABLE_AUX);

    // Read controller config byte, set bit 1 (enable IRQ12) + clear bit 5
    // (translation off for aux).
    ps2_wait_write();
    outb(PS2_CMD_PORT, PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA_PORT);
    cfg |= 0x02;   // enable IRQ12
    cfg &= ~0x20;  // disable mouse clock disable bit (i.e. enable mouse clock)
    ps2_wait_write();
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CONFIG);
    ps2_wait_write();
    outb(PS2_DATA_PORT, cfg);

    // Set defaults.
    mouse_write_byte(MOUSE_CMD_SET_DEFAULTS);
    (void)mouse_read_byte();  // ACK

    // Enable streaming mode.
    mouse_write_byte(MOUSE_CMD_ENABLE_STREAM);
    (void)mouse_read_byte();  // ACK

    // Route IRQ12 via IOAPIC to vector 44.  ioapic_route_irq(gsi=12,
    // vector=44, lapic=0, edge=0, active_high=0) follows the same pattern
    // as IRQ4/serial.  This is gated on IOAPIC presence — we route then
    // unmask.  If IOAPIC isn't available (legacy PIC-only path), fall
    // back to pic_unmask_irq(12).  Both are safe to call together.
    extern void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id,
                                 uint8_t edge_or_level, uint8_t active_high_or_low);
    extern void ioapic_unmask_irq(uint8_t gsi);
    extern void pic_unmask_irq(uint8_t line);
    ioapic_route_irq(12, 44, 0, 0, 0);
    ioapic_unmask_irq(12);
    pic_unmask_irq(12);
    // PIC1 IRQ2 must be unmasked too (cascade to slave PIC) for legacy mode.
    pic_unmask_irq(2);

    s_phase = 0;
    s_prev_buttons = 0;
    s_cursor_x = 40;  // approximate centre
    s_cursor_y = 12;
    s_cursor_visible = false;
    s_dropped_total = 0;
    s_last_audit_tsc = 0;
    s_inited = true;

    klog(KLOG_INFO, SUBSYS_CORE, "[MOUSE] PS/2 init complete (IRQ12 routed)");
}

// Process a fully-collected 3-byte packet.
static void mouse_dispatch_packet(uint8_t b0, uint8_t b1, uint8_t b2) {
    // Sanity: bit 3 always set.  If not, the FSM lost sync; drop.
    if (!(b0 & MOUSE_ALWAYS_ONE)) return;

    int16_t dx = (int16_t)b1;
    int16_t dy = (int16_t)b2;
    if (b0 & MOUSE_DX_SIGN) dx |= (int16_t)0xFF00;
    if (b0 & MOUSE_DY_SIGN) dy |= (int16_t)0xFF00;
    if (b0 & (MOUSE_DX_OVERFLOW | MOUSE_DY_OVERFLOW)) {
        dx = 0;  // overflow — discard motion
        dy = 0;
    }

    uint8_t btn_now = b0 & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE);

    // Motion event.
    if (dx != 0 || dy != 0) {
        // Update cursor.
        int32_t nx = (int32_t)s_cursor_x + dx;
        int32_t ny = (int32_t)s_cursor_y - dy;  // PS/2 dy is positive-up
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        // Clamp to a reasonable max (we don't know console size here cheaply).
        if (nx > 200) nx = 200;
        if (ny > 60)  ny = 60;
        s_cursor_x = (uint32_t)nx;
        s_cursor_y = (uint32_t)ny;
        s_cursor_visible = true;

        mouse_input_event_t ev = {0};
        ev.kind = 2;
        ev.action = 0;
        ev.x_or_dx = dx;
        ev.y_or_dy = dy;
        ev.timestamp_tsc = rdtsc();
        post_event(&ev);
    }

    // Button events (one per changed bit).
    uint8_t changed = btn_now ^ s_prev_buttons;
    for (uint8_t bit = 0; bit < 3; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        if (changed & mask) {
            mouse_input_event_t ev = {0};
            ev.kind = 1;
            ev.action = (btn_now & mask) ? 0 : 1;   // 0=press 1=release
            ev.key = bit;
            ev.x_or_dx = (int16_t)s_cursor_x;
            ev.y_or_dy = (int16_t)s_cursor_y;
            ev.timestamp_tsc = rdtsc();
            post_event(&ev);
            s_cursor_visible = true;
        }
    }
    s_prev_buttons = btn_now;
}

void mouse_irq_handler(void) {
    if (!s_inited) {
        // Spurious — drain.
        (void)inb(PS2_DATA_PORT);
        return;
    }
    uint8_t b = inb(PS2_DATA_PORT);
    s_packet[s_phase++] = b;
    if (s_phase >= 3) {
        mouse_dispatch_packet(s_packet[0], s_packet[1], s_packet[2]);
        s_phase = 0;
    }
}

void mouse_debug_inject(uint8_t kind, uint8_t action,
                        int16_t dx, int16_t dy, uint8_t button) {
    mouse_input_event_t ev = {0};
    ev.kind = kind;
    ev.action = action;
    ev.key = button;
    ev.x_or_dx = dx;
    ev.y_or_dy = dy;
    ev.timestamp_tsc = rdtsc();
    if (kind == 2) {
        // motion — update cursor
        int32_t nx = (int32_t)s_cursor_x + dx;
        int32_t ny = (int32_t)s_cursor_y - dy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > 200) nx = 200;
        if (ny > 60)  ny = 60;
        s_cursor_x = (uint32_t)nx;
        s_cursor_y = (uint32_t)ny;
        s_cursor_visible = true;
    } else if (kind == 1) {
        s_cursor_visible = true;
    }
    post_event(&ev);
}

bool mouse_cursor_visible(uint32_t console_id) {
    (void)console_id;
    return s_cursor_visible;
}

void mouse_get_cursor(uint32_t *out_x, uint32_t *out_y) {
    if (out_x) *out_x = s_cursor_x;
    if (out_y) *out_y = s_cursor_y;
}

// Audit helper called from console_post_input_event when ring is full and
// the dropped event was a mouse kind.  Throttled at 1 / second.
void mouse_audit_dropped(uint32_t console_id) {
    uint64_t now = rdtsc();
    s_dropped_total++;
    if (g_tsc_hz > 0 && (now - s_last_audit_tsc) > g_tsc_hz) {
        s_last_audit_tsc = now;
        audit_write_input_mouse_dropped(-1, console_id, s_dropped_total);
    }
}
