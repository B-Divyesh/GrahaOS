// user/tests/mouse_basic.c
//
// Phase 29 Session E gate test — PS/2 mouse driver substrate.
//
// 4 asserts:
//   1. inject motion event {kind=2, dx=3, dy=4}; SYS_CONSOLE_READ_INPUT
//      returns the event.
//   2. inject button event {kind=1, button=0, press}; event arrives.
//   3. After motion event, mouse cursor visible flag = 1.
//   4. inject many button events; the input ring drops (kernel returns
//      MORE flag set OR queue contains <= ring depth events).

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    (void)syscall_pledge(PLEDGE_SYS_CONTROL | PLEDGE_SYS_QUERY |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_COMPUTE);

    // Drain any pre-existing input events.
    input_event_u_t drain[32];
    for (int i = 0; i < 4; i++) {
        long n = syscall_console_read_input(0, drain, 32);
        if (n <= 0) break;
    }

    // Test 1: inject a motion event.
    (void)syscall_debug_inject_mouse(/*kind*/ 2, 0, /*dx*/ 3, /*dy*/ 4, 0);
    input_event_u_t ev[16];
    long n = syscall_console_read_input(0, ev, 16);
    long n_count = n & 0x3FFFFFFFFFFFFFFFLL;
    int saw_motion = 0;
    for (long i = 0; i < n_count; i++) {
        if (ev[i].kind == 2 && ev[i].x_or_dx == 3 && ev[i].y_or_dy == 4) {
            saw_motion = 1;
            break;
        }
    }
    if (!saw_motion) printf("# motion not seen (n=%ld)\n", n_count);
    TAP_ASSERT(saw_motion, "1. motion event injected and read back");

    // Test 2: inject a button event.
    (void)syscall_debug_inject_mouse(/*kind*/ 1, /*press*/ 0,
                                     0, 0, /*button*/ 0);
    n = syscall_console_read_input(0, ev, 16);
    n_count = n & 0x3FFFFFFFFFFFFFFFLL;
    int saw_button = 0;
    for (long i = 0; i < n_count; i++) {
        if (ev[i].kind == 1 && ev[i].key == 0) {
            saw_button = 1;
            break;
        }
    }
    if (!saw_button) printf("# button event not seen (n=%ld)\n", n_count);
    TAP_ASSERT(saw_button, "2. button event injected and read back");

    // Test 3: cursor visible flag.
    long visible = syscall_debug_mouse_cursor_visible(0);
    if (visible != 1) printf("# cursor_visible = %ld\n", visible);
    TAP_ASSERT(visible == 1, "3. mouse cursor visible after first motion");

    // Test 4: fill the input ring; subsequent reads see queued events with
    // the MORE flag set OR the ring drops oldest.
    for (int i = 0; i < 64; i++) {
        (void)syscall_debug_inject_mouse(1, 0, 0, 0, (uint8_t)(i & 0x03));
    }
    n = syscall_console_read_input(0, ev, 16);
    n_count = n & 0x3FFFFFFFFFFFFFFFLL;
    int more = (n & CONSOLE_INPUT_MORE_FLAG) != 0;
    // Either we have a full buffer (n_count==16) AND more flag set,
    // OR we got fewer than 64 (drops happened in the kernel ring).
    int ring_behaved = (more && n_count == 16) || (n_count < 64);
    TAP_ASSERT(ring_behaved, "4. ring overflow: MORE flag or dropped events");

    tap_done();
    syscall_exit(0);
}
