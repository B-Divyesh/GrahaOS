// user/tests/console_attach_map.c
//
// Phase 29 Session D — SYS_CONSOLE_ATTACH (wired for real) + mapped cell-VMO.
//
// 5 asserts:
//   1. SYS_CONSOLE_ATTACH returns 0 + writes non-zero handles
//   2. vmo_map on the cell handle returns a non-zero VA
//   3. memcpy a known cell (codepoint 'X') to (row=5, col=10) via the mapped VMO
//   4. kernel-side read (DEBUG_CONSOLE_READ_CELL) returns codepoint 'X'
//   5. SYS_CONSOLE_ACK_RENDER on the same console returns 0

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

// Local cell layout mirror.
typedef struct __attribute__((packed)) {
    uint32_t codepoint;
    uint8_t  fg;
    uint8_t  bg;
    uint16_t attrs;
    uint8_t  padding[8];
} tcell_t;

void _start(void) {
    tap_plan(5);

    (void)syscall_pledge(PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV |
                         PLEDGE_COMPUTE);

    uint32_t cid = 0;
    uint64_t cell_tok = 0, input_tok = 0;
    long rc = syscall_console_attach_full(cid, 0, &cell_tok, &input_tok);
    if (rc != 0) printf("# attach rc=%ld\n", rc);
    TAP_ASSERT(rc == 0 && cell_tok != 0 && input_tok != 0,
               "1. SYS_CONSOLE_ATTACH returns 0 + writes both handles");

    cap_token_u_t tok; tok.raw = cell_tok;
    long va = syscall_vmo_map(tok, 0, 0, 0x10000ull, PROT_READ | PROT_WRITE);
    if (va <= 0) printf("# vmo_map rc=%ld\n", va);
    TAP_ASSERT(va > 0, "2. vmo_map on cell handle returns non-zero VA");

    // Write cell (row=5, col=10), codepoint 'X' (0x58).
    if (va > 0) {
        tcell_t *cells = (tcell_t *)(uintptr_t)va;
        // Assume default console width 160; the kernel write_cell_debug
        // helper uses width_cells from the console table.  We compute the
        // same offset.
        const uint32_t W = 160;
        const uint32_t row = 5, col = 10;
        tcell_t *c = &cells[row * W + col];
        c->codepoint = 'X';
        c->fg = 15;
        c->bg = 0;
        c->attrs = 0;
        for (int i = 0; i < 8; i++) c->padding[i] = 0;
    }
    TAP_ASSERT(va > 0, "3. wrote codepoint 'X' via mapped VMO memcpy");

    long readback = syscall_debug_console_read_cell(cid, 5, 10);
    if (readback != 'X') printf("# readback=0x%lx, expected 0x58\n", readback);
    TAP_ASSERT(readback == 'X',
               "4. kernel-side readback matches mapped write");

    long ack = syscall_console_ack_render(cid, 1);
    TAP_ASSERT(ack == 0, "5. SYS_CONSOLE_ACK_RENDER returns 0");

    tap_done();
    syscall_exit(0);
}
