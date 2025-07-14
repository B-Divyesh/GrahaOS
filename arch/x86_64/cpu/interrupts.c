#include "interrupts.h"
#include "ports.h"
#include "sched/sched.h" // For calling the scheduler
#include "../../../drivers/video/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h" // ADDED

// PIC (Programmable Interrupt Controller) ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

// End-of-Interrupt command
#define PIC_EOI      0x20

// Remaps the PIC interrupts to avoid conflicts with CPU exceptions.
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
    keyboard_init(); // ADDED: Initialize keyboard driver
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
    // CRITICAL: Validate frame pointer
    if (!frame) {
        // This should NEVER happen - indicates severe corruption
        asm volatile(
            "cli\n"
            "mov $0xDEADBEEF, %%rax\n"  // Magic value for debugging
            "hlt\n"
            ::: "rax"
        );
    }
    
    // Validate frame address is in reasonable range
    uint64_t frame_addr = (uint64_t)frame;
    if (frame_addr < 0xFFFF800000000000) {
        // Frame is not in kernel space - corruption!
        asm volatile("cli; hlt");
    }
    
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
        // CPU exception - show detailed info
        framebuffer_draw_rect(0, 0, framebuffer_get_width(), 200, COLOR_RED);
        
        char msg[64] = "CPU Exception #";
        char num[3];
        int n = frame->int_no;
        int i = 0;
        do {
            num[i++] = '0' + (n % 10);
            n /= 10;
        } while (n > 0);
        while (i > 0) {
            msg[15 + (2-i)] = num[--i];
        }
        msg[17] = '\0';
        framebuffer_draw_string(msg, 10, 10, COLOR_WHITE, COLOR_RED);
        
        // Show exception name during hte exception
        const char *exception_names[] = {
            "Divide by Zero", "Debug", "NMI", "Breakpoint",
            "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
            "Double Fault", "Coprocessor Segment", "Invalid TSS", "Segment Not Present",
            "Stack Fault", "General Protection", "Page Fault", "Reserved",
            "x87 FP", "Alignment Check", "Machine Check", "SIMD FP"
        };
        
        if (frame->int_no < 20) {
            framebuffer_draw_string(exception_names[frame->int_no], 10, 30, COLOR_YELLOW, COLOR_RED);
        }
        
        // Show RIP where it crashed
        char rip_msg[32] = "RIP: 0x";
        const char *hex = "0123456789ABCDEF";
        uint64_t rip = frame->rip;
        for (int j = 0; j < 16; j++) {
            rip_msg[7 + j] = hex[(rip >> (60 - j * 4)) & 0xF];
        }
        rip_msg[23] = '\0';
        framebuffer_draw_string(rip_msg, 10, 50, COLOR_WHITE, COLOR_RED);
        
        // Show if it's user or kernel mode
        if (frame->cs & 3) {
            framebuffer_draw_string("USER MODE crash", 10, 70, COLOR_YELLOW, COLOR_RED);
            framebuffer_draw_string("Process was: grahai", 10, 90, COLOR_WHITE, COLOR_RED);
        } else {
            framebuffer_draw_string("KERNEL MODE crash", 10, 70, COLOR_YELLOW, COLOR_RED);
        }
        
        hcf();
    } else if (frame->int_no >= 32 && frame->int_no < 48) {
        // Is a hardware IRQ
        switch (frame->int_no) {
            case 32: // IRQ0: Timer
                schedule(frame);
                break;
            case 33: // IRQ1: Keyboard - ADDED
                keyboard_irq_handler();
                break;
        }

        // Send EOI to the PICs.
        if (frame->int_no >= 40) {
            outb(PIC2_COMMAND, PIC_EOI); // EOI to slave
        }
        outb(PIC1_COMMAND, PIC_EOI); // EOI to master
    }
}