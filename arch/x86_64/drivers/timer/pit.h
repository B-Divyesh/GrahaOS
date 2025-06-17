#pragma once
#include <stdint.h>

/**
 * @brief Initializes the Programmable Interval Timer (PIT).
 * @param frequency The desired frequency in Hz for the timer interrupt (IRQ0).
 */
void pit_init(uint32_t frequency);
