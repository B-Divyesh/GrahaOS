#!/usr/bin/env python3
"""Phase 26 Stage C.3 — gcp2wit.py completeness test.

Asserts: number of syscalls in /etc/gcp.json `syscalls` dict equals number
of `func` declarations emitted in /etc/gcp.wit (excluding the `start` export
in the world block).

Exit 0 on match, 1 on mismatch."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(ROOT, "scripts", "gcp2wit.py")
GCP = os.path.join(ROOT, "etc", "gcp.json")


def main() -> int:
    with open(GCP, "r", encoding="utf-8") as f:
        gcp = json.load(f)
    n_syscalls = len(gcp.get("syscalls", {}))

    r = subprocess.run(
        [sys.executable, GEN, GCP, "--out", "-"],
        capture_output=True,
        check=True,
        text=True,
    )
    text = r.stdout

    # Match every func line that lives inside an interface (i.e. is
    # 2-space-indented and ends with `;`), skip the `export start: func`
    # that lives in the world block (also 2-space-indented but matches
    # the literal `start: func` prefix).
    func_pattern = re.compile(r"^  ([a-z][a-z0-9-]*): func", re.MULTILINE)
    matches = func_pattern.findall(text)
    # Drop the world's `start` export (it's not derived from gcp.json).
    interface_funcs = [m for m in matches if m != "start"]

    if len(interface_funcs) != n_syscalls:
        print(
            f"FAIL: completeness mismatch.\n"
            f"  syscalls in gcp.json: {n_syscalls}\n"
            f"  func decls in gcp.wit: {len(interface_funcs)}\n"
            f"  emitted: {sorted(interface_funcs)}",
            file=sys.stderr,
        )
        # Also print which syscalls are missing.
        gcp_names = set(gcp["syscalls"].keys())
        wit_names = set(interface_funcs)

        def normalize(s: str) -> str:
            n = s.lower()
            if n.startswith("sys-"):
                n = n[4:]
            elif n.startswith("sys_"):
                n = n[4:]
            return n.replace("_", "-")

        gcp_norm = {normalize(n): n for n in gcp_names}
        missing = [v for k, v in gcp_norm.items() if k not in wit_names]
        if missing:
            print(f"  MISSING from gcp.wit: {missing}", file=sys.stderr)
        return 1

    print(
        f"OK: completeness matches: {n_syscalls} syscalls in, "
        f"{len(interface_funcs)} func decls out."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
