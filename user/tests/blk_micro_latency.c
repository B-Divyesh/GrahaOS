// user/tests/blk_micro_latency.c — Phase 24a measurement ruler.
//
// Profiling tool, NOT a gate assertion. Measures end-to-end open()→read(4 KiB)
// →close() round-trip latency, prints min/mean/p50/p99/max in TSC ticks AND
// micro-seconds (assuming TSC ≈ 2 GHz in QEMU TCG; calibrate if landing on
// real hardware).
//
// Used as the per-layer measurement gate after W1, W2, W3, W5, W6, W7, W8 in
// /home/atman/.claude/plans/scalable-stirring-gem.md. Each layer's commit is
// followed by `gash> ktest blk_micro_latency` interactively to verify the
// expected latency drop. Persists nothing on disk; capture serial output to
// problems/phase24a/baselines.json by hand.
//
// Built and copied to bin/tests/ but NOT in the gate manifest — at the
// pre-W1 baseline (~440 ms/op), 100 iters takes ~44 seconds and the gate
// budget is 90s wall-clock. Run interactively from `gash> ktest
// blk_micro_latency`.
//
// Has 1 trivial TAP assertion ("at least one sample completed") so it works
// under the existing libtap harness; the value is in the printf output.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);

#define LAT_SAMPLES 100u
#define LAT_BUF_SZ  4096u

static uint8_t  s_buf[LAT_BUF_SZ];
static uint64_t s_lat[LAT_SAMPLES];

static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Insertion sort small array (n <= 100).
static void sort_ascending(uint64_t *arr, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint64_t v = arr[i];
        uint32_t j = i;
        while (j > 0 && arr[j - 1] > v) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = v;
    }
}

void _start(void) {
    tap_plan(1);

    printf("[blk_micro_latency] %u-sample 4 KiB open->read->close benchmark\n",
           LAT_SAMPLES);

    // Warm-up: cover any first-touch path-resolution / cache-miss overhead
    // that's not representative of steady-state.
    int wfd = syscall_open("/etc/gcp.json");
    if (wfd < 0) {
        printf("[blk_micro_latency] open /etc/gcp.json failed rc=%d\n", wfd);
        TAP_ASSERT(0, "1. /etc/gcp.json must be present");
        tap_done();
        syscall_exit(1);
    }
    (void)syscall_read(wfd, s_buf, LAT_BUF_SZ);
    syscall_close(wfd);

    // Measurement loop. Re-open per iteration to include path-resolution +
    // VFS lookup cost in the per-op number — that's the realistic cost any
    // FS call from user-space pays.
    uint32_t valid = 0;
    for (uint32_t i = 0; i < LAT_SAMPLES; i++) {
        uint64_t t0 = rdtsc_now();
        int f = syscall_open("/etc/gcp.json");
        if (f < 0) continue;
        ssize_t r = syscall_read(f, s_buf, LAT_BUF_SZ);
        syscall_close(f);
        uint64_t t1 = rdtsc_now();
        if (r > 0) s_lat[valid++] = t1 - t0;
    }

    if (valid == 0) {
        printf("[blk_micro_latency] no valid samples\n");
        TAP_ASSERT(0, "1. at least one sample must complete");
        tap_done();
        syscall_exit(1);
    }

    sort_ascending(s_lat, valid);
    uint64_t min_t = s_lat[0];
    uint64_t p50_t = s_lat[(valid * 50) / 100];
    uint64_t p99_t = s_lat[(valid * 99) / 100];
    uint64_t max_t = s_lat[valid - 1];
    uint64_t sum_t = 0;
    for (uint32_t i = 0; i < valid; i++) sum_t += s_lat[i];
    uint64_t mean_t = sum_t / valid;

    // TSC -> µs assuming ~2 GHz TCG host. The ratio is what matters between
    // measurement gates; absolute value is approximate.
    const uint64_t TSC_PER_US = 2000ull;

    printf("[blk_micro_latency] samples=%u/%u\n", valid, LAT_SAMPLES);
    printf("[blk_micro_latency] TSC_ticks: min=%llu p50=%llu mean=%llu p99=%llu max=%llu\n",
           (unsigned long long)min_t,
           (unsigned long long)p50_t,
           (unsigned long long)mean_t,
           (unsigned long long)p99_t,
           (unsigned long long)max_t);
    printf("[blk_micro_latency] microseconds (TSC=2GHz approx): min=%llu p50=%llu mean=%llu p99=%llu max=%llu\n",
           (unsigned long long)(min_t  / TSC_PER_US),
           (unsigned long long)(p50_t  / TSC_PER_US),
           (unsigned long long)(mean_t / TSC_PER_US),
           (unsigned long long)(p99_t  / TSC_PER_US),
           (unsigned long long)(max_t  / TSC_PER_US));

    TAP_ASSERT(valid > 0, "1. measurement completed");
    tap_done();
    syscall_exit(0);
}
