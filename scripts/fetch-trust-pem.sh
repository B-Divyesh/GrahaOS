#!/usr/bin/env bash
# scripts/fetch-trust-pem.sh — Phase 29 Session B (FU28.A).
#
# Refreshes etc/tls/trust.pem from Mozilla's curated CA bundle via curl.se.
# The bundle is consumed by user/libtls (BearSSL backend) when verifying
# server certificates during the TLS handshake to e.g. Gemini's HTTPS
# endpoint.  Mirror of the same source pattern Mongoose used in Phase 22.
#
# Usage:
#   scripts/fetch-trust-pem.sh           # download + trim (idempotent)
#   scripts/fetch-trust-pem.sh --full    # keep all roots (~224 KiB)
#
# Default behaviour trims to the Google + ISRG (Let's Encrypt) + DigiCert +
# GlobalSign roots that GrahaOS's Phase 29 HTTPS clients actually use.

set -euo pipefail

URL="https://curl.se/ca/cacert.pem"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/etc/tls/trust.pem"
WORK_DIR="$(mktemp -d)"
TRIM=1

for arg in "$@"; do
    case "$arg" in
        --full) TRIM=0 ;;
        *) echo "Unknown arg: $arg" >&2 ; exit 2 ;;
    esac
done

cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

mkdir -p "$(dirname "$OUT")"

echo "Fetching $URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -sSL --max-time 60 -o "$WORK_DIR/cacert.pem" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget --quiet --timeout=60 -O "$WORK_DIR/cacert.pem" "$URL"
else
    echo "Neither curl nor wget available." >&2
    exit 1
fi

if [ "$TRIM" -eq 1 ]; then
    # Mozilla's cacert.pem uses '# Foo Root CA\n=====\n-----BEGIN
    # CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n' blocks.  Keep only
    # those whose '# ' header matches one of the substrings below.
    python3 - "$WORK_DIR/cacert.pem" "$OUT" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
wanted = ("GlobalSign", "GTS Root", "Google Trust",
          "DigiCert", "Let's Encrypt", "ISRG",
          "Amazon Root", "Baltimore CyberTrust",
          "Sectigo", "USERTrust", "Entrust")
with open(src) as f: text = f.read()
blocks = re.split(r"(?m)^(?=#\s)", text)
kept = []
for b in blocks:
    if not b.strip().startswith("#"): continue
    if "BEGIN CERTIFICATE" not in b: continue
    head = b.split("\n",1)[0]
    if any(w in head for w in wanted):
        kept.append(b)
with open(dst, "w") as f:
    f.write("# GrahaOS Phase 29 trust.pem — trimmed Mozilla CA bundle.\n")
    f.write("# Source: %s\n# Trim: %s\n\n" % (sys.argv[1], ", ".join(wanted)))
    f.writelines(kept)
print("wrote %d roots" % len(kept))
PY
else
    cp "$WORK_DIR/cacert.pem" "$OUT"
fi

echo "Done.  $OUT updated."
