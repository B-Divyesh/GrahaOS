#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "../drivers/video/framebuffer.h"

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

// Request framebuffer instead of terminal (which is deprecated)
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

// Define the start and end markers for the Limine requests
__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }
    return 0;
}

// Halt and catch fire function
static void hcf(void) {
    asm ("cli");  // Disable interrupts first
    for (;;) {
        asm ("hlt");
    }
}

void kmain(void) {
    // Ensure the bootloader actually understands our base revision
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // Initialize the framebuffer driver
    if (!framebuffer_init(&framebuffer_request)) {
        hcf();
    }

    // Clear the screen to a dark blue-gray
    framebuffer_clear(0x00101828);

    // Draw a welcome banner with gradient-like effect
    framebuffer_draw_rect(50, 50, 500, 120, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 496, 116, 0x00004488); // Inner darker blue
    framebuffer_draw_rect(54, 54, 492, 112, 0x000066AA); // Lighter inner

    // Draw the main title
    framebuffer_draw_string("GrahaOS!", 70, 70, COLOR_WHITE, 0x000066AA);

    // Draw status messages
    framebuffer_draw_string("Phase 1: Graphics Library OK", 70, 90, COLOR_GREEN, 0x000066AA);
    framebuffer_draw_string("Font Rendering: ACTIVE", 70, 110, COLOR_CYAN, 0x000066AA);
    framebuffer_draw_string("Framebuffer: Ready", 70, 130, COLOR_YELLOW, 0x000066AA);

    // Draw some decorative elements
    framebuffer_draw_rect_outline(40, 40, 520, 140, COLOR_WHITE);
    framebuffer_draw_rect_outline(42, 42, 516, 136, COLOR_LIGHT_GRAY);

    // Draw a small info box
    framebuffer_draw_rect(50, 200, 300, 80, COLOR_DARK_GRAY);
    framebuffer_draw_rect_outline(50, 200, 300, 80, COLOR_WHITE);
    framebuffer_draw_string("Resolution:", 60, 210, COLOR_WHITE, COLOR_DARK_GRAY);

    // Note: In a real implementation, you'd format the resolution values
    framebuffer_draw_string("Graphics driver loaded", 60, 230, COLOR_GREEN, COLOR_DARK_GRAY);
    framebuffer_draw_string("Memory management: OK", 60, 250, COLOR_GREEN, COLOR_DARK_GRAY);

    // We're done, just hang...
    hcf();
}
