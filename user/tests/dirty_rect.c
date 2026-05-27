// user/tests/dirty_rect.c
//
// Phase 29 Session D — dirty-rect coalescing gate.
//
// 4 asserts:
//   1. After reset, both counters read 0
//   2. Single cell write + synthetic render bumps partial counter by 1,
//      full counter unchanged
//   3. Render with no dirty cells still bumps partial (treated as empty
//      partial — sub-redraw of 0 area)
//   4. Reset works again (both counters back to 0)

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    (void)syscall_pledge(PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV);

    // Drain any pending dirty rects with a synthetic render first.
    uint32_t cid = syscall_debug_console_get_selected();
    (void)syscall_debug_console_synthetic_render(cid);

    // 1. Reset + readback.
    (void)syscall_debug_dirty_rect_reset();
    uint64_t counts[2] = {0xFFFF, 0xFFFF};
    long rc = syscall_debug_dirty_rect_get_counts(counts);
    if (rc != 0 || counts[0] != 0 || counts[1] != 0) {
        printf("# after reset: partial=%lu full=%lu rc=%ld\n",
               (unsigned long)counts[0], (unsigned long)counts[1], rc);
    }
    TAP_ASSERT(rc == 0 && counts[0] == 0 && counts[1] == 0,
               "1. counters zero after reset");

    // 2. Write one cell + render.
    (void)syscall_debug_console_write_cell(cid, 3, 7, 'Z', 15, 0, 0);
    (void)syscall_debug_console_synthetic_render(cid);
    (void)syscall_debug_dirty_rect_get_counts(counts);
    if (counts[0] != 1 || counts[1] != 0) {
        printf("# after 1 write + render: partial=%lu full=%lu\n",
               (unsigned long)counts[0], (unsigned long)counts[1]);
    }
    TAP_ASSERT(counts[0] == 1 && counts[1] == 0,
               "2. one-cell write + render bumps partial by 1, full unchanged");

    // 3. Render again (no new writes; dirty ring drained) → still partial.
    (void)syscall_debug_console_synthetic_render(cid);
    (void)syscall_debug_dirty_rect_get_counts(counts);
    if (counts[0] != 2 || counts[1] != 0) {
        printf("# after second render: partial=%lu full=%lu\n",
               (unsigned long)counts[0], (unsigned long)counts[1]);
    }
    TAP_ASSERT(counts[0] == 2 && counts[1] == 0,
               "3. render with empty ring still bumps partial");

    // 4. Reset again works.
    (void)syscall_debug_dirty_rect_reset();
    (void)syscall_debug_dirty_rect_get_counts(counts);
    TAP_ASSERT(counts[0] == 0 && counts[1] == 0,
               "4. reset returns counters to 0");

    tap_done();
    syscall_exit(0);
}
