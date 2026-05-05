// user/tests/console_init.tap.c
//
// Phase 27 Block A — Stage A2 gate test for the virtual console subsystem.
//
// Verifies the syscall surface that fbd will consume at Stage A4:
//   1. SYS_CONSOLE_SWITCH(0) succeeds (basic dispatch + pledge gate)
//   2. SYS_CONSOLE_SWITCH(99) returns -EINVAL (out-of-range)
//   3. SYS_CONSOLE_SWITCH(3) succeeds (last valid console; 4 default)
//   4. SYS_CONSOLE_SWITCH(4) returns -EINVAL (just past the boundary)
//   5. SYS_CONSOLE_ACK_RENDER(0, 1) succeeds + sets fbd_alive
//   6. SYS_CONSOLE_ACK_RENDER(99, 1) returns -EINVAL
//
// Cap-gating (CAP_KIND_SYSTEM RIGHT_INVOKE) is intentionally NOT enforced in
// Stage A2 — it lands at Stage C2 alongside FU26.D capability inheritance.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(6);

    long rc;

    rc = syscall_console_switch(0);
    if (rc != 0) printf("# rc1=%ld (expected 0)\n", rc);
    TAP_ASSERT(rc == 0, "1. SYS_CONSOLE_SWITCH(0) returns 0");

    rc = syscall_console_switch(99);
    if (rc != -22) printf("# rc2=%ld (expected -22)\n", rc);
    TAP_ASSERT(rc == -22, "2. SYS_CONSOLE_SWITCH(99) returns -EINVAL");

    rc = syscall_console_switch(3);
    if (rc != 0) printf("# rc3=%ld (expected 0)\n", rc);
    TAP_ASSERT(rc == 0, "3. SYS_CONSOLE_SWITCH(3) returns 0");

    rc = syscall_console_switch(4);
    if (rc != -22) printf("# rc4=%ld (expected -22)\n", rc);
    TAP_ASSERT(rc == -22, "4. SYS_CONSOLE_SWITCH(4) returns -EINVAL");

    rc = syscall_console_ack_render(0, 1);
    if (rc != 0) printf("# rc5=%ld (expected 0)\n", rc);
    TAP_ASSERT(rc == 0, "5. SYS_CONSOLE_ACK_RENDER(0, 1) returns 0");

    rc = syscall_console_ack_render(99, 1);
    if (rc != -22) printf("# rc6=%ld (expected -22)\n", rc);
    TAP_ASSERT(rc == -22, "6. SYS_CONSOLE_ACK_RENDER(99, 1) returns -EINVAL");

    // Switch back to console 0 so subsequent gate tests don't see fbd
    // pointing at a different console (cosmetic, no functional effect since
    // fbd doesn't exist yet in Stage A2).
    syscall_console_switch(0);

    tap_done();
    syscall_exit(0);
}
