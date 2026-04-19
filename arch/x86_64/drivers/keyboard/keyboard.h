// arch/x86_64/drivers/keyboard/keyboard.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initializes the keyboard driver.
 */
void keyboard_init(void);

/**
 * @brief The interrupt handler for the keyboard (IRQ1).
 *        Should be called from the main interrupt dispatcher.
 */
void keyboard_irq_handler(void);

/**
 * @brief Gets a character from the keyboard buffer.
 * @return The ASCII character if one is available, otherwise 0.
 * @note This is non-blocking. The syscall wrapper will make it blocking.
 */
char keyboard_getchar(void);

/**
 * @brief Get the number of keyboard interrupts received (for debugging)
 * @return Number of interrupts
 */
int keyboard_get_interrupt_count(void);

/**
 * @brief Handle a keyboard scancode (for polling mode)
 * @param scancode The scancode received from the keyboard
 */
void keyboard_handle_scancode(uint8_t scancode);

/**
 * @brief Phase 16 CAN callbacks. activate unmasks IRQ1 + drains stale
 *        buffer; deactivate masks IRQ1 + sets the active-flag guard.
 *        is_active reads the flag (used by SYS_DEBUG probes in tests).
 */
int  keyboard_activate(void);
int  keyboard_deactivate(void);
bool keyboard_is_active(void);