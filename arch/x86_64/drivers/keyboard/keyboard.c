// arch/x86_64/drivers/keyboard/keyboard.c
#include "keyboard.h"
#include "../../cpu/ports.h"
#include "drivers/video/framebuffer.h"
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
}

void keyboard_handle_scancode(uint8_t scancode) {
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
    
    // Clear E0 prefix for normal keys
    e0_prefix = false;
    
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