#!/usr/bin/env python3
"""Phase 26 Stage C — gcp.json → /etc/gcp.wit generator.

Reads /etc/gcp.json (the GrahaOS Capability Protocol manifest) and emits a
WebAssembly Interface Type (WIT) document at /etc/gcp.wit. The output is
consumed at module-load time by wasmd to cross-reference each loaded module's
import section against the declared GCP surface.

Determinism:
  - Output is byte-identical across repeated runs on identical input.
  - Input key order does NOT affect output (we sort by pledge_class then by
    syscall number).
  - No timestamps, no hashes, no environment-derived data are emitted.

Type model (pragmatic for current gcp.json schema sparsity):
  - The current schema records syscall `pledge` + `caps` + `description` but
    NOT formal arg/return types. We emit each syscall as a WIT `func` that
    takes a single `list<u64>` (the GCP register-passing convention rolled
    into a flat list) and returns `result<u64, <iface>-error>`.
  - Per-interface error variant is synthesized from a fixed canonical set
    (`e-inval`, `e-perm`, `e-pledge`, `e-nomem`, `e-noent`, `e-busy`,
     `e-fault`, `e-shutdown`, `e-stale`, `e-other`).
  - When the schema is enriched (Phase 27+) to include per-syscall errors,
    args, and return types, this generator's `_emit_func()` learns to emit
    the richer signature without breaking existing consumers.

Pledge → interface mapping:
  - compute, fs_read, fs_write, ipc_send, ipc_recv, sys_control, sys_query,
    spawn, net_*, storage_*, input_*, ai_* are each a separate WIT
    interface. Syscalls with empty `pledge: []` (e.g., SYS_TSC_HZ_QUERY) go
    into the `core` interface.
  - Each syscall is placed in the interface of its FIRST pledge class
    (lexically first when sorted), to keep the partition deterministic.

Usage:
  python3 scripts/gcp2wit.py etc/gcp.json --out etc/gcp.wit
  python3 scripts/gcp2wit.py etc/gcp.json --out -          # write to stdout
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any


WIT_PACKAGE = "grahaos:gcp@1.0.0"

# Canonical error set used across all interfaces. Synthesized today; will be
# replaced with per-interface errors once gcp.json's `errors:` arrays land.
CANONICAL_ERRORS = [
    "e-inval",
    "e-perm",
    "e-pledge",
    "e-nomem",
    "e-noent",
    "e-busy",
    "e-fault",
    "e-shutdown",
    "e-stale",
    "e-other",
]

# Pledge classes that get their own WIT interface. Order is canonical: when
# we sort syscalls by their first pledge class, we use this list's index as
# the sort key (so output is deterministic across input key shuffles).
PLEDGE_INTERFACES = [
    "core",          # empty pledge list
    "compute",
    "time",
    "fs_read",
    "fs_write",
    "ipc_send",
    "ipc_recv",
    "spawn",
    "sys_query",
    "sys_control",
    "net_client",
    "net_server",
    "storage_client",
    "storage_server",
    "input_client",
    "input_server",
    "ai_query",
    "ai_local",
]


def wit_ident(s: str) -> str:
    """Convert a GCP name (SYS_FOO_BAR / fs_read) into a WIT identifier
    (foo-bar / fs-read). WIT identifiers are kebab-case lowercase."""
    s = s.lower()
    if s.startswith("sys-"):
        s = s[4:]
    elif s.startswith("sys_"):
        s = s[4:]
    return s.replace("_", "-")


def iface_ident(pledge: str) -> str:
    """Convert a pledge class (fs_read, sys_control) to a WIT-legal interface
    identifier (kebab-case, no underscores)."""
    return pledge.lower().replace("_", "-")


def first_pledge(syscall: dict[str, Any]) -> str:
    """Return the canonical interface name for a syscall.

    Falls back to `core` if pledge list is empty/missing."""
    pledges = syscall.get("pledge", []) or syscall.get("pledge_classes", [])
    if not pledges:
        return "core"
    # Pick lexically-first pledge that we recognise; otherwise use the very
    # first one verbatim. Compound expressions like `net_server|storage_server`
    # are split on `|` and the first token used.
    for p in sorted(pledges):
        head = p.split("|")[0].strip()
        if head in PLEDGE_INTERFACES:
            return head
    return pledges[0].split("|")[0].strip() or "core"


def partition_by_iface(syscalls: dict[str, dict[str, Any]]) -> dict[str, list[tuple[str, dict]]]:
    """Group syscalls by their WIT interface (first pledge class).

    Within each interface, syscalls are sorted by `number` for stable output."""
    bucket: dict[str, list[tuple[str, dict]]] = {iface: [] for iface in PLEDGE_INTERFACES}
    for name, sc in syscalls.items():
        iface = first_pledge(sc)
        bucket.setdefault(iface, []).append((name, sc))
    for iface in bucket:
        bucket[iface].sort(key=lambda kv: kv[1].get("number", 0))
    return bucket


def emit_error_variant(iface: str) -> list[str]:
    """Emit the synthetic per-interface error variant."""
    out = [f"  variant {iface_ident(iface)}-error {{"]
    for e in CANONICAL_ERRORS:
        out.append(f"    {e},")
    out.append("  }")
    return out


def emit_func(name: str, syscall: dict[str, Any], iface: str) -> str:
    """Emit a single WIT func declaration for one syscall.

    Signature today (pragmatic): `name: func(args: list<u64>) -> result<u64, iface-error>`.
    When schema is enriched, this generator learns to emit richer args + return."""
    fn_ident = wit_ident(name)
    return f"  {fn_ident}: func(args: list<u64>) -> result<u64, {iface_ident(iface)}-error>;"


def emit_interface(iface: str, syscalls: list[tuple[str, dict[str, Any]]]) -> list[str]:
    """Emit one `interface` block."""
    if not syscalls:
        return []
    out: list[str] = []
    out.append(f"interface {iface_ident(iface)} {{")
    out.extend(emit_error_variant(iface))
    out.append("")
    for name, sc in syscalls:
        out.append(emit_func(name, sc, iface))
    out.append("}")
    out.append("")
    return out


def emit_world(populated_ifaces: list[str]) -> list[str]:
    """Emit the `world wasmd-guest` block tying everything together."""
    out: list[str] = []
    out.append("world wasmd-guest {")
    for iface in populated_ifaces:
        out.append(f"  import {iface_ident(iface)};")
    out.append("")
    out.append("  /// Module entry point. Returns 0 on success, non-zero on failure.")
    out.append("  /// wasmd-worker invokes this exactly once after instantiate.")
    out.append("  export start: func() -> s32;")
    out.append("}")
    return out


def generate(gcp: dict[str, Any]) -> str:
    """Generate the full /etc/gcp.wit text from a parsed gcp.json dict."""
    syscalls = gcp.get("syscalls", {})

    bucket = partition_by_iface(syscalls)
    populated = [iface for iface in PLEDGE_INTERFACES if bucket.get(iface)]
    # Catch any syscalls whose pledge wasn't in PLEDGE_INTERFACES (would have
    # been bucketed under their first-pledge string). Sort those after the
    # canonical ones, by name, for determinism.
    extras = sorted(set(bucket) - set(PLEDGE_INTERFACES))
    populated += [iface for iface in extras if bucket.get(iface)]

    lines: list[str] = []
    lines.append("// AUTO-GENERATED by scripts/gcp2wit.py from etc/gcp.json. DO NOT EDIT.")
    lines.append("// Phase 26 Stage C. Regenerate by re-running the generator.")
    lines.append("")
    lines.append(f"package {WIT_PACKAGE};")
    lines.append("")

    for iface in populated:
        lines.extend(emit_interface(iface, bucket[iface]))

    lines.extend(emit_world(populated))

    # Trailing newline; `wit-parser check` requires one.
    return "\n".join(lines) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("input", help="Path to gcp.json")
    p.add_argument("--out", "-o", default="-",
                   help="Output path (use - for stdout). Default: stdout.")
    p.add_argument("--check", action="store_true",
                   help="Run `wit-parser check` on the output (requires wit-parser in PATH).")
    args = p.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        gcp = json.load(f)

    text = generate(gcp)

    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)

    if args.check:
        import shutil
        import subprocess
        wp = shutil.which("wit-parser")
        if not wp:
            print("warning: wit-parser not in PATH; skipping --check", file=sys.stderr)
        else:
            target = "/dev/stdin" if args.out == "-" else args.out
            r = subprocess.run([wp, "check", target], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
