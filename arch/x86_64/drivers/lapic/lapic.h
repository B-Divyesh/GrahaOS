// arch/x86_64/drivers/lapic/lapic.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

// LAPIC Register Offsets
#define LAPIC_REG_ID                0x0020  // Local APIC ID
#define LAPIC_REG_VERSION           0x0030  // Local APIC Version
#define LAPIC_REG_TPR               0x0080  // Task Priority Register
#define LAPIC_REG_EOI               0x00B0  // End of Interrupt
#define LAPIC_REG_LDR               0x00D0  // Logical Destination Register
#define LAPIC_REG_DFR               0x00E0  // Destination Format Register
#define LAPIC_REG_SIV               0x00F0  // Spurious Interrupt Vector
#define LAPIC_REG_ISR0              0x0100  // In-Service Register (bits 0-31)
#define LAPIC_REG_TMR0              0x0180  // Trigger Mode Register (bits 0-31)
#define LAPIC_REG_IRR0              0x0200  // Interrupt Request Register (bits 0-31)
#define LAPIC_REG_ESR               0x0280  // Error Status Register
#define LAPIC_REG_ICR_LOW           0x0300  // Interrupt Command Register (low)
#define LAPIC_REG_ICR_HIGH          0x0310  // Interrupt Command Register (high)
#define LAPIC_REG_LVT_TIMER         0x0320  // LVT Timer Register
#define LAPIC_REG_LVT_THERMAL       0x0330  // LVT Thermal Sensor Register
#define LAPIC_REG_LVT_PERF          0x0340  // LVT Performance Counter Register
#define LAPIC_REG_LVT_LINT0         0x0350  // LVT LINT0 Register
#define LAPIC_REG_LVT_LINT1         0x0360  // LVT LINT1 Register
#define LAPIC_REG_LVT_ERROR         0x0370  // LVT Error Register
#define LAPIC_REG_TIMER_INIT_COUNT  0x0380  // Initial Count for Timer
#define LAPIC_REG_TIMER_CURR_COUNT  0x0390  // Current Count for Timer
#define LAPIC_REG_TIMER_DIVIDE      0x03E0  // Divide Configuration for Timer

// Flags for SIV register
#define LAPIC_SIV_ENABLE            (1 << 8) // Bit 8 enables the APIC

// Flags for LVT registers
#define LAPIC_LVT_MASKED            (1 << 16)
#define LAPIC_LVT_TRIGGER_LEVEL     (1 << 15)
#define LAPIC_LVT_REMOTE_IRR        (1 << 14)
#define LAPIC_LVT_PIN_POLARITY      (1 << 13)
#define LAPIC_LVT_DELIVERY_STATUS   (1 << 12)

// Timer modes
#define LAPIC_TIMER_ONESHOT         0
#define LAPIC_TIMER_PERIODIC        (1 << 17)

/**
 * @brief Initializes the Local APIC for the current CPU core.
 *        This only maps the LAPIC registers but doesn't enable it.
 */
void lapic_init(void);

/**
 * @brief Actually enables the LAPIC (call this when ready to switch from PIC)
 */
void lapic_enable(void);

/**
 * @brief Sends an End-of-Interrupt signal to the LAPIC.
 */
void lapic_eoi(void);

/**
 * @brief Gets the ID of the current CPU's LAPIC.
 * @return The 8-bit LAPIC ID.
 */
uint32_t lapic_get_id(void);

/**
 * @brief Checks if LAPIC is available and enabled
 * @return true if LAPIC is enabled and ready for use
 */
bool lapic_is_enabled(void);

/**
 * @brief Gets the LAPIC base address
 * @return Virtual address of LAPIC registers
 */
volatile uint32_t* lapic_get_base(void);