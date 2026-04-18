#!/usr/bin/env python3
# scripts/klog_sweep.py
# Phase 13 mechanical sweep: rewrite the most common serial_write
# patterns into klog() calls. Conservative — handles the
# straight-line cases that cover the vast majority of call sites
# (single-line message, multi-line "label/hex/dec" sequences) and
# leaves anything ambiguous untouched.
#
# Per-file subsystem mapping is fixed (no auto-detection), and an
# `#include "log.h"` (with the right relative path) is inserted at
# the top if not already present.
#
# Usage:
#   klog_sweep.py            # apply to all known files in REPO_ROOT
#   klog_sweep.py --dry-run  # print diff summary without writing
#   klog_sweep.py file.c     # only one file
#
# Each rewrite is logged to scripts/sweep_report.txt. Manual cleanup
# is expected for the residual sites that don't match either pattern.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Per-file subsystem + include path map.
# ---------------------------------------------------------------------------

FILE_TABLE = {
    # Kernel core
    "kernel/main.c":               ("SUBSYS_CORE",    "log.h"),
    "kernel/elf.c":                ("SUBSYS_CORE",    "log.h"),
    "kernel/autorun.c":            ("SUBSYS_CORE",    "log.h"),
    "kernel/cmdline.c":            ("SUBSYS_CORE",    "log.h"),
    "kernel/shutdown.c":           ("SUBSYS_CORE",    "log.h"),
    # Subsystems
    "kernel/capability.c":         ("SUBSYS_CAP",     "log.h"),
    "kernel/net/net.c":            ("SUBSYS_NET",     "../log.h"),
    "kernel/fs/grahafs.c":         ("SUBSYS_FS",      "../log.h"),
    "kernel/fs/cluster.c":         ("SUBSYS_FS",      "../log.h"),
    "kernel/fs/pipe.c":            ("SUBSYS_VFS",     "../log.h"),
    # arch/
    "arch/x86_64/cpu/sched/sched.c":             ("SUBSYS_SCHED",   "../../../../kernel/log.h"),
    "arch/x86_64/cpu/syscall/syscall.c":         ("SUBSYS_SYSCALL", "../../../../kernel/log.h"),
    "arch/x86_64/cpu/interrupts.c":              ("SUBSYS_CORE",    "../../../kernel/log.h"),
    "arch/x86_64/cpu/smp.c":                     ("SUBSYS_CORE",    "../../../kernel/log.h"),
    "arch/x86_64/drivers/e1000/e1000.c":         ("SUBSYS_DRV",     "../../../../kernel/log.h"),
}

# ---------------------------------------------------------------------------
# Pattern A — single line: serial_write("…\n");
# ---------------------------------------------------------------------------
_PATTERN_A = re.compile(
    r'^(?P<indent>\s*)serial_write\("(?P<msg>(?:[^"\\]|\\.)*)"\);\s*$'
)

