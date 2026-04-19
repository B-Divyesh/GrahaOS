// arch/x86_64/drivers/keyboard/keyboard.c
#include "keyboard.h"
#include "../../cpu/ports.h"
#include "../../cpu/interrupts.h"
#include "drivers/video/framebuffer.h"
#include "../../../../kernel/cap/can.h"
#include "../../../../kernel/log.h"
#include <stdbool.h>
#include <stddef.h>

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_COMMAND_PORT 0x64
#define KEYBOARD_BUFFER_SIZE 256

// PS/2 Controller Commands
#define PS2_CMD_DISABLE_PORT1 0xAD
#define PS2_CMD_ENABLE_PORT1 0xAE
#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_TEST_PORT1 0xAB

// Keyboard Commands
#define KB_CMD_RESET 0xFF
#define KB_CMD_SET_SCANCODE 0xF0
#define KB_CMD_ENABLE 0xF4
#define KB_CMD_DISABLE 0xF5
#define KB_CMD_SET_DEFAULTS 0xF6

// Keyboard Responses
#define KB_RESPONSE_ACK 0xFA
#define KB_RESPONSE_RESEND 0xFE
#define KB_RESPONSE_TEST_PASSED 0xAA
#define KB_RESPONSE_ERROR 0xFC

// --- Globals ---
static char key_buffer[KEYBOARD_BUFFER_SIZE];
static size_t read_index = 0;
static size_t write_index = 0;
static volatile int keyboard_interrupts_received = 0;
static volatile bool expecting_scancode_set_response = false;

// US QWERTY Scancode Set 1 to ASCII mapping
static const char scancode_set1_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Shift key mapping for US QWERTY
static const char scancode_set1_shift_map[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Track shift state - FIX: separate left and right shift
static bool left_shift_pressed = false;
static bool right_shift_pressed = false;
static bool caps_lock = false;

// Phase 16: CAN-controlled activation flag. Drives the scancode guard and the
// PIC mask. True at boot because keyboard_init() calls cap_register with type
// CAP_DRIVER (default state OFF, then activated by boot sequence) — but
// because the existing keyboard IRQ path works before cap_activate runs, we
// default true and let deactivate flip it off on demand.
static bool g_keyboard_active = true;

// Wait for PS/2 controller ready
static void ps2_wait_write(void) {
    int timeout = 10000;
    while (timeout-- > 0) {
        if (!(inb(KEYBOARD_STATUS_PORT) & 0x02)) return;
    }
}

static void ps2_wait_read(void) {
    int timeout = 10000;
    while (timeout-- > 0) {
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) return;
    }
}

// Send command to PS/2 controller
static void ps2_send_command(uint8_t cmd) {
    ps2_wait_write();
    outb(KEYBOARD_COMMAND_PORT, cmd);
}

// Send data to keyboard
static void kb_send_data(uint8_t data) {
    ps2_wait_write();
    outb(KEYBOARD_DATA_PORT, data);
}

// Read data from keyboard with timeout
static uint8_t kb_read_data(void) {
    ps2_wait_read();
    return inb(KEYBOARD_DATA_PORT);
}

