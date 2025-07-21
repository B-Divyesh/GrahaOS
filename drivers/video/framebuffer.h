/* drivers/video/framebuffer.h */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../kernel/sync/spinlock.h"
// Forward declaration
struct limine_framebuffer_request;

// --- Color Definitions ---
#define COLOR_BLACK      0x00000000
#define COLOR_WHITE      0x00FFFFFF
#define COLOR_RED        0x00FF0000
#define COLOR_GREEN      0x0000FF00
#define COLOR_BLUE       0x000000FF
#define COLOR_CYAN       0x0000FFFF
#define COLOR_MAGENTA    0x00FF00FF
#define COLOR_YELLOW     0x00FFFF00
#define COLOR_GRAHA_BLUE 0xFF0066CC
#define COLOR_DARK_GRAY  0x00404040
#define COLOR_LIGHT_GRAY 0x00C0C0C0

// --- Public Functions ---

/**
 * @brief Initializes the framebuffer driver
 * @param fb_request The framebuffer request from Limine
 * @return true if successful, false otherwise
 */
bool framebuffer_init(volatile struct limine_framebuffer_request *fb_request);

/**
 * @brief Gets the framebuffer width
 * @return Width in pixels
 */
uint32_t framebuffer_get_width(void);

/**
 * @brief Gets the framebuffer height
 * @return Height in pixels
 */
uint32_t framebuffer_get_height(void);

/**
 * @brief Draws a single pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color 32-bit ARGB color
 */
void framebuffer_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/**
 * @brief Draws a filled rectangle
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color Fill color
 */
void framebuffer_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

/**
 * @brief Draws a rectangle outline
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color Outline color
 */
void framebuffer_draw_rect_outline(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

/**
 * @brief Draws a single character (8x16 font)
 * @param c Character to draw
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param fg_color Foreground color
 * @param bg_color Background color
 */
 void framebuffer_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color);

/**
 * @brief Draws a null-terminated string
 * @param str String to draw
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param fg_color Foreground color
 * @param bg_color Background color
 */
void framebuffer_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);


// Add this function declaration
void framebuffer_draw_hex(uint64_t value, int x, int y, uint32_t fg_color, uint32_t bg_color);

/**
 * @brief Clears the entire screen
 * @param color Fill color
 */
void framebuffer_clear(uint32_t color);

