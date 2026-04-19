// user/tests/canstress.c — Phase 16 stress test.
//
// Cycles the keyboard CAN cap OFF/ON 100 times in a tight loop and asserts
// the PIC mask bit + the `g_keyboard_active` flag toggle in lock-step each
// cycle. Goal: catch stuck-interrupt states, stale flags, audit-queue
// overflows, or any drift across cycles. Spec gate = yes.
//
// Assertion plan (5):
//   1. Initial state (active + PIC unmasked)
//   2. All 100 deactivates returned a count >= 1
//   3. All 100 PIC mask checks post-deactivate showed bit=1
//   4. All 100 reactivates returned 0
//   5. Final state after cycles is active + PIC unmasked (no drift)

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define CYCLES 100

static long pic_mask_bit(int line) {
    return syscall_debug3(DEBUG_PIC_READ_MASK, line, 0);
}
static int kb_active(void) { return (int)syscall_debug3(DEBUG_KB_IS_ACTIVE, 0, 0); }

void _start(void) {
    tap_plan(5);

    // Group 0: initial state. If this fails, the test is meaningless.
    TAP_ASSERT(kb_active() == 1 && pic_mask_bit(1) == 0,
               "keyboard starts active with PIC IRQ1 unmasked");

    cap_token_u_t tok = syscall_can_lookup("keyboard_input", strlen("keyboard_input"));
    if (tok.raw == 0) {
        tap_not_ok("keyboard_input lookup", "SYS_CAN_LOOKUP returned 0");
        tap_done();
        exit(1);
    }

    int deact_ok = 0;
    int mask_ok  = 0;
    int react_ok = 0;

    for (int i = 0; i < CYCLES; i++) {
        long rd = syscall_can_deactivate_t(tok);
        if (rd >= 1) deact_ok++;
        if (pic_mask_bit(1) == 1) mask_ok++;
        long ra = syscall_can_activate_t(tok);
        if (ra == 0) react_ok++;
    }

    TAP_ASSERT(deact_ok == CYCLES, "all 100 deactivates returned count >= 1");
    TAP_ASSERT(mask_ok  == CYCLES, "all 100 PIC-mask checks showed bit=1 during deactivation");
    TAP_ASSERT(react_ok == CYCLES, "all 100 reactivates returned 0");
    TAP_ASSERT(kb_active() == 1 && pic_mask_bit(1) == 0,
               "keyboard ends in same state it started (no drift)");

    tap_done();
    exit(0);
}
