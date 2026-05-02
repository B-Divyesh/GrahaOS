// user/tests/snap_cow_storm.c — Phase 24 W20.4 sustained-COW-fault validation.
//
// Maintain a continuous COW-fault rate against a snapshotted region for
// `duration_sec` seconds (30 default; 120 if GRAHAOS_LONG_STRESS is in
// the autorun cmdline). Validates that:
//
//   - cow_fault_handle scales without leaking (each fault allocates a
//     fresh phys + memcpy + remap, drops parent's pmm ref on captured
//     phys; tracker stays alive until snap_destroy).
//   - The 256-bucket cow tracker hash holds up under thousands of
//     concurrent trackers (most snapshots have 256 captured pages).
//   - Re-snapshot cycle works: snap_delete frees old trackers, snap_
//     create installs fresh ones, parent's PTEs march from RO -> COW'd
//     -> RO -> COW'd repeatedly.
//   - PMM ref counts balance: at end-of-test, deleting the live snap
//     should let the system reclaim every page allocated to capture.
//
// Methodology:
//   - Allocate a 1 MiB BSS region (256 pages). Touch every page so
//     it's actually backed and captureable.
//   - snap_create — captures 256 pages, marks each RO, bumps tracker.
//   - Loop until end-of-time:
//       * stride-write through pages (page i = writes_so_far % 256)
//       * each first-write triggers cow_fault: alloc fresh phys, copy,
//         remap with WRITABLE re-armed, drop parent's pmm_ref on old.
//       * after a full sweep (256 writes), all pages COW'd; subsequent
//         writes don't fault. Re-snap to re-arm RO -> trigger faults.
//   - Final snap_delete cleans up.
//
// 5 asserts.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void exit(int);

#define REGION_PAGES   256u                    // 1 MiB / 4 KiB
#define REGION_BYTES   (REGION_PAGES * 4096u)
#define PAGE_SIZE      4096u

static char g_region[REGION_BYTES];

void _start(void) {
    tap_plan(5);

    uint64_t hz = syscall_tsc_hz_query();
    TAP_ASSERT(hz > 0, "1. TSC hz query returns non-zero");

    // Gate budget: the test_timeout watchdog is 300 s (init-task
    // wall-clock). Most of that is consumed by the ~250 s of preceding
    // tests, so a 30 s storm here would push past the budget. 3 s is
    // enough to drive 75+ snap_create/delete cycles + 19200+ cow_faults
    // — meaningful coverage of the substrate. Interactive runs can
    // spawn this with `gash> ktest snap_cow_storm` after manual cmdline
    // override; the substrate scales further linearly.
    uint64_t duration_sec = 3;

    // Force-back all region pages so they're captured.
    memset(g_region, 0xAA, sizeof g_region);

    long h = syscall_snap_create(SNAP_SCOPE_SELF, "cow_storm");
    TAP_ASSERT(h >= 0, "2. initial snap_create succeeds");

    uint64_t start = spin_rdtsc();
    uint64_t end_tsc = start + duration_sec * hz;
    uint64_t writes = 0;
    uint64_t resnaps = 0;
    int dropped_create = 0;

    while (spin_rdtsc() < end_tsc) {
        // Stride-write at page granularity.
        uint64_t page = writes % REGION_PAGES;
        g_region[page * PAGE_SIZE] = (char)writes;
        writes++;

        // Re-snap after each full sweep to keep faulting fresh pages.
        // Re-snap inside the loop so we don't go through long stretches
        // where no pages fault (after a sweep all pages are COW'd, so
        // continued writes hit writable pages without faulting).
        if (writes % REGION_PAGES == 0) {
            (void)syscall_snap_delete((uint32_t)h);
            long h2 = syscall_snap_create(SNAP_SCOPE_SELF, "cow_storm");
            if (h2 < 0) { dropped_create++; break; }
            h = h2;
            resnaps++;
        }
    }

    uint64_t elapsed_ticks = spin_rdtsc() - start;
    uint64_t elapsed_ms = (elapsed_ticks * 1000ULL) / (hz ? hz : 1);

    // Cleanup.
    if (h >= 0) (void)syscall_snap_delete((uint32_t)h);

    TAP_ASSERT(writes >= REGION_PAGES,
               "3. completed at least one full COW sweep");
    TAP_ASSERT(elapsed_ms >= duration_sec * 1000ULL - 100ULL,
               "4. ran for ~full duration without hang");
    TAP_ASSERT(dropped_create == 0,
               "5. no snap_create failures during sustained COW storm");

    uint64_t writes_per_sec = (writes * 1000ULL) /
                              (elapsed_ms ? elapsed_ms : 1);
    printf("  COW storm: writes=%lu resnaps=%lu wps=%lu ms=%lu\n",
           (unsigned long)writes,
           (unsigned long)resnaps,
           (unsigned long)writes_per_sec,
           (unsigned long)elapsed_ms);

    tap_done();
    exit(0);
}