// Driver framework stats callback
static int keyboard_get_driver_stats(state_driver_stat_t *stats, int max) {
    if (!stats || max < 2) return 0;
    const char *k0 = "interrupts";
    for (int i = 0; k0[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[0].key[i] = k0[i];
    stats[0].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[0].value = (uint64_t)keyboard_interrupts_received;
    const char *k1 = "buf_used";
    for (int i = 0; k1[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[1].key[i] = k1[i];
    stats[1].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[1].value = (write_index >= read_index) ?
        (write_index - read_index) :
        (KEYBOARD_BUFFER_SIZE - read_index + write_index);
    return 2;
}

void keyboard_init(void) {
    // Clear the buffer
    read_index = 0;
    write_index = 0;
    keyboard_interrupts_received = 0;
    left_shift_pressed = false;
    right_shift_pressed = false;
    caps_lock = false;
    
    framebuffer_draw_string("KB: Initializing...", 10, 280, COLOR_YELLOW, 0x00101828);
    
    // Disable keyboard while configuring
    ps2_send_command(PS2_CMD_DISABLE_PORT1);
    
    // Flush output buffer
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_DATA_PORT);
    }
    
    // Read configuration byte
    ps2_send_command(PS2_CMD_READ_CONFIG);
    uint8_t config = kb_read_data();
    
    // For polling mode, we disable the interrupt (bit 0)
    // and disable translation (bit 6)
    config &= ~0x01;  // Disable IRQ1 for polling mode
    config &= ~0x40;  // Disable translation
    
    // Write configuration back
    ps2_send_command(PS2_CMD_WRITE_CONFIG);
    kb_send_data(config);
    
    // Enable keyboard port
    ps2_send_command(PS2_CMD_ENABLE_PORT1);
    
    // Reset keyboard
    kb_send_data(KB_CMD_RESET);
    uint8_t response = kb_read_data();
    if (response != KB_RESPONSE_ACK) {
        framebuffer_draw_string("KB: Reset no ACK", 10, 300, COLOR_RED, 0x00101828);
    }
    
    // Wait for self-test to pass
    response = kb_read_data();
    if (response != KB_RESPONSE_TEST_PASSED) {
        framebuffer_draw_string("KB: Self-test failed", 10, 320, COLOR_RED, 0x00101828);
    }
    
    // Set scancode set 1
    framebuffer_draw_string("KB: Setting scancode set 1...", 10, 340, COLOR_YELLOW, 0x00101828);
    
    kb_send_data(KB_CMD_SET_SCANCODE);
    response = kb_read_data();
    if (response == KB_RESPONSE_ACK) {
        // Send the scancode set number (1)
        kb_send_data(0x01);
        response = kb_read_data();
        if (response == KB_RESPONSE_ACK) {
            framebuffer_draw_string("KB: Scancode set 1 enabled", 10, 340, COLOR_GREEN, 0x00101828);
        } else {
            framebuffer_draw_string("KB: Set 1 failed!", 10, 340, COLOR_RED, 0x00101828);
        }
    }
    
    // Enable keyboard scanning
    kb_send_data(KB_CMD_ENABLE);
    response = kb_read_data();
    
    // Flush any remaining data
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_DATA_PORT);
    }
    
    framebuffer_draw_string("KB: Ready (Polling Mode)", 10, 280, COLOR_GREEN, 0x00101828);

    // Register with Capability Activation Network
    const char *kbd_deps[] = {"interrupt_controller"};
    cap_op_t kbd_ops[1];
    cap_op_set(&kbd_ops[0], "getchar", 0, 0);
    cap_register("keyboard_input", CAP_DRIVER, CAP_SUBTYPE_INPUT, -1, kbd_deps, 1,
                 keyboard_activate, keyboard_deactivate,
                 kbd_ops, 1, keyboard_get_driver_stats);
}

// Phase 16: CAN activate callback. Drains any stale scan codes the hardware
// queued while the driver was off, unmasks IRQ 1 on the PIC (defence in
// depth — LAPIC/PIC mode is orthogonal; if we ever re-enable PS/2 IRQ
// delivery in the config byte, this ensures the IRQ is routable), and flips
// g_keyboard_active back on so keyboard_handle_scancode accepts input. Called
// by cap_activate from the CAN dispatcher.
int keyboard_activate(void) {
    // Drain stale bytes from the 8042 output buffer before we re-arm — the
    // controller may have captured keystrokes while we were off.
    int drain_budget = 16;
    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) && drain_budget-- > 0) {
        (void)inb(KEYBOARD_DATA_PORT);
    }
    pic_unmask_irq(1);
    g_keyboard_active = true;
    klog(KLOG_INFO, SUBSYS_CORE, "[KB] activated (IRQ1 unmasked, buffer drained)");
    return 0;
}

