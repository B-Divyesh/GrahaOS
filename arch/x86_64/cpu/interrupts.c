#include "interrupts.h"
#include "ports.h"
#include "sched/sched.h" // For calling the scheduler
#include "../../../drivers/video/framebuffer.h"

// PIC (Programmable Interrupt Controller) ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

// End-of-Interrupt command
#define PIC_EOI      0x20

// Remaps the PIC interrupts to avoid conflicts with CPU exceptions.
// By default, IRQs 0-7 map to interrupts 8-15, which conflicts with #DF, #TS, etc.
// We will remap them to interrupts 32-47.
static void pic_remap(void) {
    // Save masks
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    // Start initialization sequence (in cascade mode)
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);

    // Set vector offsets (PIC1 starts at 32, PIC2 at 40)
    outb(PIC1_DATA, 32);
    outb(PIC2_DATA, 40);

    // Tell PICs about their cascade relationship
    outb(PIC1_DATA, 4); // Tell Master PIC there is a slave PIC at IRQ2 (0100)
    outb(PIC2_DATA, 2); // Tell Slave PIC its cascade identity (0010)

    // Set 8086/88 (MCS-80/85) mode
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    // Restore saved masks
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

// Called from idt_init()
void irq_init(void) {
    pic_remap();
    // Enable only the timer (IRQ0) and keyboard (IRQ1) for now
    outb(PIC1_DATA, 0b11111100); // Unmask IRQ0 and IRQ1
    outb(PIC2_DATA, 0b11111111); // Mask all slave PIC IRQs
    asm volatile ("sti"); // Enable interrupts
}

// Simple hcf function
static void hcf(void) {
    asm ("cli");  // Disable interrupts first
    for (;;) {
        asm ("hlt");
    }
}

// Helper to print a hex value at a specific screen location
static void print_hex_at(uint64_t value, int x, int y) {
    char buffer[17] = {0};
    const char *hex_chars = "0123456789ABCDEF";
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 15; i >= 2; i--) {
        buffer[i] = hex_chars[value & 0xF];
        value >>= 4;
    }
    framebuffer_draw_string(buffer, x, y, COLOR_WHITE, COLOR_RED);
}

static void print_hex_value(const char *label, uint64_t value, int x, int y) {
    framebuffer_draw_string(label, x, y, COLOR_WHITE, COLOR_RED);
    
    // Simple hex conversion
    char hex[17] = "0x";
    const char *digits = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        hex[17 - i] = digits[(value >> (i * 4)) & 0xF];
    }
    hex[16] = '\0';
    
    framebuffer_draw_string(hex, x + 80, y, COLOR_YELLOW, COLOR_RED);
}


