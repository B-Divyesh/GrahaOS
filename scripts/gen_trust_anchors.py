#!/usr/bin/env python3
"""gen_trust_anchors.py — Phase 29 FU29.B.bearssl_wire trust-anchor generator.

Reads a PEM trust store (default etc/tls/trust.pem — the Mozilla CA bundle)
and emits user/libtls/trust_anchors.c containing a static, pure-C
`const br_x509_trust_anchor g_grahaos_TAs[]` table plus
`const size_t g_grahaos_TAs_num`.

This mirrors the role of BearSSL's own `brssl ta` tool but produces output
that depends ONLY on the kernel-free BearSSL headers (vendor/bearssl/inc) and
has NO runtime PEM/X.509 parsing: every trust anchor (subject Distinguished
Name DER, RSA modulus+exponent OR EC curve-id+public-point) is baked into
rodata at build time.  libtls_init() therefore just returns the count.

Determinism: certificates are emitted in the exact order they appear in the
input PEM (file order).  Two invocations on the same trust.pem produce
byte-identical output.

Design parallels scripts/gen_manifest_blob.py (auto-generated rodata .c with
a DO-NOT-EDIT banner, re-run on input mtime change via the Makefile).

Dependencies: the `cryptography` Python package (provides X.509 + public-key
parsing without shelling out to openssl per-cert).

Usage:
    python3 scripts/gen_trust_anchors.py etc/tls/trust.pem \
        --out user/libtls/trust_anchors.c
"""

import argparse
import os
import re
import sys

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa, ec
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "gen_trust_anchors: the 'cryptography' package is required "
        "(pip install cryptography)\n"
    )
    sys.exit(2)


# BearSSL curve identifiers (vendor/bearssl/inc/bearssl_ec.h).
BR_EC_secp256r1 = 23
BR_EC_secp384r1 = 24
BR_EC_secp521r1 = 25

# Map the cryptography curve name to the BearSSL curve id.  Only the curves
# BearSSL's i31 / m31 EC code can actually validate are accepted; any other
# curve causes the anchor to be skipped (with a diagnostic).
_EC_CURVE_MAP = {
    "secp256r1": BR_EC_secp256r1,
    "prime256v1": BR_EC_secp256r1,
    "secp384r1": BR_EC_secp384r1,
    "secp521r1": BR_EC_secp521r1,
}

# BearSSL key types (vendor/bearssl/inc/bearssl_x509.h).
BR_KEYTYPE_RSA = 1
BR_KEYTYPE_EC = 2
BR_X509_TA_CA = 0x0001

_PEM_BLOCK = re.compile(
    rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
    re.DOTALL,
)


def _c_byte_array(name, data):
    """Emit a `static const unsigned char NAME[] = { ... };` block."""
    lines = [f"static const unsigned char {name}[] = {{"]
    chunk = 12
    for i in range(0, len(data), chunk):
        row = data[i:i + chunk]
        nums = ", ".join(f"0x{b:02X}" for b in row)
        lines.append(f"\t{nums},")
    lines.append("};")
    return "\n".join(lines)


def _split_certs(pem_bytes):
    """Yield each PEM CERTIFICATE block (with markers) in file order."""
    for m in _PEM_BLOCK.finditer(pem_bytes):
        yield m.group(0)


