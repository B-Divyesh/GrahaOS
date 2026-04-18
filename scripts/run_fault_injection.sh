#!/usr/bin/env bash
# scripts/run_fault_injection.sh — Phase 13 fault-injection gate test.
#
# Exercises kernel cmdline knobs that simulate hostile boot conditions
# the spec's `fault_injection_tests` section calls out:
#
#   inject_klog_preinit=N   — N klog calls before klog_init runs
#                              (bumps g_early_drops, kernel surfaces a
#                              retrospective "dropped N" entry).
#
#   inject_ring_wrap=N      — N klog calls right after klog_init
#                              (forces ring head to wrap, so seq gaps
#                              should appear in subsequent reads).
#
# Each scenario is a separate QEMU boot. The script greps the serial
# log for the expected breadcrumb. Exits 0 if both pass, non-zero if
# either misses its evidence.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LIMINE_CONF="limine.conf"
LIMINE_BACKUP="$(mktemp)"
cp "$LIMINE_CONF" "$LIMINE_BACKUP"
cleanup() {
    cp "$LIMINE_BACKUP" "$LIMINE_CONF"
    rm -f "$LIMINE_BACKUP"
}
trap cleanup EXIT

# Helper: rebuild ISO with a custom cmdline and boot QEMU.
run_qemu_with_cmdline() {
    local cmdline="$1"
    local logfile="$2"
    awk '!/^cmdline:/' "$LIMINE_BACKUP" > "$LIMINE_CONF"
    printf '\ncmdline: %s\n' "$cmdline" >> "$LIMINE_CONF"
    rm -f "$logfile"

    if ! make -s all >/dev/null 2>&1; then
        echo "fault_injection: build failed for cmdline='$cmdline'" >&2
        return 2
    fi

    set +e
    timeout 60s qemu-system-x86_64 \
        -cdrom grahaos.iso \
        -m 512M -smp 4 \
        -drive file=disk.img,format=raw,if=none,id=mydisk \
        -device ich9-ahci,id=ahci \
        -device ide-hd,drive=mydisk,bus=ahci.0 \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -chardev "file,id=serial0,path=$logfile,signal=off" \
        -serial chardev:serial0 \
        -display none \
        -no-reboot \
        </dev/null >/dev/null 2>&1
    local ec=$?
    set -e
    return "$ec"
}

FAILS=0

# -------- Scenario 1: pre-init drops --------------------------------------
PREINIT_LOG="/tmp/grahaos_preinit.log"
run_qemu_with_cmdline \
    "autorun=ktest quiet=1 test_timeout_seconds=90 inject_klog_preinit=50" \
    "$PREINIT_LOG" || true

# After the Phase 13 sweep, every former serial_write before klog_init
# is also a klog call — those add a handful of additional pre-init
# drops on top of our injected 50. The retrospective entry shows the
# combined count, so check for "dropped >= 50" rather than exactly 50.
DROP_LINE=$(grep -oE "dropped [0-9]+ early-boot messages" "$PREINIT_LOG" | head -1 || true)
DROP_N=$(echo "$DROP_LINE" | grep -oE '[0-9]+' | head -1)
if [ -n "$DROP_N" ] && [ "$DROP_N" -ge 50 ]; then
    echo "fault_injection: PASS preinit ($DROP_N drops surfaced, expected ≥50)"
else
    echo "fault_injection: FAIL preinit — got '$DROP_LINE' in $PREINIT_LOG"
    FAILS=$((FAILS + 1))
fi

# -------- Scenario 2: ring wrap -------------------------------------------
WRAP_LOG="/tmp/grahaos_ringwrap.log"
# 20000 > 16384 ring slots → head wraps once. Look for a klog mirror
# line whose seq column comes from the ring-wrap probe series. The
# kernel keeps emitting our probes via mirror_to_serial.
run_qemu_with_cmdline \
    "autorun=ktest quiet=1 test_timeout_seconds=120 inject_ring_wrap=20000" \
    "$WRAP_LOG" || true

# Each probe goes to TEST subsystem with prefix "ring-wrap probe ".
# Count how many of our probes the mirror saw — should be ~all 20000.
PROBE_COUNT=$(grep -cE "TEST +ring-wrap probe " "$WRAP_LOG" || true)
if [ "$PROBE_COUNT" -ge 20000 ]; then
    echo "fault_injection: PASS ringwrap ($PROBE_COUNT probes mirrored)"
else
    echo "fault_injection: FAIL ringwrap — only saw $PROBE_COUNT/20000 probes"
    FAILS=$((FAILS + 1))
fi

if [ "$FAILS" -eq 0 ]; then
    echo "fault_injection: ALL OK"
    exit 0
fi
echo "fault_injection: $FAILS scenario(s) failed"
exit 1
