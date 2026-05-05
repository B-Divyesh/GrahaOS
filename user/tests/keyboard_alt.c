// user/tests/keyboard_alt.c
//
// Phase 27 Block A — Stage A3 gate test for keyboard Alt+N → console-switch.
//
// Injects PS/2 scancodes via DEBUG_INJECT_SCANCODE into the keyboard ISR
// path and verifies that the selected console moves to the expected target.
// Verifies left-Alt only (right-Alt deferred to FU27.X).
//
// PS/2 set-1 scancodes used:
//   0x38 = Left-Alt press
//   0xB8 = Left-Alt release
//   0x02..0x05 = '1'..'4' press (0x82..0x85 release; not needed for v1 test)

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

static void inject(uint8_t code) {
    syscall_debug_inject_scancode(code);
}

void _start(void) {
    tap_plan(6);

    // Baseline: switch to console 0 explicitly.
    syscall_console_switch(0);
    if (syscall_debug_console_get_selected() != 0) {
        printf("# baseline: selected != 0 (got %u)\n",
               syscall_debug_console_get_selected());
    }

    // 1: Alt+2 → switches to console 1.
    inject(0x38);  // Alt down
    inject(0x03);  // '2' (scancode 0x03 = key '2')
    inject(0xB8);  // Alt up
    uint32_t sel = syscall_debug_console_get_selected();
    if (sel != 1) printf("# alt2: sel=%u (expected 1)\n", sel);
    TAP_ASSERT(sel == 1, "1. Alt+2 selects console 1");

    // 2: Alt+3 → switches to console 2.
    inject(0x38);
    inject(0x04);  // '3'
    inject(0xB8);
    sel = syscall_debug_console_get_selected();
    if (sel != 2) printf("# alt3: sel=%u (expected 2)\n", sel);
    TAP_ASSERT(sel == 2, "2. Alt+3 selects console 2");

    // 3: Alt+4 → switches to console 3.
    inject(0x38);
    inject(0x05);  // '4'
    inject(0xB8);
    sel = syscall_debug_console_get_selected();
    if (sel != 3) printf("# alt4: sel=%u (expected 3)\n", sel);
    TAP_ASSERT(sel == 3, "3. Alt+4 selects console 3");

    // 4: Alt+1 → returns to console 0.
    inject(0x38);
    inject(0x02);  // '1'
    inject(0xB8);
    sel = syscall_debug_console_get_selected();
    if (sel != 0) printf("# alt1: sel=%u (expected 0)\n", sel);
    TAP_ASSERT(sel == 0, "4. Alt+1 selects console 0");

    // 5: Plain '2' (no Alt held) does NOT change console.
    syscall_console_switch(2);  // baseline: select console 2
    inject(0x03);  // '2' alone (no Alt)
    sel = syscall_debug_console_get_selected();
    if (sel != 2) printf("# plain_2: sel=%u (expected 2)\n", sel);
    TAP_ASSERT(sel == 2, "5. Plain '2' (no Alt) doesn't change console");

    // 6: Alt+5 (scancode 0x06) is NOT in the intercept range (Alt+1..Alt+4
    // only) — should not change console. Inject Alt+5; selected should stay
    // at 2 (set in test 5).
    inject(0x38);
    inject(0x06);  // '5' — outside Alt+N intercept range
    inject(0xB8);
    sel = syscall_debug_console_get_selected();
    if (sel != 2) printf("# alt5: sel=%u (expected 2; out-of-range)\n", sel);
    TAP_ASSERT(sel == 2, "6. Alt+5 (out of range) doesn't change console");

    // Restore baseline before exit.
    syscall_console_switch(0);

    tap_done();
    syscall_exit(0);
}
