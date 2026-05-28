#!/usr/bin/env bash
# scripts/vendor-bearssl.sh — Phase 29 Session B (FU28.A).
#
# Re-vendors BearSSL 0.6 (MIT licensed) into vendor/bearssl/.
# Idempotent: safe to re-run; will refuse if vendor/bearssl/src/ already
# contains a populated tree and --force is not passed.
#
# Usage:
#   scripts/vendor-bearssl.sh           # fetch + extract (skip if present)
#   scripts/vendor-bearssl.sh --force   # force re-fetch + replace
#
# After running this script:
#   1. The libtls glue in user/libtls/libtls.c can link against BearSSL.
#   2. Gate goes +10 (libtls_handshake.tap + libtls.tap stop tap_skip'ing).
#   3. grahai's HTTPS -> Gemini path uses the new BearSSL backend.

set -euo pipefail

VERSION="0.6"
TARBALL_URL="https://bearssl.org/bearssl-${VERSION}.tar.gz"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR_DIR="$REPO_ROOT/vendor/bearssl"
WORK_DIR="$(mktemp -d)"
FORCE=0

for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) echo "Unknown arg: $arg" >&2 ; exit 2 ;;
    esac
done

cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

if [ -d "$VENDOR_DIR/src" ] && [ "$FORCE" -eq 0 ]; then
    echo "vendor/bearssl/src already populated; pass --force to overwrite."
    exit 0
fi

echo "Fetching BearSSL $VERSION from $TARBALL_URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -sSL --max-time 60 -o "$WORK_DIR/bearssl.tar.gz" "$TARBALL_URL"
elif command -v wget >/dev/null 2>&1; then
    wget --quiet --timeout=60 -O "$WORK_DIR/bearssl.tar.gz" "$TARBALL_URL"
else
    echo "Neither curl nor wget is available; cannot fetch." >&2
    exit 1
fi

echo "Extracting ..."
tar -xzf "$WORK_DIR/bearssl.tar.gz" -C "$WORK_DIR"

# Trim non-essential parts (Doxyfile, T0Comp.exe, build/, mk/, test/, tools/,
# samples/, conf/, T0/, top-level Makefile, README.txt) so the vendored tree
# stays under 3 MiB and only contains what libtls + the gate need.
SRC_TREE="$WORK_DIR/bearssl-${VERSION}"
for path in Doxyfile T0Comp.exe build mk test tools samples T0 conf Makefile README.txt; do
    rm -rf "$SRC_TREE/$path"
done

mkdir -p "$VENDOR_DIR"
# Wipe existing tree (only when --force or src/ absent).
rm -rf "$VENDOR_DIR/src" "$VENDOR_DIR/inc" "$VENDOR_DIR/LICENSE.txt"
cp -r "$SRC_TREE"/* "$VENDOR_DIR/"

echo "Done.  vendor/bearssl/ now contains BearSSL $VERSION (MIT licensed)."
echo "Next: cd into the GrahaOS root and run 'make' to relink libtls.a"
echo "against BearSSL.  Then 'make test' should pick up +10 from"
echo "libtls_handshake.tap + libtls.tap."