// Phase 16: CAN deactivate callback. Masks IRQ 1 on the PIC and disables the
// scancode-processing guard. Returns 0 unconditionally — there is no case in
// which we refuse to deactivate a keyboard.
int keyboard_deactivate(void) {
    pic_mask_irq(1);
    g_keyboard_active = false;
    klog(KLOG_INFO, SUBSYS_CORE, "[KB] deactivated (IRQ1 masked, handler guarded)");
    return 0;
}

// Test hook: read the current flag from outside the driver. Used by the
// SYS_DEBUG subcommand that powers cantest_v2's G1 assertion.
bool keyboard_is_active(void) { return g_keyboard_active; }

void keyboard_handle_scancode(uint8_t scancode) {
    // Phase 16: refuse to process keys while the CAN cap is off. Defence in
    // depth: the PIC mask already blocks most events, but the handler is also
    // reachable from polling paths, so guard here too.
    if (!g_keyboard_active) return;

    // Filter out special responses
    if (scancode >= 0xFA) {
        return; // ACK, RESEND, etc.
    }
    
    // E0 prefix handling for extended scancodes (arrow keys, etc.)
    static bool e0_prefix = false;
    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }
    
    // Handle key releases (bit 7 set)
    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;
        
        // Check for shift release - FIX: handle separately
        if (key == 0x2A) {  // Left shift release
            left_shift_pressed = false;
        } else if (key == 0x36) {  // Right shift release
            right_shift_pressed = false;
        }
        
        e0_prefix = false; // Clear E0 prefix on any key release
        return;
    }
    
    // Handle special keys (key presses)
    if (scancode == 0x2A) {  // Left shift press
        left_shift_pressed = true;
        e0_prefix = false;
        return;
    } else if (scancode == 0x36) {  // Right shift press
        right_shift_pressed = true;
        e0_prefix = false;
        return;
    }
    
    if (scancode == 0x3A) {  // Caps lock
        caps_lock = !caps_lock;
        e0_prefix = false;
        return;
    }
    
    // Handle extended scancodes (arrow keys, etc.)
    if (e0_prefix) {
        e0_prefix = false;
        // Emit ANSI escape sequences for arrow keys
        // Up=0x48, Down=0x50, Left=0x4B, Right=0x4D
        const char *seq = ((void*)0);
        switch (scancode) {
            case 0x48: seq = "\033[A"; break; // Up
            case 0x50: seq = "\033[B"; break; // Down
            case 0x4B: seq = "\033[D"; break; // Left
            case 0x4D: seq = "\033[C"; break; // Right
        }
        if (seq) {
            for (int si = 0; seq[si]; si++) {
                size_t next_write = (write_index + 1) % KEYBOARD_BUFFER_SIZE;
                if (next_write != read_index) {
                    key_buffer[write_index] = seq[si];
                    write_index = next_write;
                }
            }
        }
        return;
    }

    // Convert scancode to ASCII
    char ascii = 0;

    if (scancode < 128) {
        // Check if ANY shift key is pressed
        bool shift_pressed = left_shift_pressed || right_shift_pressed;
        
        // Check if we should use shifted character
        bool use_shift = shift_pressed;
        
        // For letters, also consider caps lock
        char base_char = scancode_set1_map[scancode];
        if (base_char >= 'a' && base_char <= 'z') {
            use_shift = shift_pressed ^ caps_lock;  // XOR for caps lock effect
        }
        
        if (use_shift) {
            ascii = scancode_set1_shift_map[scancode];
        } else {
            ascii = scancode_set1_map[scancode];
        }
        
        // Store in buffer if we got a valid character
        if (ascii != 0) {
            size_t next_write = (write_index + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != read_index) {
                key_buffer[write_index] = ascii;
                write_index = next_write;
            }
        }
    }
}

void keyboard_irq_handler(void) {
    keyboard_interrupts_received++;
    
    // Read the scancode
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Process it through the common handler
    keyboard_handle_scancode(scancode);
}

char keyboard_getchar(void) {
    if (read_index == write_index) {
        return 0;
    }
    
    char c = key_buffer[read_index];
    read_index = (read_index + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

int keyboard_get_interrupt_count(void) {
    return keyboard_interrupts_received;
}