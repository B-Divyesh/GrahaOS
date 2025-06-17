#include "interrupts.h"
#include "../../../drivers/video/framebuffer.h"

// Simple interrupt handler for demonstration
void interrupt_handler(struct interrupt_frame *frame) {
    // Create a message showing the interrupt number in hex
    char msg[] = "Interrupt: 0x00";
    char hex[] = "0123456789ABCDEF";

    // Convert interrupt number to hex string
    msg[13] = hex[(frame->int_no >> 4) & 0xF];
    msg[14] = hex[frame->int_no & 0xF];

    // Display the interrupt message
    framebuffer_draw_string(msg, 10, 200, COLOR_RED, 0x00101828);

    // For now, halt the system after displaying the interrupt
    // In a real OS, you would handle the interrupt appropriately
    asm volatile ("cli");
    for (;;) {
        asm volatile ("hlt");
    }
}
