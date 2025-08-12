// kernel/keyboard_task.h
#pragma once

// The keyboard polling task function
void keyboard_poll_task(void);

// Get a pointer to the keyboard polling task
void (*get_keyboard_poll_task(void))(void);