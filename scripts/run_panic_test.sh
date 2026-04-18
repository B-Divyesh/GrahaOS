#!/usr/bin/env bash
# scripts/run_panic_test.sh — Phase 13 oops gate test driver.
#
# Boots GrahaOS with autorun=<binary>, captures serial, runs
# scripts/parse_oops.py to confirm a parseable ==OOPS== block landed
# on the wire. Used by `make test-panic` (panic_test invokes
# SYS_DEBUG(DEBUG_PANIC)) and `make test-kernel-pf` (kpf_test invokes
# SYS_DEBUG(DEBUG_KERNEL_PF) which dereferences an unmapped kernel
# address — exercising the page-fault handler's kpanic_at path).
#
# Usage: run_panic_test.sh <init_binary> [require_klog]
#   init_binary   — name in initrd /bin/, e.g. "panic_test", "kpf_test"
#   require_klog  — "1" to require non-empty ==KLOG== tail (default 0)
#
# Exit codes: 0 oops parsed; 1 missing/malformed; 2 harness error.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

INIT_BIN="${1:-panic_test}"
REQUIRE_KLOG="${2:-0}"
SERIAL_LOG="${GRAHAOS_PANIC_LOG:-/tmp/grahaos_panic.log}"
WALL_TIMEOUT="${GRAHAOS_PANIC_TIMEOUT:-30}"

rm -f "$SERIAL_LOG"

# Inject autorun cmdline, restore on exit.
LIMINE_CONF="limine.conf"
LIMINE_BACKUP="$(mktemp)"
cp "$LIMINE_CONF" "$LIMINE_BACKUP"
cleanup() {
    cp "$LIMINE_BACKUP" "$LIMINE_CONF"
    rm -f "$LIMINE_BACKUP"
}
trap cleanup EXIT

awk '!/^cmdline:/' "$LIMINE_BACKUP" > "$LIMINE_CONF"
printf '\ncmdline: autorun=%s quiet=1 test_timeout_seconds=15\n' "$INIT_BIN" >> "$LIMINE_CONF"

echo "run_panic_test: building ISO with autorun=$INIT_BIN"
if ! make -s all >/dev/null 2>&1; then
    echo "run_panic_test: FAIL — build failed. Verbose rebuild:" >&2
    make all >&2
    exit 2
fi

echo "run_panic_test: booting QEMU (wall timeout ${WALL_TIMEOUT}s)"
set +e
timeout "${WALL_TIMEOUT}s" qemu-system-x86_64 \
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
QEMU_EC=$?
set -e

if [ ! -s "$SERIAL_LOG" ]; then
    echo "run_panic_test: FAIL — serial log empty (qemu_ec=$QEMU_EC)" >&2
    exit 2
fi

EXTRA_ARGS=()
if [ "$REQUIRE_KLOG" = "1" ]; then
    EXTRA_ARGS+=("--require-klog")
fi

if python3 "$REPO_ROOT/scripts/parse_oops.py" \
        "$SERIAL_LOG" --quiet "${EXTRA_ARGS[@]}"; then
    REASON="$(python3 "$REPO_ROOT/scripts/parse_oops.py" \
                "$SERIAL_LOG" --field reason | head -1)"
    echo "run_panic_test: OK — oops captured (reason=\"$REASON\")"
    exit 0
fi

echo "run_panic_test: FAIL — see $SERIAL_LOG" >&2
exit 1
