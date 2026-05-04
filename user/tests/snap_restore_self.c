// user/tests/snap_restore_self.c — Phase 25 / FU24.I caller-page restore.
//
// Validates that snap_restore(SNAP_SCOPE_SELF) actually rolls back the
// caller's heap + BSS pages. Pre-Phase-25 code skipped the entire caller,
// so writes to globals "bled through" the restore and the test would
// silently observe post-snap state. Stage C made restore_pages skip only
// the active stack page (identified via current->syscall_frame_ptr->user_rsp)
// while replaying every other captured page back into the caller's CR3.
//
// Tests:
//   1. snap_create(SCOPE_SELF) returns a handle.
//   2. After mutation + snap_restore, a BSS marker is reverted to its
//      pre-snap value.
//   3. After mutation + snap_restore, a heap-region marker (allocated
//      before snap_create, so its page is captured) is reverted.
//   4. snap_delete after restore returns 0.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

extern int printf(const char *fmt, ...);
extern void exit(int);

// BSS marker — captured by snap_walk_user_half because BSS pages are
// PRESENT|USER in the caller's PML4. Pre-Phase-25 SCOPE_SELF restore was
// a no-op for the caller, so a write here would NOT revert.
static volatile uint32_t g_marker = 0xAAAAAAAAu;

// Heap-region marker — allocate before snap_create so the page is
// captured; mutate after; expect revert.
static char *g_heap_region = NULL;

void _start(void) {
    tap_plan(4);

    // Set up the heap region. malloc-equivalent — calloc 8 KiB so we span
    // two pages.
    g_heap_region = (char *)calloc(8192, 1);
    if (!g_heap_region) {
        printf("  calloc failed; aborting\n");
        TAP_ASSERT(0, "0. calloc 8 KiB heap region");
        TAP_ASSERT(0, "1. snap_create(SCOPE_SELF)");
        TAP_ASSERT(0, "2. BSS marker reverts after snap_restore");
        TAP_ASSERT(0, "3. heap marker reverts after snap_restore");
        TAP_ASSERT(0, "4. snap_delete after restore");
        tap_done();
        exit(1);
    }
    g_heap_region[0] = (char)0x11;
    g_heap_region[4096] = (char)0x22;  // second page
    g_marker = 0xAAAAAAAAu;

    long h = syscall_snap_create(SNAP_SCOPE_SELF, "rs-self");
    printf("  snap_create = %ld (g_marker=0x%lx, heap[0]=0x%x, heap[4096]=0x%x)\n",
           h, (unsigned long)g_marker,
           (unsigned)(unsigned char)g_heap_region[0],
           (unsigned)(unsigned char)g_heap_region[4096]);
    TAP_ASSERT(h >= 0, "1. snap_create(SCOPE_SELF) returns handle");

    // Mutate BSS + heap.
    g_marker = 0xBBBBBBBBu;
    g_heap_region[0]    = (char)0x33;
    g_heap_region[4096] = (char)0x44;

    // Sanity: confirm mutation took effect (otherwise we'd false-pass).
    if (g_marker != 0xBBBBBBBBu) {
        printf("  pre-restore mutation failed (g_marker=0x%lx)\n",
               (unsigned long)g_marker);
        TAP_ASSERT(0, "2. BSS marker reverts after snap_restore");
        TAP_ASSERT(0, "3. heap marker reverts after snap_restore");
        TAP_ASSERT(0, "4. snap_delete after restore");
        tap_done();
        exit(1);
    }

    long rc_restore = syscall_snap_restore((uint32_t)h);
    printf("  snap_restore = %ld (g_marker=0x%lx, heap[0]=0x%x, heap[4096]=0x%x)\n",
           rc_restore, (unsigned long)g_marker,
           (unsigned)(unsigned char)g_heap_region[0],
           (unsigned)(unsigned char)g_heap_region[4096]);
    TAP_ASSERT(g_marker == 0xAAAAAAAAu,
               "2. BSS marker reverts after snap_restore");
    TAP_ASSERT(g_heap_region[0]    == (char)0x11 &&
               g_heap_region[4096] == (char)0x22,
               "3. heap markers revert after snap_restore");

    long rc_del = syscall_snap_delete((uint32_t)h);
    TAP_ASSERT(rc_del == 0, "4. snap_delete after restore returns 0");

    free(g_heap_region);
    g_heap_region = NULL;

    tap_done();
    exit(0);
}