def emit_c(input_path, out_path):
    with open(input_path, "rb") as fh:
        pem = fh.read()

    anchors = []  # list of dicts describing each emitted anchor
    skipped = 0
    idx = 0
    for block in _split_certs(pem):
        idx += 1
        try:
            cert = x509.load_pem_x509_certificate(block)
        except Exception as exc:  # noqa: BLE001 — diagnostic only
            sys.stderr.write(f"gen_trust_anchors: cert #{idx} parse failed: {exc}\n")
            skipped += 1
            continue

        # Subject Distinguished Name, DER-encoded (exactly the bytes BearSSL's
        # X.509 minimal engine compares against the certificate-chain DNs).
        dn_der = cert.subject.public_bytes()

        pk = cert.public_key()
        if isinstance(pk, rsa.RSAPublicKey):
            nums = pk.public_numbers()
            n = nums.n.to_bytes((nums.n.bit_length() + 7) // 8, "big")
            e = nums.e.to_bytes((nums.e.bit_length() + 7) // 8, "big")
            anchors.append({
                "kind": "rsa",
                "dn": dn_der,
                "n": n,
                "e": e,
            })
        elif isinstance(pk, ec.EllipticCurvePublicKey):
            curve_name = pk.curve.name
            br_curve = _EC_CURVE_MAP.get(curve_name)
            if br_curve is None:
                sys.stderr.write(
                    f"gen_trust_anchors: cert #{idx} EC curve {curve_name} "
                    "unsupported by BearSSL build — skipping anchor\n"
                )
                skipped += 1
                continue
            q = pk.public_bytes(
                serialization.Encoding.X962,
                serialization.PublicFormat.UncompressedPoint,
            )
            anchors.append({
                "kind": "ec",
                "dn": dn_der,
                "curve": br_curve,
                "q": bytes(q),
            })
        else:
            sys.stderr.write(
                f"gen_trust_anchors: cert #{idx} unsupported key type "
                f"{type(pk).__name__} — skipping anchor\n"
            )
            skipped += 1
            continue

    out = []
    out.append("// user/libtls/trust_anchors.c — AUTO-GENERATED by")
    out.append("// scripts/gen_trust_anchors.py.  DO NOT EDIT BY HAND.")
    out.append("// Re-run the generator after etc/tls/trust.pem changes.")
    out.append("//")
    out.append("// Phase 29 FU29.B.bearssl_wire — static BearSSL trust-anchor table")
    out.append("// baked from the Mozilla CA bundle.  Pure C: no kernel deps, no")
    out.append("// runtime PEM/X.509 parsing.  libtls_init() returns the count.")
    out.append("#include <stddef.h>")
    out.append("")
    out.append("#include \"bearssl.h\"")
    out.append("")

    # Per-anchor byte arrays.
    for i, a in enumerate(anchors):
        out.append(_c_byte_array(f"TA{i}_DN", a["dn"]))
        if a["kind"] == "rsa":
            out.append(_c_byte_array(f"TA{i}_RSA_N", a["n"]))
            out.append(_c_byte_array(f"TA{i}_RSA_E", a["e"]))
        else:
            out.append(_c_byte_array(f"TA{i}_EC_Q", a["q"]))
        out.append("")

    # The trust-anchor table.
    out.append("const br_x509_trust_anchor g_grahaos_TAs[] = {")
    for i, a in enumerate(anchors):
        out.append("\t{")
        out.append(f"\t\t{{ (unsigned char *)TA{i}_DN, sizeof TA{i}_DN }},")
        out.append("\t\tBR_X509_TA_CA,")
        if a["kind"] == "rsa":
            out.append("\t\t{")
            out.append("\t\t\tBR_KEYTYPE_RSA,")
            out.append("\t\t\t{ .rsa = {")
            out.append(f"\t\t\t\t(unsigned char *)TA{i}_RSA_N, sizeof TA{i}_RSA_N,")
            out.append(f"\t\t\t\t(unsigned char *)TA{i}_RSA_E, sizeof TA{i}_RSA_E,")
            out.append("\t\t\t} }")
            out.append("\t\t}")
        else:
            out.append("\t\t{")
            out.append("\t\t\tBR_KEYTYPE_EC,")
            out.append("\t\t\t{ .ec = {")
            out.append(f"\t\t\t\t{a['curve']},")
            out.append(f"\t\t\t\t(unsigned char *)TA{i}_EC_Q, sizeof TA{i}_EC_Q,")
            out.append("\t\t\t} }")
            out.append("\t\t}")
        out.append("\t},")
    out.append("};")
    out.append("")
    out.append("const size_t g_grahaos_TAs_num =")
    out.append("\tsizeof g_grahaos_TAs / sizeof g_grahaos_TAs[0];")
    out.append("")

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w") as fh:
        fh.write("\n".join(out))

    sys.stderr.write(
        f"gen_trust_anchors: {len(anchors)} anchors emitted "
        f"({skipped} skipped) from {input_path} -> {out_path}\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", nargs="?", default="etc/tls/trust.pem",
                    help="path to the PEM trust store (default etc/tls/trust.pem)")
    ap.add_argument("--out", default="user/libtls/trust_anchors.c",
                    help="output .c file path")
    args = ap.parse_args()
    emit_c(args.input, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
