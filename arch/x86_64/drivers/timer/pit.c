#include "pit.h"
#include "../../cpu/ports.h"

// PIT I/O Ports
#define PIT_DATA_PORT_0 0x40
#define PIT_DATA_PORT_1 0x41
#define PIT_DATA_PORT_2 0x42
#define PIT_COMMAND_PORT 0x43

// The PIT's base frequency is ~1.193182 MHz
#define PIT_BASE_FREQUENCY 1193182

void pit_init(uint32_t frequency) {
    // Calculate the divisor needed to get the desired frequency.
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    // Command byte to set the PIT to square wave mode (mode 3)
    // and to send the divisor in two bytes (low byte, then high byte).
    // Channel 0, Access mode: lo/hi byte, Mode 3, Binary mode.
    uint8_t command = 0x36;
    outb(PIT_COMMAND_PORT, command);

    // Send the divisor value, low byte first, then high byte.
    uint8_t low_byte = (uint8_t)(divisor & 0xFF);
    uint8_t high_byte = (uint8_t)((divisor >> 8) & 0xFF);

    outb(PIT_DATA_PORT_0, low_byte);
    outb(PIT_DATA_PORT_0, high_byte);
}
