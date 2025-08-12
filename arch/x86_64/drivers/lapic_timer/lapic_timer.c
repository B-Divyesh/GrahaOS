// arch/x86_64/drivers/lapic_timer/lapic_timer.c
#include "lapic_timer.h"
#include "../lapic/lapic.h"
#include "../../cpu/ports.h"
#include "../../../../drivers/video/framebuffer.h"

// LAPIC Timer registers (offsets from LAPIC base)
#define LAPIC_TIMER_LVT         0x320  // LVT Timer Register
#define LAPIC_TIMER_INITIAL     0x380  // Initial Count Register
#define LAPIC_TIMER_CURRENT     0x390  // Current Count Register
#define LAPIC_TIMER_DIVIDE      0x3E0  // Divide Configuration Register

// Timer modes
#define LAPIC_TIMER_PERIODIC    (1 << 17)
#define LAPIC_TIMER_MASKED      (1 << 16)

// Divide values for the timer
#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

// PIT ports for calibration
#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43

// Global calibration value (ticks per second)
static uint32_t lapic_timer_frequency = 0;
static bool timer_initialized = false;

// Helper to read LAPIC register
static uint32_t lapic_read_reg(uint32_t reg) {
    volatile uint32_t *lapic_base = lapic_get_base();
    if (!lapic_base) return 0;
    return lapic_base[reg / 4];
}

// Helper to write LAPIC register
static void lapic_write_reg(uint32_t reg, uint32_t value) {
    volatile uint32_t *lapic_base = lapic_get_base();
    if (!lapic_base) return;
    lapic_base[reg / 4] = value;
    // Read back to ensure write completion
    lapic_read_reg(LAPIC_REG_ID);
}

// Use PIT to wait for a specific number of milliseconds
static void pit_wait_ms(uint32_t ms) {
    // Configure PIT channel 0 for one-shot mode
    outb(PIT_COMMAND, 0x30);  // Channel 0, lobyte/hibyte, one-shot mode
    
    // PIT frequency is 1193182 Hz, so for 1ms we need ~1193 ticks
    uint16_t count = (1193 * ms);
    outb(PIT_CHANNEL0, count & 0xFF);
    outb(PIT_CHANNEL0, (count >> 8) & 0xFF);
    
    // Wait for the PIT to count down
    // We poll the PIT status to see when it's done
    uint8_t status;
    do {
        outb(PIT_COMMAND, 0xE2);  // Read-back command for channel 0
        status = inb(PIT_CHANNEL0);
    } while (status & 0x80);  // Bit 7 is the output pin state
}

uint32_t lapic_timer_calibrate(void) {
    if (!lapic_is_enabled()) {
        return 0;
    }
    
    // CRITICAL: Disable interrupts during calibration
    uint64_t flags;
    asm volatile(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
    );
    
    // Set the divider to 16
    lapic_write_reg(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    
    // Ensure timer is stopped first
    lapic_write_reg(LAPIC_TIMER_INITIAL, 0);
    
    // Start with the maximum initial count
    lapic_write_reg(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    
    // Use PIT for timing (more reliable than pit_wait_ms)
    // Configure PIT channel 0 for one-shot mode
    outb(0x43, 0x30);  // Channel 0, lobyte/hibyte, one-shot mode
    
    // For 10ms at 1193182 Hz, we need ~11932 ticks
    uint16_t pit_count = 11932;
    outb(0x40, pit_count & 0xFF);
    outb(0x40, (pit_count >> 8) & 0xFF);
    
    // Wait for PIT to count down
    uint8_t status;
    do {
        outb(0x43, 0xE2);  // Read-back command for channel 0
        status = inb(0x40);
    } while (!(status & 0x80));  // Wait until output goes high
    
    // Read how much the LAPIC timer counted
    uint32_t current = lapic_read_reg(LAPIC_TIMER_CURRENT);
    uint32_t ticks_in_10ms = 0xFFFFFFFF - current;
    
    // Stop the timer
    lapic_write_reg(LAPIC_TIMER_INITIAL, 0);
    
    // Calculate frequency: ticks_in_10ms * 100 = ticks per second
    // Multiply by 16 because we used a divider of 16
    lapic_timer_frequency = ticks_in_10ms * 100 * 16;
    
    // Restore interrupt state
    if (flags & 0x200) {
        asm volatile("sti");
    }
    
    return lapic_timer_frequency;
}

void lapic_timer_init(uint32_t frequency, uint8_t vector) {
    if (!lapic_is_enabled()) {
        framebuffer_draw_string("ERROR: LAPIC not enabled for timer!", 10, 700, COLOR_RED, COLOR_BLACK);
        return;
    }
    
    // Calibrate if not already done
    if (lapic_timer_frequency == 0) {
        lapic_timer_calibrate();
        if (lapic_timer_frequency == 0) {
            framebuffer_draw_string("ERROR: LAPIC timer calibration failed!", 10, 720, COLOR_RED, COLOR_BLACK);
            return;
        }
    }
    
    // Calculate the initial count for the desired frequency
    // We use a divider of 16, so actual frequency is lapic_timer_frequency / 16
    uint32_t ticks_per_interrupt = (lapic_timer_frequency / 16) / frequency;
    
    // Configure the timer
    lapic_write_reg(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIV_16);
    
    // Set up the LVT timer register: periodic mode, not masked, using specified vector
    lapic_write_reg(LAPIC_TIMER_LVT, vector | LAPIC_TIMER_PERIODIC);
    
    // Set the initial count (this starts the timer)
    lapic_write_reg(LAPIC_TIMER_INITIAL, ticks_per_interrupt);
    
    timer_initialized = true;
}

void lapic_timer_stop(void) {
    if (!lapic_is_enabled()) {
        return;
    }
    
    // Mask the timer interrupt
    uint32_t lvt = lapic_read_reg(LAPIC_TIMER_LVT);
    lapic_write_reg(LAPIC_TIMER_LVT, lvt | LAPIC_TIMER_MASKED);
    
    // Set initial count to 0 to stop the timer
    lapic_write_reg(LAPIC_TIMER_INITIAL, 0);
    
    timer_initialized = false;
}

bool lapic_timer_is_running(void) {
    if (!lapic_is_enabled() || !timer_initialized) {
        return false;
    }
    
    // Check if timer is not masked and has a non-zero initial count
    uint32_t lvt = lapic_read_reg(LAPIC_TIMER_LVT);
    uint32_t initial = lapic_read_reg(LAPIC_TIMER_INITIAL);
    
    return !(lvt & LAPIC_TIMER_MASKED) && (initial != 0);
}