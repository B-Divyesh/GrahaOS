// user/tests/console_sprite.c
//
// Phase 27 Block B — Stage B1 sprite-cell registry gate test.
//
// Verifies:
//   1. SYS_CONSOLE_SPRITE_REGISTER accepts a registration.
//   2. Cell with codepoint=TUI_SPRITE_BASE (0xE100, sprite #0) renders
//      using the registered bitmap (verify a foreground pixel).
//   3. Codepoint outside sprite range falls through to ASCII rendering.
//   4. Out-of-range sprite_id is rejected.

#include "../libtap.h"
#include "../syscalls.h"
#include "../libtui/libtui.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    (void)syscall_console_switch(0);

    // 1. Register a sprite (id=0) with all-pixels-set bitmap.
    static const uint8_t all_set[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    long rc = syscall_console_sprite_register(/*console_id*/ 0,
                                              /*sprite_id*/ 0,
                                              all_set);
    if (rc != 0) printf("# sprite_register rc=%ld\n", rc);
    TAP_ASSERT(rc == 0, "1. sprite_register(0, 0, all_set) returns 0");

    // 2. Write cell with codepoint=0xE100, fg=15(white), bg=0(black).
    // After synthetic render, the entire cell should be foreground (white)
    // because the sprite has all bits set.
    rc = syscall_debug_console_write_cell(/*console_id*/ 0,
                                          /*row*/ 20, /*col*/ 0,
                                          /*codepoint*/ TUI_SPRITE_BASE,
                                          /*fg*/ 15, /*bg*/ 0,
                                          /*attrs*/ 0);
    (void)rc;
    rc = syscall_debug_console_synthetic_render(0);
    (void)rc;
    uint32_t px = syscall_debug_fb_read_pixel(4, 20*16 + 8);
    if (px != 0x00FFFFFFu) printf("# sprite px=0x%08x (expected 0x00FFFFFF)\n",
                                  (unsigned)px);
    TAP_ASSERT(px == 0x00FFFFFFu,
               "2. sprite cell renders as foreground (all-bits-set sprite)");

    // 3. Codepoint outside sprite range (use 'B') still renders normally.
    rc = syscall_debug_console_write_cell(/*console_id*/ 0,
                                          /*row*/ 21, /*col*/ 0,
                                          /*codepoint*/ 'B',
                                          /*fg*/ 15, /*bg*/ 0,
                                          /*attrs*/ 0);
    (void)rc;
    rc = syscall_debug_console_synthetic_render(0);
    (void)rc;
    uint32_t px_B_corner = syscall_debug_fb_read_pixel(0, 21*16 + 0);
    if (px_B_corner != 0x00000000u) {
        printf("# 'B' top-left pixel=0x%08x (expected bg 0x00000000)\n",
               (unsigned)px_B_corner);
    }
    TAP_ASSERT(px_B_corner == 0x00000000u,
               "3. ASCII codepoint renders ASCII glyph (not sprite)");

    // 4. Out-of-range sprite_id is rejected.
    rc = syscall_console_sprite_register(/*console_id*/ 0,
                                         /*sprite_id*/ 99999,
                                         all_set);
    if (rc == 0) printf("# unexpected: out-of-range sprite_id accepted\n");
    TAP_ASSERT(rc != 0, "4. out-of-range sprite_id rejected");

    tap_done();
    syscall_exit(0);
}
