#!/usr/bin/env bash
# scripts/run_soak.sh — Phase 28 Session G.2 soak harness.
#
# Purpose: iterate the gate (scripts/run_tests.sh) over TARGET_DURATION_SEC
# seconds of wall-clock time, optionally cycling kernel fault injection
# every FAIL_INJECT_CYCLE iterations.  The soak passes only if every
# iteration finishes without a kernel panic.  Test-level failures are
# tolerated (and counted) — they are how the harness *catches* bugs the
# injected faults expose.
#
# Env vars:
#   TARGET_DURATION_SEC      wall budget in seconds          (default 1800 = 30 min)
#   FAIL_INJECT_CYCLE        inject once per N iterations    (default 10)
#   GRAHAOS_SOAK_FAULT_INJECT 0/1 — enable injection         (default 0)
#   GRAHAOS_SOAK_LOG_DIR     where to drop per-iter logs     (default /tmp/grahaos_soak_<ts>/)
#
# Exit codes:
#   0   — soak completed cleanly (kernel never panicked; test failures
#         tolerated when injection was active)
#   1   — at least one iteration hit a kernel panic / harness crash
#   2   — harness internal error (missing binaries, bad config)
#
# How fault injection works in v1:
#   We write the next iter's planned injection (kind + value) into
#   etc/soak_inject.conf BEFORE rebuilding the ISO.  The kernel ships
#   etc/ into initrd, so user/tests/soak_inject.tap reads the file at
#   gate-start and applies the values via DEBUG_INJECT_* subops.  This
#   keeps the kernel/cmdline parser untouched.
#
#   If soak_inject.conf is absent the gate behaves like a normal `make
#   test` run — useful as a no-injection baseline.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

TARGET_DURATION_SEC="${TARGET_DURATION_SEC:-1800}"
FAIL_INJECT_CYCLE="${FAIL_INJECT_CYCLE:-10}"
ENABLE_INJECT="${GRAHAOS_SOAK_FAULT_INJECT:-0}"
LOG_DIR="${GRAHAOS_SOAK_LOG_DIR:-/tmp/grahaos_soak_$(date +%s)}"

mkdir -p "$LOG_DIR"

echo "run_soak: target=${TARGET_DURATION_SEC}s cycle=${FAIL_INJECT_CYCLE} inject=${ENABLE_INJECT}"
echo "run_soak: log_dir=$LOG_DIR"

start=$(date +%s)
iter=0
n_pass=0
n_test_fail=0
n_panic=0

# Helper: pick a random injection.  Echoes "kind value" on stdout.
pick_inject() {
    local kind_idx=$((RANDOM % 4))
    case $kind_idx in
        0) echo "pmm $((RANDOM % 40 + 20))" ;;       # countdown 20..59
        1) echo "kmalloc $((RANDOM % 40 + 20))" ;;   # countdown 20..59
        2) echo "chan_rate $((RANDOM % 5 + 1))" ;;   # rate 1..5  (sample at 1/256)
        3) echo "spin_rate $((RANDOM % 5 + 1))" ;;   # rate 1..5  (observe-only)
    esac
}

# Generate or clear etc/soak_inject.conf for the upcoming iter.
update_inject_conf() {
    local kind="$1"
    local val="$2"
    if [ -n "$kind" ]; then
        printf '%s=%s\n' "$kind" "$val" > "$REPO_ROOT/etc/soak_inject.conf"
    else
        rm -f "$REPO_ROOT/etc/soak_inject.conf"
    fi
}

# Ensure the conf file is removed on exit so subsequent `make test`
# runs are noise-free.
cleanup() {
    rm -f "$REPO_ROOT/etc/soak_inject.conf"
    echo "run_soak: cleanup done"
}
trap cleanup EXIT

while :; do
    now=$(date +%s)
    elapsed=$((now - start))
    if [ "$elapsed" -ge "$TARGET_DURATION_SEC" ]; then
        break
    fi
    iter=$((iter + 1))

    # Decide injection for this iter.
    inject_kind=""
    inject_val=""
    if [ "$ENABLE_INJECT" = "1" ] && \
       [ "$iter" -gt 0 ] && \
       [ $((iter % FAIL_INJECT_CYCLE)) -eq 0 ]; then
        read -r inject_kind inject_val < <(pick_inject)
        echo "run_soak: iter=$iter inject=${inject_kind}=${inject_val}"
    else
        echo "run_soak: iter=$iter (no injection)"
    fi
    update_inject_conf "$inject_kind" "$inject_val"

    iter_log="$LOG_DIR/iter_$(printf '%04d' "$iter").log"
    set +e
    bash "$REPO_ROOT/scripts/run_tests.sh" > "$iter_log" 2>&1
    rc=$?
    set -e

    # Classify outcome.
    if [ "$rc" -eq 0 ]; then
        n_pass=$((n_pass + 1))
    elif [ "$rc" -eq 1 ]; then
        # Gate parser reported test failures but harness completed.
        # With injection on, this is expected and tolerated.
        n_test_fail=$((n_test_fail + 1))
        if [ "$ENABLE_INJECT" != "1" ]; then
            echo "run_soak: iter=$iter FAIL (test failures, no injection) — $iter_log"
            n_panic=$((n_panic + 1))
            break
        fi
    else
        # rc=2 from run_tests.sh => harness/QEMU/kernel crashed.
        echo "run_soak: iter=$iter PANIC (rc=$rc) — $iter_log"
        n_panic=$((n_panic + 1))
        break
    fi
done

end=$(date +%s)
total=$((end - start))
mins=$((total / 60))
secs=$((total % 60))

echo "run_soak: done — ${iter} iters in ${mins}m${secs}s"
echo "run_soak: clean=$n_pass test_fail_injected=$n_test_fail panic=$n_panic"
echo "run_soak: log_dir=$LOG_DIR"

# Aggregate JSON summary so a closeout script can scrape it.
cat > "$LOG_DIR/soak_summary.json" <<EOF
{
  "target_duration_sec": $TARGET_DURATION_SEC,
  "actual_duration_sec": $total,
  "iters": $iter,
  "iters_clean": $n_pass,
  "iters_test_fail_injected": $n_test_fail,
  "iters_panic": $n_panic,
  "fault_inject_enabled": $ENABLE_INJECT,
  "fault_inject_cycle": $FAIL_INJECT_CYCLE,
  "log_dir": "$LOG_DIR"
}
EOF

if [ "$n_panic" -gt 0 ]; then
    exit 1
fi
exit 0
