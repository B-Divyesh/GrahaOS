#!/usr/bin/env python3
# scripts/parse_oops.py
# Phase 13 host-side validator. Walks a serial-log file, finds the
# ==OOPS== ... ==OOPS END== block emitted by kernel/panic.c, and
# returns structured JSON to stdout. Exit code:
#   0 — exactly one well-formed oops block found
#   1 — block missing or malformed (caller has the diagnostic)
#   2 — file missing / unreadable
#
# Usage:
#   parse_oops.py <serial.log>            # full structured JSON
#   parse_oops.py <serial.log> --quiet    # only exit code, no stdout
#   parse_oops.py <serial.log> --field cpu pid reason  # one per line
#
# parse_oops.py is read by `make test-panic` (and the
# kernel-page-fault gate test). Keep the JSON shape stable —
# parsers in CI and dmesg --json depend on it.

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


# --- Regexes that key on kernel/panic.c output --------------------------
# Header line: ==OOPS== phase=13 build=<sha> cpu=<n> pid=<p> reason="<...>"
_HEADER_RE = re.compile(
    r"^==OOPS== phase=(?P<phase>\d+) build=(?P<build>\S+) "
    r"cpu=(?P<cpu>-?\d+) pid=(?P<pid>-?\d+) "
    r"reason=\"(?P<reason>[^\"]*)\""
)

# magic line.
_MAGIC_RE  = re.compile(r"^oops\.magic=(?P<magic>0x[0-9a-fA-F]+)")

# CR2 / CR3 line.
_CR_RE     = re.compile(
    r"^oops\.cr2=(?P<cr2>0x[0-9a-fA-F]+) oops\.cr3=(?P<cr3>0x[0-9a-fA-F]+)"
)

# RIP / RSP / RFLAGS line.
_RIP_RE    = re.compile(
    r"^oops\.rip=(?P<rip>0x[0-9a-fA-F]+) "
    r"oops\.rsp=(?P<rsp>0x[0-9a-fA-F]+) "
    r"oops\.rflags=(?P<rflags>0x[0-9a-fA-F]+)"
)

# Stack trace frame: "frame N: rip=0x...".
_FRAME_RE  = re.compile(r"^frame (?P<idx>\d+): rip=(?P<rip>0x[0-9a-fA-F]+)")

# klog tail entry: [seq=NN  T.TTT...] LEVEL SUBSYS msg
_KLOG_RE   = re.compile(
    r"^\[seq=(?P<seq>\d+) +(?P<secs>\d+)\.(?P<nsec>\d+)\] "
    r"(?P<level>\w+) (?P<subsys>\S+) (?P<msg>.*)$"
)

_OOPS_END_RE   = re.compile(r"^==OOPS END==")
_KLOG_BEGIN_RE = re.compile(r"^==KLOG BEGIN==")
_KLOG_END_RE   = re.compile(r"^==KLOG END==")


