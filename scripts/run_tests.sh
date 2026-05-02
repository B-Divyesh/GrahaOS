#!/usr/bin/env bash
# scripts/run_tests.sh — Phase 12 test harness orchestrator.
#
# Workflow:
#   1. Build a test ISO whose limine.conf carries `cmdline: autorun=ktest
#      quiet=1 test_timeout_seconds=300` (substituted in-place, restored
#      on exit).  Phase 24a path (A): timeout bumped from 90→300 s to
#      accommodate channel-mode FS I/O at ~100 ms/op (TCG AHCI emulation
#      floor; see project memory feedback_phase24a_tcg_ahci_floor.md).
#   2. Boot that ISO headless in QEMU with serial captured to a log.
#   3. Wait for the kernel to shut itself down (ACPI after PID-1 exit)
#      or for the outer `timeout 360s` wrapper to fire.
#   4. Run scripts/parse_tap.py on the captured log; write
#      /tmp/grahaos_tests/summary.json and propagate the parser's exit
#      code to the caller.
#
# Invoked by `make test`. Exit codes:
#   0   — all gate tests passed
#   1   — one or more test failures
#   2   — harness internal error (build failed, QEMU didn't start,
#          log missing, ...)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LOG_DIR="${GRAHAOS_TEST_LOG_DIR:-/tmp/grahaos_tests}"
SERIAL_LOG="${GRAHAOS_SERIAL_LOG:-/tmp/grahaos_test.log}"
SUMMARY_JSON="$LOG_DIR/summary.json"
MESON_FLAGS="${GRAHAOS_TEST_MESON_FLAGS:-autorun=ktest quiet=1 test_timeout_seconds=400}"
QEMU_WALL_TIMEOUT_SEC="${GRAHAOS_TEST_WALL_TIMEOUT_SEC:-480}"

mkdir -p "$LOG_DIR"
rm -f "$SERIAL_LOG" "$SUMMARY_JSON"

# --- 0. Fresh disk --------------------------------------------------------
# The ported tests (simtest, clustertest, fdtest, etc.) were written
# assuming a freshly-formatted GrahaFS disk. Running them in sequence
# against a persistent disk causes order-dependent failures (e.g.
# clustertest's "new cluster created" check fails when simtest already
# populated the cluster table). Reformatting before every `make test`
# run gives each test the clean-slate view it was designed for.
if [ -f disk.img ]; then
    echo "run_tests: reformatting disk.img for a clean test run"
    make -s reformat >/dev/null 2>&1 || {
        echo "run_tests: FAIL — disk reformat failed" >&2
        exit 2
    }
fi

# --- 1. Build test ISO ---------------------------------------------------

LIMINE_CONF="limine.conf"
LIMINE_BACKUP="$(mktemp)"
cp "$LIMINE_CONF" "$LIMINE_BACKUP"
cleanup() {
    cp "$LIMINE_BACKUP" "$LIMINE_CONF"
    rm -f "$LIMINE_BACKUP"
}
trap cleanup EXIT

# Inject the autorun cmdline, rebuild.
# Purge any prior cmdline: line and append a fresh one.
awk '!/^cmdline:/' "$LIMINE_BACKUP" > "$LIMINE_CONF"
printf '\ncmdline: %s\n' "$MESON_FLAGS" >> "$LIMINE_CONF"

echo "run_tests: building test ISO with cmdline: $MESON_FLAGS"
if ! make -s all >/dev/null 2>&1; then
    echo "run_tests: FAIL — build failed. Running verbose rebuild for diagnostics:" >&2
    make all >&2
    exit 2
fi

# --- 2. Boot QEMU headless -----------------------------------------------

START_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
START_EPOCH="$(date +%s)"

echo "run_tests: launching QEMU (wall timeout ${QEMU_WALL_TIMEOUT_SEC}s)"

# -no-reboot lets a triple-fault fallback exit cleanly; we do NOT pass
# -no-shutdown (which would hang QEMU in a paused state after our ACPI
# shutdown succeeds). Display is disabled; serial goes to a file.
#
# Host-CPU regulation (Phase 23 closeout — host PC was crashing/freezing
# under unconstrained TCG):
# - `taskset -c "$QEMU_CPUS"` HARD-LIMITS QEMU to a fixed set of host
#   cores (default cores 0..3 = 4 of 24).  QEMU literally cannot use
#   the other 20 cores; foreground host work (browser, IDE) on those
#   cores is uncontested.  This is a kernel-level affinity, far stronger
#   than nice priority alone.
# - `nice -n 5` (mild) keeps the 4 dedicated cores responsive to host
#   foreground work without slowing tests below the 300 s test_timeout
#   watchdog (nice -n 10 caused gate INCOMPLETEs under host load).
# - `ionice -c 2 -n 5` (best-effort, low-priority class) for I/O.
# - Phase 24 sub-phase H: KVM enabled. The TCG-calibrated busy-waits
#   (`spin_ms_approx`) have been replaced with TSC-calibrated `spin_us`
#   in user/syscalls.h, so the gate tests (e1000dtest, ahcid_*,
#   userdrv_respawn_*) wait the same wall-time on TCG and KVM. The
#   spin_ms_approx compat helper in syscalls.h preserves the original
#   ~400 µs/ms-unit behaviour to keep gate budgets unchanged.
QEMU_CPUS="${GRAHAOS_TEST_QEMU_CPUS:-0-3}"

# GRAHAOS_TEST_NO_KVM=1 to fall back to TCG (e.g., on hosts without KVM
# support). Default: KVM if /dev/kvm is accessible.
QEMU_KVM_FLAGS=""
if [ -z "${GRAHAOS_TEST_NO_KVM:-}" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_KVM_FLAGS="-enable-kvm -cpu host"
fi

set +e
taskset -c "$QEMU_CPUS" nice -n 5 ionice -c 2 -n 5 \
    timeout "${QEMU_WALL_TIMEOUT_SEC}s" qemu-system-x86_64 \
    $QEMU_KVM_FLAGS \
    -cdrom grahaos.iso \
    -m 512M -smp 4 \
    -drive file=disk.img,format=raw,if=none,id=mydisk \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=mydisk,bus=ahci.0 \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -chardev "file,id=serial0,path=$SERIAL_LOG,signal=off" \
    -serial chardev:serial0 \
    -display none \
    -no-reboot \
    </dev/null >/dev/null 2>&1
QEMU_EXIT=$?
set -e

END_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
END_EPOCH="$(date +%s)"
DURATION=$((END_EPOCH - START_EPOCH))
echo "run_tests: QEMU exited code=$QEMU_EXIT duration=${DURATION}s"

if [ ! -s "$SERIAL_LOG" ]; then
    echo "run_tests: FAIL — serial log is empty ($SERIAL_LOG)" >&2
    exit 2
fi

# --- 3. Parse TAP --------------------------------------------------------

python3 "$REPO_ROOT/scripts/parse_tap.py" \
    "$SERIAL_LOG" "$SUMMARY_JSON" \
    --started-utc "$START_UTC" \
    --ended-utc   "$END_UTC" \
    --duration-seconds "$DURATION" \
    --repo-root "$REPO_ROOT"
PARSER_EXIT=$?

echo "run_tests: summary at $SUMMARY_JSON (parser exit=$PARSER_EXIT)"
exit "$PARSER_EXIT"
