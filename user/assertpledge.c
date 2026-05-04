// user/assertpledge.c
// Phase 26 Stage D.4 — child binary used by tests/pledge_narrow_exec.tap.c.
//
// When spawned via SYS_PLEDGE | PLEDGE_FLAG_NARROW_EXEC with a narrowed
// pledge mask, this binary reads its own pledge_mask via SYS_DEBUG /
// DEBUG_READ_PLEDGE and exits with status code = mask. The TAP test reaps
// the child and verifies the exit code matches the requested narrow mask.
//
// Exit code conventions (test-driven):
//   exit(mask)            -- normal report; test inspects mask via syscall_wait
//   exit(0xFE)            -- DEBUG_READ_PLEDGE returned 0 (something wrong)
//
// We can't print without PLEDGE_COMPUTE; this binary always asks for at
// least PLEDGE_COMPUTE in its narrow mask so syscall_putc works for
// optional debug printing.

#include "syscalls.h"

#include <stdint.h>

void _start(void) {
    long mask = syscall_debug3(DEBUG_READ_PLEDGE, 0, 0);
    if (mask <= 0) {
        syscall_exit(0xFE);
    }
    // Truncate to 8 bits for exit-code transport. The TAP test only
    // checks specific bits, not the full mask.
    syscall_exit((int)(mask & 0xFF));
}
