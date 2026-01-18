// arch/x86_64/drivers/serial/serial.c
// Serial port driver for logging and debugging

#include "serial.h"
#include "../../cpu/ports.h"

#define PORT_COM1 0x3F8

static int serial_initialized = 0;

void serial_init(void) {
    // Disable interrupts
    outb(PORT_COM1 + 1, 0x00);

    // Enable DLAB (set baud rate divisor)
    outb(PORT_COM1 + 3, 0x80);

    // Set divisor to 3 (38400 baud)
    outb(PORT_COM1 + 0, 0x03);
    outb(PORT_COM1 + 1, 0x00);

    // 8 bits, no parity, one stop bit
    outb(PORT_COM1 + 3, 0x03);

    // Enable FIFO, clear, with 14-byte threshold
    outb(PORT_COM1 + 2, 0xC7);

    // Enable IRQs, RTS/DSR set
    outb(PORT_COM1 + 4, 0x0B);

    serial_initialized = 1;
}

static int is_transmit_empty(void) {
    return inb(PORT_COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    if (!serial_initialized) return;

    // Wait for transmit buffer to be empty
    while (!is_transmit_empty());

    outb(PORT_COM1, c);
}

void serial_write(const char *str) {
    if (!serial_initialized) return;

    while (*str) {
        serial_putc(*str++);
    }
}

void serial_write_hex(uint64_t value) {
    if (!serial_initialized) return;

    serial_write("0x");

    char hex[17];
    hex[16] = '\0';

    for (int i = 15; i >= 0; i--) {
        int digit = value & 0xF;
        hex[i] = digit < 10 ? '0' + digit : 'A' + (digit - 10);
        value >>= 4;
    }

    serial_write(hex);
}

void serial_write_dec(uint64_t value) {
    if (!serial_initialized) return;

    if (value == 0) {
        serial_putc('0');
        return;
    }

    char buf[21];
    int i = 20;
    buf[i] = '\0';

    while (value > 0 && i > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }

    serial_write(&buf[i]);
}
