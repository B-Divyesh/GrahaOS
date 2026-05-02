// user/tests/snap_stress_cycle.c — Phase 24 W20.3 stress validation.
//
// Run 10000 iterations of snap_create + COW-fault-on-write + snap_delete
// to validate that the W14 capture / W15 cow_fault / W17 destroy paths
// don't leak slab memory, page frames, or cap_objects under sustained
// load. Each iteration:
//
//   1. snap_create(SNAP_SCOPE_SELF) — captures the test process's
//      user-half PTEs, bumps cow_page_tracker for each, marks RO.
//   2. Write a stack variable + a BSS-region byte. The first write to
//      each captured page triggers cow_fault: allocates a fresh phys
//      page, memcpy + remap, drops parent's pmm ref on captured phys.
//   3. snap_delete — drops cow_page_tracker refs (frees trackers when
//      refcount hits 0), pmm_page_unref on captured phys (frees old
//      phys when refcount hits 0). v1 closeout fix: also restores
//      PTE_WRITABLE on captured pages that haven't been COW'd, so
//      subsequent writes don't fault into a missing tracker.
//
// We use SCOPE_SELF (not SCOPE_GLOBAL) because SCOPE_GLOBAL needs
// CAP_KIND_SYSTEM which ktest doesn't hold. The caller-restore for
// SCOPE_SELF is a documented no-op v1 limitation (Phase 25 will add
// syscall_set_user_frame to swizzle the caller's syscall_frame); but
// create + delete + COW-fault still exercise the substrate fully.
//
// Validation:
//   - All 10000 cycles complete with rc=0
//   - No errors during stress
//   - kheap delta < 256 KiB (per-CPU magazine residency upper bound:
//     ~30 caches × 4 CPUs × 8 objects × 256 B avg = ~245 KiB)
//   - Final snap_list reports zero live snapshots
//
// 6 asserts.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern int  memcmp(const void *, const void *, size_t);
extern void exit(int);

// 16 captured pages of BSS. Static so it lands in .bss and is captured
// by snap_walk_user_half (user-half PTEs).
static char heap_region[64 * 1024];

void _start(void) {
    tap_plan(6);

    uint64_t hz = syscall_tsc_hz_query();
    TAP_ASSERT(hz > 0, "1. TSC hz query returns non-zero");

    // Snapshot kheap baseline.
    kheap_stats_entry_u_t base[64];
    int base_n = syscall_kheap_stats(base, 64);
    TAP_ASSERT(base_n > 0, "2. kheap stats baseline available");

    // Force-back the heap_region pages so they're captured (touched
    // before any snap_create so demand-paging doesn't interfere).
    memset(heap_region, 0xCD, sizeof heap_region);

    uint64_t start = spin_rdtsc();
    int oks = 0;
    int errs = 0;

    for (int i = 0; i < 10000; i++) {
        long h = syscall_snap_create(SNAP_SCOPE_SELF, NULL);
        if (h < 0) { errs++; continue; }

        // Trigger COW: stack write + heap_region write. Each first-write
        // to a captured page faults; cow_fault allocates a fresh phys.
        volatile char sv = (char)(i & 0xFF);
        (void)sv;
        heap_region[i % (int)sizeof heap_region] = (char)i;

        long rc = syscall_snap_delete((uint32_t)h);
        if (rc != 0) { errs++; continue; }
        oks++;
    }

    uint64_t elapsed_ticks = spin_rdtsc() - start;
    uint64_t elapsed_ms = (elapsed_ticks * 1000ULL) / (hz ? hz : 1);

    TAP_ASSERT(oks == 10000, "3. all 10000 cycles succeeded");
    TAP_ASSERT(errs == 0, "4. zero errors during stress");

    // Snapshot kheap after stress and compute delta.
    kheap_stats_entry_u_t final[64];
    int final_n = syscall_kheap_stats(final, 64);
    int64_t total_delta = 0;
    for (int i = 0; i < final_n; i++) {
        for (int j = 0; j < base_n; j++) {
            if (memcmp(final[i].name, base[j].name, 32) == 0) {
                int64_t d = (int64_t)final[i].in_use - (int64_t)base[j].in_use;
                total_delta += d * (int64_t)final[i].object_size;
                break;
            }
        }
    }
    if (total_delta < 0) total_delta = -total_delta;

    // Allow up to 256 KiB residual (per-CPU magazine residency).
    TAP_ASSERT(total_delta < 256 * 1024,
               "5. kheap delta < 256 KiB after 10K cycles");

    // Verify all snapshots are gone.
    snap_info_user_t list[8];
    long n = syscall_snap_list(list, 8);
    TAP_ASSERT(n == 0, "6. zero live snapshots after stress");

    printf("  10000 cycles: oks=%d errs=%d kheap_delta=%ld bytes elapsed=%lu ms\n",
           oks, errs, (long)total_delta, (unsigned long)elapsed_ms);

    tap_done();
    exit(0);
}
