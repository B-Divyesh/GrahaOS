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

// US QWERTY Scancode Set 1 to ASCII mapping, using the DELL SK-8115, will add remaining key in the future
//we get some error about an extra array element here, will work on it later
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
    
    // Enable interrupt (bit 0) and disable translation (bit 6)
    // Disabling translation ensures we get raw scancodes
    config |= 0x01;   // Enable IRQ1
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
    
    // CRITICAL: Set scancode set 1
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
    
    framebuffer_draw_string("KB: Ready (Set 1)", 10, 280, COLOR_GREEN, 0x00101828);
}

void keyboard_irq_handler(void) {
    keyboard_interrupts_received++;
    
    // Read the scancode
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Debug: Show scancode (first 10 keys)
    static int key_count = 0;
    if (key_count < 10 && scancode < 0x80) {
        char msg[48] = "Key ";
        int i = 4;
        msg[i++] = '0' + key_count;
        msg[i++] = ':';
        msg[i++] = ' ';
        msg[i++] = '0';
        msg[i++] = 'x';
        const char *hex = "0123456789ABCDEF";
        msg[i++] = hex[(scancode >> 4) & 0xF];
        msg[i++] = hex[scancode & 0xF];
        
        // Show the mapped character
        if (scancode < 128 && scancode_set1_map[scancode] != 0) {
            msg[i++] = ' ';
            msg[i++] = '=';
            msg[i++] = ' ';
            msg[i++] = '\'';
            msg[i++] = scancode_set1_map[scancode];
            msg[i++] = '\'';
        }
        msg[i] = '\0';
        
        framebuffer_draw_string(msg, 10, 360 + (key_count * 16), COLOR_CYAN, 0x00101828);
        key_count++;
    }
    
    // Filter out special responses
    if (scancode >= 0xFA) {
        return; // ACK, RESEND, etc.
    }
    
    // Only process key presses (not releases)
    if (scancode < 0x80) {
        char ascii = scancode_set1_map[scancode];
        if (ascii != 0) {
            // Add to buffer
            size_t next_write = (write_index + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != read_index) {
                key_buffer[write_index] = ascii;
                write_index = next_write;
            }
        }
    }
}

char keyboard_getchar(void) {
    if (read_index == write_index) {
        return 0;
    }
    
    char c = key_buffer[read_index];
    read_index = (read_index + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}