def parse_oops(text: str) -> dict[str, Any] | None:
    """Walk ``text`` and return the first oops block as a dict, or None."""
    out: dict[str, Any] = {
        "found": False,
        "phase": None,
        "build": None,
        "cpu": None,
        "pid": None,
        "reason": None,
        "magic": None,
        "cr2": None,
        "cr3": None,
        "rip": None,
        "rsp": None,
        "rflags": None,
        "stack_frames": [],
        "klog_tail": [],
        "complete": False,
    }

    state = "WAIT"     # WAIT → IN_OOPS → IN_KLOG → IN_OOPS → DONE
    lines = text.splitlines()

    for raw in lines:
        line = raw.rstrip("\r")

        if state == "WAIT":
            m = _HEADER_RE.match(line)
            if m:
                out["found"]  = True
                out["phase"]  = int(m.group("phase"))
                out["build"]  = m.group("build")
                out["cpu"]    = int(m.group("cpu"))
                out["pid"]    = int(m.group("pid"))
                out["reason"] = m.group("reason")
                state = "IN_OOPS"
            continue

        if state == "IN_OOPS":
            if _OOPS_END_RE.match(line):
                out["complete"] = True
                state = "DONE"
                break
            if _KLOG_BEGIN_RE.match(line):
                state = "IN_KLOG"
                continue

            m = _MAGIC_RE.match(line)
            if m:
                out["magic"] = m.group("magic")
                continue
            m = _CR_RE.match(line)
            if m:
                out["cr2"] = m.group("cr2")
                out["cr3"] = m.group("cr3")
                continue
            m = _RIP_RE.match(line)
            if m:
                out["rip"]    = m.group("rip")
                out["rsp"]    = m.group("rsp")
                out["rflags"] = m.group("rflags")
                continue
            m = _FRAME_RE.match(line)
            if m:
                out["stack_frames"].append({
                    "index": int(m.group("idx")),
                    "rip":   m.group("rip"),
                })
                continue
            # Other lines (regs.*, int_no=…) are recorded as raw context.
            continue

        if state == "IN_KLOG":
            if _KLOG_END_RE.match(line):
                state = "IN_OOPS"
                continue
            m = _KLOG_RE.match(line)
            if m:
                out["klog_tail"].append({
                    "seq":     int(m.group("seq")),
                    "ns":      int(m.group("secs")) * 1_000_000_000
                              + int(m.group("nsec")),
                    "level":   m.group("level"),
                    "subsys":  m.group("subsys"),
                    "message": m.group("msg"),
                })
            continue

    return out if out["found"] else None


def validate(oops: dict[str, Any] | None,
             require_klog: bool = False) -> tuple[bool, str]:
    """Return (ok, reason). reason is empty when ok."""
    if oops is None:
        return False, "no ==OOPS== header found in serial log"
    if not oops["complete"]:
        return False, "==OOPS== block was not closed by ==OOPS END=="
    # The kernel prints magic as all 16 hex digits of a u64 ("0x00..dead0005").
    # Accept any width as long as the low 32 bits match OOPS_MAGIC.
    try:
        magic_val = int(oops["magic"], 16)
    except (TypeError, ValueError):
        return False, f"oops.magic not parseable: {oops['magic']!r}"
    if (magic_val & 0xFFFFFFFF) != 0xDEAD0005:
        return False, f"oops.magic mismatch: got {oops['magic']!r}"
    if oops["rip"] is None:
        return False, "no oops.rip line found"
    if not oops["stack_frames"]:
        return False, "stack_frames is empty (kpanic must walk RBP)"
    if require_klog and not oops["klog_tail"]:
        return False, "klog_tail is empty (==KLOG BEGIN==/END== block missing)"
    return True, ""


def main() -> int:
    p = argparse.ArgumentParser(description="Parse a kernel ==OOPS== block.")
    p.add_argument("logfile", help="Serial log file path.")
    p.add_argument("--quiet", action="store_true",
                   help="Suppress JSON output; just return exit code.")
    p.add_argument("--field", nargs="*", default=None,
                   help="Print only these top-level fields, one per line.")
    p.add_argument("--require-klog", action="store_true",
                   help="Fail if ==KLOG== block has zero entries.")
    args = p.parse_args()

    path = Path(args.logfile)
    if not path.exists():
        sys.stderr.write(f"parse_oops: file not found: {path}\n")
        return 2

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        sys.stderr.write(f"parse_oops: cannot read {path}: {exc}\n")
        return 2

    oops = parse_oops(text)
    ok, reason = validate(oops, require_klog=args.require_klog)
    if not ok:
        sys.stderr.write(f"parse_oops: FAIL — {reason}\n")
        return 1

    if args.quiet:
        return 0

    if args.field:
        for f in args.field:
            v = oops.get(f, "")
            if isinstance(v, list):
                v = json.dumps(v)
            print(v)
        return 0

    print(json.dumps(oops, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