// The main C-level interrupt handler, with corrected page fault diagnostics
void interrupt_handler(struct interrupt_frame *frame) {
    if (frame->int_no == 14) { // Page Fault
        uint64_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr)); // Get faulting address from CR2

        // Create a red banner for the error message
        framebuffer_draw_rect(0, 0, framebuffer_get_width(), 120, COLOR_RED);
        framebuffer_draw_string("CPU Exception: 0E (Page Fault)", 10, 10, COLOR_WHITE, COLOR_RED);
        
        // Print faulting address and instruction pointer on separate lines
        framebuffer_draw_string("Faulting Address:", 10, 30, COLOR_WHITE, COLOR_RED);
        print_hex_at(fault_addr, 180, 30);

        framebuffer_draw_string("Instruction Ptr:", 10, 50, COLOR_WHITE, COLOR_RED);
        print_hex_at(frame->rip, 180, 50);

        framebuffer_draw_string("Error Code:", 10, 70, COLOR_WHITE, COLOR_RED);
        print_hex_at(frame->err_code, 180, 70);
        
        // Decode error code
        if (frame->err_code & 4) {
            framebuffer_draw_string("USER MODE fault", 10, 90, COLOR_WHITE, COLOR_RED);
        } else {
            framebuffer_draw_string("KERNEL MODE fault", 10, 90, COLOR_WHITE, COLOR_RED);
        }
        if (frame->cs & 3) {  // User mode exception
            framebuffer_draw_string("=== USER CRASH DUMP ===", 500, 300, COLOR_YELLOW, COLOR_RED);
            print_hex_value("RAX:", frame->rax, 500, 320);
            print_hex_value("RCX:", frame->rcx, 500, 340);
            print_hex_value("RDX:", frame->rdx, 500, 360);
            print_hex_value("RSI:", frame->rsi, 500, 380);
            print_hex_value("RDI:", frame->rdi, 500, 400);
            print_hex_value("RBP:", frame->rbp, 500, 420);
            print_hex_value("RSP:", frame->rsp, 500, 440);
            print_hex_value("RIP:", frame->rip, 500, 460);
            print_hex_value("R11:", frame->r11, 500, 480);
        }

        hcf();
    }

    if (frame->int_no < 32) {
        // Is a CPU exception
        char msg[] = "CPU Exception: 00 at RIP=0x00000000";
        char hex[] = "0123456789ABCDEF";
        msg[15] = hex[(frame->int_no >> 4) & 0xF];
        msg[16] = hex[frame->int_no & 0xF];
        
        // Add RIP to the error message
        uint64_t rip = frame->rip;
        for (int i = 0; i < 8; i++) {
            uint8_t nibble = (rip >> (28 - i * 4)) & 0xF;
            msg[26 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        
        framebuffer_draw_rect(0, 0, framebuffer_get_width(), 90, COLOR_RED);
        framebuffer_draw_string(msg, 10, 10, COLOR_WHITE, COLOR_RED);
        
        // Show what type of exception this is
        if (frame->int_no == 6) {
            framebuffer_draw_string("Invalid Opcode (#UD) - syscall not supported?", 10, 30, COLOR_WHITE, COLOR_RED);
        } else if (frame->int_no == 13) {
            framebuffer_draw_string("General Protection Fault (#GP)", 10, 30, COLOR_WHITE, COLOR_RED);
        } else {
            framebuffer_draw_string("User process crashed!", 10, 30, COLOR_WHITE, COLOR_RED);
        }
        
        // Show if it's user or kernel mode
        if (frame->cs & 3) {
            framebuffer_draw_string("USER MODE exception", 10, 50, COLOR_WHITE, COLOR_RED);
        } else {
            framebuffer_draw_string("KERNEL MODE exception", 10, 50, COLOR_WHITE, COLOR_RED);
        }
        
        framebuffer_draw_string("Error code:", 10, 70, COLOR_WHITE, COLOR_RED);
        print_hex_at(frame->err_code, 120, 70);

        if (frame->cs & 3) {  // User mode exception
            framebuffer_draw_string("=== USER CRASH DUMP ===", 500, 300, COLOR_YELLOW, COLOR_RED);
            print_hex_value("RAX:", frame->rax, 500, 320);
            print_hex_value("RCX:", frame->rcx, 500, 340);
            print_hex_value("RDX:", frame->rdx, 500, 360);
            print_hex_value("RSI:", frame->rsi, 500, 380);
            print_hex_value("RDI:", frame->rdi, 500, 400);
            print_hex_value("RBP:", frame->rbp, 500, 420);
            print_hex_value("RSP:", frame->rsp, 500, 440);
            print_hex_value("RIP:", frame->rip, 500, 460);
            print_hex_value("R11:", frame->r11, 500, 480);
        }
        
        hcf(); // Halt on any other exception
    } else if (frame->int_no >= 32 && frame->int_no < 48) {
        // Is a hardware IRQ
        if (frame->int_no == 32) { // IRQ0: Timer
            schedule(frame); // Call the scheduler
        }

        // Send EOI to the PICs.
        if (frame->int_no >= 40) {
            outb(PIC2_COMMAND, PIC_EOI); // EOI to slave
        }
        outb(PIC1_COMMAND, PIC_EOI); // EOI to master
    }
}