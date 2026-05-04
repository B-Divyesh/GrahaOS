#!/usr/bin/env python3
"""Phase 26 Stage C.3 — gcp2wit.py determinism test.

Runs the generator twice on the same input and asserts byte-identical output.
Then shuffles the JSON's `syscalls` dict key order and runs again — output
MUST still be byte-identical (the generator sorts deterministically by pledge
class index then by syscall number).

Exit 0 on success, 1 on any divergence."""

from __future__ import annotations

import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(ROOT, "scripts", "gcp2wit.py")
GCP = os.path.join(ROOT, "etc", "gcp.json")


def run_gen(input_path: str) -> bytes:
    """Run gcp2wit.py against `input_path`; return stdout bytes."""
    r = subprocess.run(
        [sys.executable, GEN, input_path, "--out", "-"],
        capture_output=True,
        check=True,
    )
    return r.stdout


def shuffle_syscalls(json_text: str, seed: int) -> str:
    """Return `json_text` with the top-level `syscalls` dict's keys
    re-ordered using `random.seed(seed)`. The shuffled dict's ITEMS
    are otherwise unchanged."""
    d = json.loads(json_text)
    sc = d.get("syscalls", {})
    if not isinstance(sc, dict):
        return json_text
    keys = list(sc.keys())
    rng = random.Random(seed)
    rng.shuffle(keys)
    d["syscalls"] = {k: sc[k] for k in keys}
    # Use sort_keys=False so dict order is preserved verbatim.
    return json.dumps(d, indent=2, sort_keys=False)


def main() -> int:
    # Pass 1: same-input determinism.
    a = run_gen(GCP)
    b = run_gen(GCP)
    if a != b:
        print(
            f"FAIL: identical input produced divergent output.\n"
            f"  sha256(a)={hashlib.sha256(a).hexdigest()[:16]}\n"
            f"  sha256(b)={hashlib.sha256(b).hexdigest()[:16]}",
            file=sys.stderr,
        )
        return 1

    # Pass 2: key-shuffle determinism (3 different seeds).
    with open(GCP, "r", encoding="utf-8") as f:
        original_text = f.read()

    expected_sha = hashlib.sha256(a).hexdigest()
    for seed in (42, 1337, 0xDEADBEEF):
        shuffled = shuffle_syscalls(original_text, seed)
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", suffix=".json", delete=False
        ) as f:
            f.write(shuffled)
            tmp_path = f.name
        try:
            out = run_gen(tmp_path)
        finally:
            os.unlink(tmp_path)

        out_sha = hashlib.sha256(out).hexdigest()
        if out_sha != expected_sha:
            print(
                f"FAIL: seed={seed:#x} produced divergent output.\n"
                f"  expected sha256={expected_sha[:16]}...\n"
                f"  observed sha256={out_sha[:16]}...",
                file=sys.stderr,
            )
            return 1
        print(f"  shuffle seed={seed:#x}: matches expected sha256 ({out_sha[:16]}...)")

    print(f"OK: gcp2wit.py is deterministic (sha256={expected_sha[:16]}...).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
