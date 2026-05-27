// user/tests/console_read_input.c
//
// Phase 29 Session D — SYS_CONSOLE_READ_INPUT gate.
//
// Exercises the kernel input ring + drain path without spawning fbd.  We
// inject scancodes via DEBUG_INJECT_SCANCODE (existing slot 69 — drives the
// real keyboard.c::keyboard_handle_scancode path, which now feeds
// console_post_input_event for the selected console) and read them back
// via the new SYS_CONSOLE_READ_INPUT.
//
// 5 asserts:
//   1. read on empty ring returns 0
//   2. inject 'A' (scancode 0x1E), read 1 event, verify key == 0x1E
//   3. inject 3 scancodes then read with max=2 → 2 returned with MORE flag
//   4. drain remaining 1 → returned, no MORE flag
//   5. empty ring again returns 0

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(5);

    (void)syscall_pledge(PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV);

    uint32_t selected = syscall_debug_console_get_selected();

    // Drain any stale events first (any prior test or boot keystroke).
    input_event_u_t scratch[16];
    while (1) {
        long n = syscall_console_read_input(selected, scratch, 16);
        if (n <= 0) break;
        n &= ~CONSOLE_INPUT_MORE_FLAG;
        if (n == 0) break;
    }

    // 1. Empty ring → 0.
    input_event_u_t ev[8];
    long rc = syscall_console_read_input(selected, ev, 8);
    if (rc != 0) printf("# expected empty 0, got %ld\n", rc);
    TAP_ASSERT(rc == 0, "1. read on empty ring returns 0");

    // 2. Inject scancode 0x1E ('a' press), read 1.
    (void)syscall_debug_inject_scancode(0x1E);
    rc = syscall_console_read_input(selected, ev, 8);
    long count = rc & ~CONSOLE_INPUT_MORE_FLAG;
    if (count != 1 || ev[0].key != 0x1E) {
        printf("# got count=%ld key=%u\n", count, (unsigned)ev[0].key);
    }
    TAP_ASSERT(count == 1 && ev[0].key == 0x1E,
               "2. injected scancode arrives in ring");

    // 3. Inject 3 scancodes, read with max=2 → 2 + MORE.
    (void)syscall_debug_inject_scancode(0x10);  // 'q'
    (void)syscall_debug_inject_scancode(0x11);  // 'w'
    (void)syscall_debug_inject_scancode(0x12);  // 'e'
    rc = syscall_console_read_input(selected, ev, 2);
    count = rc & ~CONSOLE_INPUT_MORE_FLAG;
    int more_set = (rc & CONSOLE_INPUT_MORE_FLAG) ? 1 : 0;
    if (count != 2 || !more_set) {
        printf("# got count=%ld more=%d (raw rc=%ld)\n", count, more_set, rc);
    }
    TAP_ASSERT(count == 2 && more_set,
               "3. read with max=2 returns 2 + MORE flag");

    // 4. Drain remaining 1.
    rc = syscall_console_read_input(selected, ev, 8);
    count = rc & ~CONSOLE_INPUT_MORE_FLAG;
    more_set = (rc & CONSOLE_INPUT_MORE_FLAG) ? 1 : 0;
    TAP_ASSERT(count >= 1 && !more_set,
               "4. drain remaining returns last events, no MORE");

    // 5. Empty again.
    rc = syscall_console_read_input(selected, ev, 8);
    TAP_ASSERT(rc == 0, "5. ring empty again after full drain");

    tap_done();
    syscall_exit(0);
}
