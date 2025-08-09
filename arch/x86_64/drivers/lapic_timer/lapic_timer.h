// arch/x86_64/drivers/lapic_timer/lapic_timer.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the LAPIC timer for the current CPU core
 * @param frequency Desired timer frequency in Hz (e.g., 100 for 100Hz)
 * @param vector Interrupt vector number for timer interrupts (e.g., 32 for IRQ0)
 */
void lapic_timer_init(uint32_t frequency, uint8_t vector);

/**
 * @brief Stop the LAPIC timer on the current CPU
 */
void lapic_timer_stop(void);

/**
 * @brief Check if LAPIC timer is initialized and running
 * @return true if timer is running, false otherwise
 */
bool lapic_timer_is_running(void);

/**
 * @brief Calibrate the LAPIC timer using the PIT as reference
 * @return The LAPIC timer frequency in Hz, or 0 on failure
 */
uint32_t lapic_timer_calibrate(void);