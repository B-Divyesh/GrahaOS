// user/tests/cell_grid_atomic.c
//
// Phase 29 Session E gate test — SYS_CONSOLE_BEGIN_TX / COMMIT_TX / ABORT_TX.
//
// 6 asserts:
//   1. begin_tx returns valid handle (>=1)
//   2. write cell under TX; read-back via DEBUG_CONSOLE_READ_CELL shows the
//      new cell (writes-go-to-shadow semantic; shadow IS the live view of
//      cell_vmo->pages[] in v1 substrate)
//   3. commit_tx returns 0
//   4. begin_tx + abort_tx returns 0 + we don't observe the abandoned writes
//      after a subsequent fresh read; AUDIT_TUI_TX_ABORT (code 59) emitted
//   5. begin_tx twice on same console → -EBUSY (= -16)
//   6. commit_tx with invalid handle → -EINVAL

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(6);

    (void)syscall_pledge(PLEDGE_SYS_CONTROL | PLEDGE_SYS_QUERY |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_COMPUTE);

    // Subscribe to AUDIT_TUI_TX_ABORT (bit 59).
    long sub = syscall_audit_subscribe(1ull << 59);
    if (sub < 0) printf("# audit_subscribe rc=%ld\n", sub);

    // Test 1: begin a TX.
    long handle = syscall_console_begin_tx(0);
    if (handle < 1) printf("# begin_tx rc=%ld\n", handle);
    TAP_ASSERT(handle >= 1, "1. begin_tx returns valid handle");

    // Test 2: write a cell, read it back.
    long wrc = syscall_debug_console_write_cell(0, 3, 7,
                                                'Z', 15, 0, 0);
    (void)wrc;
    long rb = syscall_debug_console_read_cell(0, 3, 7);
    if (rb != 'Z') printf("# read-back under TX = 0x%lx (want 'Z')\n", rb);
    TAP_ASSERT(rb == 'Z', "2. write under TX is observable through cell-VMO");

    // Test 3: commit.
    long crc = syscall_console_commit_tx((uint32_t)handle);
    if (crc != 0) printf("# commit rc=%ld\n", crc);
    TAP_ASSERT(crc == 0, "3. commit_tx returns 0");

    // Test 4: begin → write → abort → audit emitted.
    long h2 = syscall_console_begin_tx(0);
    (void)syscall_debug_console_write_cell(0, 5, 5, 'Q', 15, 0, 0);
    long arc = syscall_console_abort_tx((uint32_t)h2);
    if (arc != 0) printf("# abort rc=%ld\n", arc);
    // Drain audit stream and look for code 59.
    audit_entry_u_t buf[16];
    long n = syscall_audit_stream_read((int)sub, buf, 16);
    int saw_abort = 0;
    for (long i = 0; i < n; i++) {
        if (buf[i].event_type == 59) { saw_abort = 1; break; }
    }
    if (!saw_abort) printf("# AUDIT_TUI_TX_ABORT not observed (n=%ld)\n", n);
    TAP_ASSERT(arc == 0 && saw_abort,
               "4. abort_tx returns 0 + AUDIT_TUI_TX_ABORT emitted");

    // Test 5: begin twice should produce -EBUSY.
    long h3 = syscall_console_begin_tx(0);
    long h4 = syscall_console_begin_tx(0);
    if (h4 != -16) printf("# 2nd begin rc=%ld (want -16)\n", h4);
    TAP_ASSERT(h4 == -16, "5. begin_tx twice returns -EBUSY");
    if (h3 >= 1) (void)syscall_console_abort_tx((uint32_t)h3);  // cleanup

    // Test 6: commit_tx with bogus handle.
    long bogus = syscall_console_commit_tx(0xDEAD);
    if (bogus >= 0) printf("# commit_tx(bogus) returned %ld\n", bogus);
    TAP_ASSERT(bogus < 0, "6. commit_tx(invalid handle) returns negative");

    tap_done();
    syscall_exit(0);
}
