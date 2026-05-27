// user/tests/sprite_anim.c
//
// Phase 29 Session E gate test — SYS_CONSOLE_SPRITE_ANIMATE.
//
// 5 asserts:
//   1. animate returns positive slot handle (>=0)
//   2. after one DEBUG tick, cur_frame == 1
//   3. after enough ticks, state == COMMITTED (2)
//   4. animate with invalid sprite_id → negative -errno
//   5. animate with n_frames==0 → -EINVAL

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

// Local cell mirror (16 bytes).
typedef struct __attribute__((packed)) {
    uint32_t codepoint;
    uint8_t  fg;
    uint8_t  bg;
    uint16_t attrs;
    uint8_t  padding[8];
} tcell_t;

void _start(void) {
    tap_plan(5);

    (void)syscall_pledge(PLEDGE_SYS_CONTROL | PLEDGE_SYS_QUERY |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_COMPUTE);

    // Build 4 keyframes — each one a distinct 16-byte pattern.  Both
    // tui_cell_t-shaped *and* 8x16 sprite-shaped.
    tcell_t kf[4];
    for (int i = 0; i < 4; i++) {
        kf[i].codepoint = 0xE100u + (uint32_t)i;
        kf[i].fg = 15;
        kf[i].bg = 0;
        kf[i].attrs = 0;
        for (int j = 0; j < 8; j++) kf[i].padding[j] = (uint8_t)(0x10 * (i + 1));
    }

    // Test 1: register the animation.
    long slot = syscall_console_sprite_animate(/*cid*/ 0, /*sprite_id*/ 100,
                                               kf, 4, /*dur_ms*/ 10);
    if (slot < 0) printf("# animate rc=%ld\n", slot);
    TAP_ASSERT(slot >= 0, "1. sprite_animate returns non-negative slot");

    // Test 2: tick once, cur_frame should be 1.
    (void)syscall_debug_anim_tick(0);
    long frame = (slot >= 0) ? syscall_debug_anim_get_frame(0, (uint32_t)slot) : -1;
    if (frame != 1) printf("# frame after 1 tick = %ld\n", frame);
    TAP_ASSERT(frame == 1, "2. after one tick, cur_frame == 1");

    // Test 3: tick remaining frames; state should become COMMITTED.
    for (int i = 0; i < 5; i++) {
        (void)syscall_debug_anim_tick(0);
    }
    long state = (slot >= 0) ? syscall_debug_anim_get_state(0, (uint32_t)slot) : -1;
    if (state != 2) printf("# state after exhaust = %ld (expected 2=COMMITTED)\n", state);
    TAP_ASSERT(state == 2, "3. after n_frames ticks, state==COMMITTED");

    // Test 4: bad sprite_id.
    long bad_sid = syscall_console_sprite_animate(0, 0xFFFFFFu, kf, 4, 10);
    TAP_ASSERT(bad_sid < 0, "4. invalid sprite_id returns negative -errno");

    // Test 5: n_frames=0 should be rejected.
    long bad_n = syscall_console_sprite_animate(0, 101, kf, 0, 10);
    TAP_ASSERT(bad_n < 0, "5. n_frames==0 returns -EINVAL");

    tap_done();
    syscall_exit(0);
}