# Pattern B — adjacent serial_write/serial_write_hex/serial_write_dec
# group at the same indent level. Captured separately at line level.
_RE_SW_PLAIN = re.compile(
    r'^(?P<indent>\s*)serial_write\("(?P<msg>(?:[^"\\]|\\.)*)"\);\s*$'
)
_RE_SW_HEX   = re.compile(
    r'^(?P<indent>\s*)serial_write_hex\((?P<arg>[^;]+)\);\s*$'
)
_RE_SW_DEC   = re.compile(
    r'^(?P<indent>\s*)serial_write_dec\((?P<arg>[^;]+)\);\s*$'
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def derive_level(msg: str) -> str:
    """Pick a klog level from the message text. Defaults to INFO."""
    upper = msg.upper()
    if "PANIC" in upper or "FATAL" in upper or "!!!" in upper:
        return "KLOG_FATAL"
    if "ERROR" in upper or "FAIL" in upper:
        return "KLOG_ERROR"
    if "WARN" in upper:
        return "KLOG_WARN"
    if "DEBUG" in upper or "[DBG]" in upper:
        return "KLOG_DEBUG"
    return "KLOG_INFO"


def strip_trailing_newline(s: str) -> str:
    # klog auto-newlines at mirror time; strip a single trailing \n.
    if s.endswith("\\n"):
        return s[:-2]
    return s


def msg_uses_format_specifier(msg: str) -> bool:
    return "%" in msg


def maybe_insert_include(text: str, header: str) -> str:
    """Add `#include "header"` after the last existing include if not present."""
    if f'#include "{header}"' in text:
        return text
    inc_lines = [
        i for i, ln in enumerate(text.splitlines())
        if ln.lstrip().startswith("#include")
    ]
    if not inc_lines:
        return text  # caller did something weird; punt
    insert_at = inc_lines[-1] + 1
    lines = text.splitlines(keepends=True)
    new_inc = f'#include "{header}"\n'
    lines.insert(insert_at, new_inc)
    return "".join(lines)


def rewrite_lines(lines: list[str], subsys: str) -> tuple[list[str], int, int]:
    """Walk the file and apply pattern A / B. Returns (new_lines, simple_n, group_n)."""
    out: list[str] = []
    simple_n = 0
    group_n = 0

    i = 0
    while i < len(lines):
        line = lines[i]

        # ---------- Pattern B: try to detect a multi-line group ----------
        m_first = _RE_SW_PLAIN.match(line)
        if m_first:
            indent = m_first.group("indent")
            # Greedily collect adjacent serial_write{,_hex,_dec} at same indent.
            group: list[tuple[str, str]] = []  # (kind, payload)
            group.append(("plain", m_first.group("msg")))
            j = i + 1
            while j < len(lines):
                nxt = lines[j]
                if not nxt.strip():
                    break
                m_p = _RE_SW_PLAIN.match(nxt)
                m_h = _RE_SW_HEX.match(nxt)
                m_d = _RE_SW_DEC.match(nxt)
                if m_p and m_p.group("indent") == indent:
                    group.append(("plain", m_p.group("msg")))
                elif m_h and m_h.group("indent") == indent:
                    group.append(("hex", m_h.group("arg").strip()))
                elif m_d and m_d.group("indent") == indent:
                    group.append(("dec", m_d.group("arg").strip()))
                else:
                    break
                j += 1

            # If the group has >1 element OR a single plain element with
            # no format specifier, rewrite it.
            if len(group) > 1:
                fmt_parts = []
                args = []
                for kind, payload in group:
                    if kind == "plain":
                        # Defensive: escape any % in the literal so klog
                        # doesn't interpret them.
                        fmt_parts.append(payload.replace("%", "%%"))
                    elif kind == "hex":
                        fmt_parts.append("0x%lx")
                        args.append(f"(unsigned long)({payload})")
                    elif kind == "dec":
                        fmt_parts.append("%lu")
                        args.append(f"(unsigned long)({payload})")

                fmt = "".join(fmt_parts)
                fmt = strip_trailing_newline(fmt)
                level = derive_level(fmt.replace("%%", "%"))
                args_str = ("" if not args else ", " + ", ".join(args))
                rewritten = (
                    f'{indent}klog({level}, {subsys}, "{fmt}"{args_str});\n'
                )
                out.append(rewritten)
                group_n += 1
                i = j
                continue

            # Single plain serial_write — handle below as Pattern A.

        m_a = _PATTERN_A.match(line)
        if m_a and not msg_uses_format_specifier(m_a.group("msg")):
            indent = m_a.group("indent")
            msg = strip_trailing_newline(m_a.group("msg"))
            level = derive_level(msg)
            rewritten = f'{indent}klog({level}, {subsys}, "{msg}");\n'
            out.append(rewritten)
            simple_n += 1
            i += 1
            continue

        # No match — keep verbatim.
        out.append(line)
        i += 1

    return out, simple_n, group_n


def process_file(path: Path, dry_run: bool, report: list[str]) -> tuple[int, int]:
    rel = str(path.relative_to(REPO_ROOT))
    if rel not in FILE_TABLE:
        report.append(f"{rel}: SKIP (not in sweep table)")
        return (0, 0)

    subsys, header = FILE_TABLE[rel]
    text = path.read_text()
    before = text

    lines = text.splitlines(keepends=True)
    new_lines, simple_n, group_n = rewrite_lines(lines, subsys)
    new_text = "".join(new_lines)
    if simple_n + group_n > 0:
        new_text = maybe_insert_include(new_text, header)

    if new_text == before:
        report.append(f"{rel}: 0 changes")
        return (0, 0)

    report.append(
        f"{rel}: {simple_n} simple + {group_n} grouped = "
        f"{simple_n + group_n} klog rewrites"
    )

    if not dry_run:
        path.write_text(new_text)

    return (simple_n, group_n)


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 13 klog sweep.")
    p.add_argument("--dry-run", action="store_true",
                   help="Don't write files; only print summary.")
    p.add_argument("files", nargs="*",
                   help="Limit to specific files (relative to repo root).")
    args = p.parse_args()

    report: list[str] = []
    total_simple = 0
    total_group = 0

    paths: list[Path] = []
    if args.files:
        paths = [REPO_ROOT / f for f in args.files]
    else:
        paths = [REPO_ROOT / f for f in FILE_TABLE.keys()]

    for path in paths:
        if not path.exists():
            report.append(f"{path.relative_to(REPO_ROOT)}: MISSING")
            continue
        s, g = process_file(path, args.dry_run, report)
        total_simple += s
        total_group += g

    out_path = REPO_ROOT / "scripts" / "sweep_report.txt"
    if not args.dry_run:
        out_path.write_text("\n".join(report) + "\n")

    print("\n".join(report))
    print(
        f"\nTotal: {total_simple} simple + {total_group} grouped = "
        f"{total_simple + total_group} rewrites"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
