#!/usr/bin/env bash
# scripts/check_sweep.sh — Phase 13 spec gate test #7.
#
# Walks kernel/ and arch/ looking for any direct `serial_write(` call
# outside arch/x86_64/drivers/serial/serial.c (where the implementation
# lives) and outside vendored sources (Mongoose, klib). Exits non-zero
# if any remain.
#
# Comments containing "serial_write" are tolerated — only actual
# call sites count. Used by run_tests.sh as a pre-flight gate.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Find every .c file in scope.
mapfile -t FILES < <(
    find kernel arch -name '*.c' \
        -not -path '*/serial/serial.c' \
        -not -path '*/mongoose*' \
        -not -path '*/klib.c' \
        2>/dev/null
)

# Match lines with a `serial_write(` invocation that isn't a comment.
# Strip lines whose first non-whitespace character is `//` or `*`.
RESIDUAL=$(for f in "${FILES[@]}"; do
    grep -nE 'serial_write\(' "$f" 2>/dev/null \
        | grep -vE '^[0-9]+:[[:space:]]*//' \
        | grep -vE '^[0-9]+:[[:space:]]*\*' \
        | sed "s|^|$f:|"
done | grep -vE 'serial_write_(hex|dec|raw)\(' || true)

if [ -z "$RESIDUAL" ]; then
    echo "check_sweep: OK — zero serial_write call sites in kernel/ or arch/"
    exit 0
fi

echo "check_sweep: FAIL — residual call sites:"
echo "$RESIDUAL"
exit 1
