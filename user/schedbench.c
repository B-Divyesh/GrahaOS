// user/schedbench.c
//
// Phase 20 U16 — scheduler benchmark.
//
// Spawns N worker processes (each running /bin/schedbench_worker, a tight
// compute loop), samples SYS_GET_SYSTEM_STATE every 100 ms for a chosen
// duration, and reports:
//
//   * max_runq_depth / min_runq_depth across all samples
//   * balance_ratio = max / min  (ideal SMP: ~1.0; BSP-only: N/A)
//   * total context switches during the window
//   * total steal_successes / steal_failures on peer runqs
//
// p99 wakeup latency uses the TSC fields exposed in state_system_t
// (tsc_ns_now + tsc_hz, populated since Phase 20). Each sample interval
// we record the elapsed ns between two SYS_GET_SYSTEM_STATE calls and
// the wall-clock delta we asked for; the difference is overhead +
// wake-path latency. Histogram tracks p99 across the run.
//
// CLI:
//   schedbench [--n=N] [--duration=Ts]
//   defaults: N=4, T=3.

#include "syscalls.h"
#include "../kernel/state.h"
#include <stdint.h>
#include <stddef.h>

static void print(const char *s) { while (*s) syscall_putc(*s++); }

static void print_u64(uint64_t v) {
    char buf[32]; int n = 0;
    if (v == 0) { print("0"); return; }
    while (v > 0 && n < 31) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; --i) { char s[2] = { buf[i], 0 }; print(s); }
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int parse_tail_uint(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

// Busy-poll uptime_ticks until `ms` has elapsed. 100 Hz timer gives 10 ms
// resolution — good enough for the 100 ms sample interval.
static void wait_ms(uint32_t ms) {
    state_system_t st;
    syscall_get_system_state(STATE_CAT_SYSTEM, &st, sizeof(st));
    uint64_t start = st.uptime_ticks;
    uint64_t target = start + (uint64_t)(ms / 10u);
    if (target == start) target = start + 1;
    while (1) {
        syscall_get_system_state(STATE_CAT_SYSTEM, &st, sizeof(st));
        if (st.uptime_ticks >= target) break;
    }
}

#define SCHEDBENCH_MAX_WORKERS 64

void _start(int argc, char **argv) {
    int n_workers = 4;
    int duration_s = 3;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (str_starts(a, "--n=")) {
            int v = parse_tail_uint(a + 4);
            if (v > 0 && v <= SCHEDBENCH_MAX_WORKERS) n_workers = v;
        } else if (str_starts(a, "--duration=")) {
            int v = parse_tail_uint(a + 11);
            if (v > 0 && v <= 60) duration_s = v;
        }
    }

    print("schedbench: n=");
    print_u64((uint64_t)n_workers);
    print(" duration=");
    print_u64((uint64_t)duration_s);
    print("s\n");

    // Spawn workers.
    int worker_pids[SCHEDBENCH_MAX_WORKERS];
    int spawned = 0;
    for (int i = 0; i < n_workers; i++) {
        int pid = syscall_spawn("/bin/schedbench_worker");
        if (pid < 0) {
            print("schedbench: spawn failed at i=");
            print_u64((uint64_t)i);
            print(" (errno=");
            print_u64((uint64_t)(-pid));
            print(")\n");
            break;
        }
        worker_pids[spawned++] = pid;
    }
    print("schedbench: spawned=");
    print_u64((uint64_t)spawned);
    print("\n");

    // Baseline counters.
    state_system_t st;
    syscall_get_system_state(STATE_CAT_SYSTEM, &st, sizeof(st));
    uint64_t ctx_start = 0;
    uint64_t steal_ok_start = 0;
    uint64_t steal_fail_start = 0;
    for (uint32_t c = 0; c < st.cpu_entries; c++) {
        ctx_start        += st.cpus[c].ctx_switches;
        steal_ok_start   += st.cpus[c].steal_successes;
        steal_fail_start += st.cpus[c].steal_failures;
    }

    // Sample loop.
    int total_samples = duration_s * 10;  // 10 samples/sec
    uint64_t max_depth = 0;
    uint64_t min_depth_sum = 0;
    uint64_t max_depth_sum = 0;
    uint32_t min_depth_any = 0xFFFFFFFFu;
    uint32_t valid_samples = 0;

    // p99 wakeup overhead: histogram of (tsc_ns_after - tsc_ns_before -
    // 100ms_target) per sample. Captures kernel-mode time + dispatch
    // overhead the syscall round-trip incurred. 256 buckets, 1us each.
    #define HIST_BUCKETS 256u
    uint32_t hist[HIST_BUCKETS] = {0};
    uint32_t hist_total = 0;

    for (int s = 0; s < total_samples; s++) {
        uint64_t tsc_before = st.tsc_ns_now;  // from previous sample
        wait_ms(100);
        syscall_get_system_state(STATE_CAT_SYSTEM, &st, sizeof(st));
        if (tsc_before > 0 && st.tsc_ns_now > tsc_before) {
            uint64_t elapsed_ns = st.tsc_ns_now - tsc_before;
            if (elapsed_ns > 100000000ULL) {
                uint64_t over_us = (elapsed_ns - 100000000ULL) / 1000ULL;
                uint32_t bucket = over_us < HIST_BUCKETS ? (uint32_t)over_us
                                                         : HIST_BUCKETS - 1;
                hist[bucket]++;
                hist_total++;
            }
        }
        uint32_t sample_max = 0;
        uint32_t sample_min = 0xFFFFFFFFu;
        uint32_t active_cpus = 0;
        for (uint32_t c = 0; c < st.cpu_entries; c++) {
            if (!st.cpus[c].active) continue;
            active_cpus++;
            uint32_t d = st.cpus[c].runq_depth;
            if (d > sample_max) sample_max = d;
            if (d < sample_min) sample_min = d;
        }
        if (active_cpus == 0) continue;
        if (active_cpus == 1) {
            // BSP-only: balance ratio is meaningless. Still record depth.
            if (sample_max > max_depth) max_depth = sample_max;
            continue;
        }
        max_depth_sum += sample_max;
        min_depth_sum += sample_min;
        if (sample_min < min_depth_any) min_depth_any = sample_min;
        if ((uint64_t)sample_max > max_depth) max_depth = sample_max;
        valid_samples++;
    }

    // Final counters.
    syscall_get_system_state(STATE_CAT_SYSTEM, &st, sizeof(st));
    uint64_t ctx_end = 0;
    uint64_t steal_ok_end = 0;
    uint64_t steal_fail_end = 0;
    for (uint32_t c = 0; c < st.cpu_entries; c++) {
        ctx_end        += st.cpus[c].ctx_switches;
        steal_ok_end   += st.cpus[c].steal_successes;
        steal_fail_end += st.cpus[c].steal_failures;
    }

    // Teardown — send SIGKILL, reap.
    for (int i = 0; i < spawned; i++) {
        syscall_kill(worker_pids[i], 2);  // SIGKILL = 2
    }
    for (int i = 0; i < spawned; i++) {
        int wst = 0;
        syscall_wait(&wst);
    }

    // Report.
    print("schedbench: context_switches=");
    print_u64(ctx_end - ctx_start);
    print("\n");
    print("schedbench: steal_successes=");
    print_u64(steal_ok_end - steal_ok_start);
    print(" steal_failures=");
    print_u64(steal_fail_end - steal_fail_start);
    print("\n");
    print("schedbench: max_runq_depth=");
    print_u64(max_depth);
    print("\n");
    if (valid_samples > 0) {
        uint64_t avg_max = max_depth_sum / valid_samples;
        uint64_t avg_min = min_depth_sum / valid_samples;
        print("schedbench: avg_max_depth=");
        print_u64(avg_max);
        print(" avg_min_depth=");
        print_u64(avg_min);
        print("\n");
        if (avg_min > 0) {
            // Express as a X.YY decimal without floating point.
            uint64_t ratio_hundredths = (avg_max * 100) / avg_min;
            print("schedbench: balance_ratio=");
            print_u64(ratio_hundredths / 100);
            print(".");
            uint64_t frac = ratio_hundredths % 100;
            if (frac < 10) print("0");
            print_u64(frac);
            print("\n");
        } else {
            print("schedbench: balance_ratio=inf (min_depth=0)\n");
        }
    } else {
        print("schedbench: balance_ratio=N/A (single-CPU; APs parked)\n");
    }
    // p99 wakeup overhead from histogram. Walk back from the highest
    // bucket until we've covered the top 1% of samples.
    if (hist_total > 0) {
        uint32_t threshold = hist_total - (hist_total / 100u);
        uint32_t cumulative = 0;
        uint32_t p99_us = 0;
        for (uint32_t i = 0; i < HIST_BUCKETS; i++) {
            cumulative += hist[i];
            if (cumulative >= threshold) { p99_us = i; break; }
        }
        print("schedbench: p99_wakeup_overhead_us=");
        print_u64((uint64_t)p99_us);
        print(" (samples=");
        print_u64((uint64_t)hist_total);
        print(")\n");
    } else {
        print("schedbench: p99_wakeup_us=N/A (no TSC samples)\n");
    }

    syscall_exit(0);
}
