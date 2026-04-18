#!/usr/bin/env python3
# scripts/klog_sweep_pass2.py
# Phase 13 pass-2 cleanup: hand-tune the residual serial_write calls
# the first sweep couldn't reach. The most common shape after pass 1
# looks like:
#
#   klog(LEVEL, SUBSYS, "...prefix...");
#   serial_write(VAR);
#   klog(LEVEL, SUBSYS, "...suffix...");
#
# We collapse three lines into a single klog with `%s` for VAR.
# Optionally one of the bracketing klog calls can be missing
# (collapse two lines instead of three). Standalone bare
# `serial_write(VAR);` lines that aren't safely wrappable get a
# small `klog(KLOG_INFO, SUBSYS_X, "%s", VAR);` rewrite so the gate
# test passes and the output still reaches dmesg.
#
# Run after klog_sweep.py. Idempotent on already-clean files.

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Files in scope (must match klog_sweep.py).
FILE_TABLE = {
    "kernel/main.c":               "SUBSYS_CORE",
    "kernel/elf.c":                "SUBSYS_CORE",
    "kernel/autorun.c":            "SUBSYS_CORE",
    "kernel/cmdline.c":            "SUBSYS_CORE",
    "kernel/shutdown.c":           "SUBSYS_CORE",
    "kernel/watchdog.c":           "SUBSYS_CORE",
    "kernel/capability.c":         "SUBSYS_CAP",
    "kernel/net/net.c":            "SUBSYS_NET",
    "kernel/fs/grahafs.c":         "SUBSYS_FS",
    "kernel/fs/cluster.c":         "SUBSYS_FS",
    "kernel/fs/pipe.c":            "SUBSYS_VFS",
    "arch/x86_64/cpu/sched/sched.c":             "SUBSYS_SCHED",
    "arch/x86_64/cpu/syscall/syscall.c":         "SUBSYS_SYSCALL",
    "arch/x86_64/cpu/interrupts.c":              "SUBSYS_CORE",
    "arch/x86_64/cpu/smp.c":                     "SUBSYS_CORE",
    "arch/x86_64/drivers/e1000/e1000.c":         "SUBSYS_DRV",
}

_RE_KLOG = re.compile(
    r'^(?P<indent>\s*)klog\((?P<level>KLOG_\w+),\s*(?P<subsys>SUBSYS_\w+),'
    r'\s*"(?P<msg>(?:[^"\\]|\\.)*)"(?P<args>(?:,\s*[^)]+)?)\);\s*$'
)
_RE_SW_VAR = re.compile(
    r'^(?P<indent>\s*)serial_write\((?P<arg>[^)"][^);]*?)\);\s*$'
)
_RE_SW_LIT = re.compile(
    r'^(?P<indent>\s*)serial_write\("(?P<msg>(?:[^"\\]|\\.)*)"\);\s*$'
)


def consolidate_lines(lines: list[str], subsys: str) -> tuple[list[str], int]:
    """Walk lines, fold klog+serial_write(VAR)+klog → single klog %s."""
    out: list[str] = []
    n_changes = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        # Try to find the 3-line sandwich:
        #   klog(prefix)
        #   serial_write(VAR)
        #   klog(suffix)
        if i + 2 < len(lines):
            m1 = _RE_KLOG.match(lines[i])
            m2 = _RE_SW_VAR.match(lines[i + 1])
            m3 = _RE_KLOG.match(lines[i + 2])
            if (m1 and m2 and m3
                    and m1.group("level") == m3.group("level")
                    and m1.group("subsys") == m3.group("subsys")
                    and m1.group("indent") == m2.group("indent")
                    and m2.group("indent") == m3.group("indent")):
                level = m1.group("level")
                sub   = m1.group("subsys")
                indent = m1.group("indent")
                prefix = m1.group("msg")
                suffix = m3.group("msg")
                args1 = m1.group("args") or ""
                args3 = m3.group("args") or ""
                merged_msg = prefix + "%s" + suffix
                # Combine args. m1.args first (original positional
                # placeholders inside prefix), then VAR, then m3.args.
                arg_parts = []
                if args1:
                    arg_parts.append(args1.lstrip(", ").rstrip())
                arg_parts.append(m2.group("arg").strip())
                if args3:
                    arg_parts.append(args3.lstrip(", ").rstrip())
                arg_str = (", " + ", ".join(arg_parts)) if arg_parts else ""
                out.append(
                    f'{indent}klog({level}, {sub}, "{merged_msg}"{arg_str});\n'
                )
                n_changes += 1
                i += 3
                continue

        # Two-line: serial_write(VAR) followed by klog(...) — fold into the
        # klog by prepending VAR.
        if i + 1 < len(lines):
            m1 = _RE_SW_VAR.match(lines[i])
            m2 = _RE_KLOG.match(lines[i + 1])
            if m1 and m2 and m1.group("indent") == m2.group("indent"):
                level = m2.group("level")
                sub   = m2.group("subsys")
                indent = m1.group("indent")
                suffix = m2.group("msg")
                args2 = m2.group("args") or ""
                merged_msg = "%s" + suffix
                arg_parts = [m1.group("arg").strip()]
                if args2:
                    arg_parts.append(args2.lstrip(", ").rstrip())
                arg_str = (", " + ", ".join(arg_parts)) if arg_parts else ""
                out.append(
                    f'{indent}klog({level}, {sub}, "{merged_msg}"{arg_str});\n'
                )
                n_changes += 1
                i += 2
                continue

            # klog(prefix...) then serial_write(VAR)
            m1 = _RE_KLOG.match(lines[i])
            m2 = _RE_SW_VAR.match(lines[i + 1])
            if m1 and m2 and m1.group("indent") == m2.group("indent"):
                level = m1.group("level")
                sub   = m1.group("subsys")
                indent = m1.group("indent")
                prefix = m1.group("msg")
                args1 = m1.group("args") or ""
                merged_msg = prefix + "%s"
                arg_parts = []
                if args1:
                    arg_parts.append(args1.lstrip(", ").rstrip())
                arg_parts.append(m2.group("arg").strip())
                arg_str = (", " + ", ".join(arg_parts)) if arg_parts else ""
                out.append(
                    f'{indent}klog({level}, {sub}, "{merged_msg}"{arg_str});\n'
                )
                n_changes += 1
                i += 2
                continue

        # Standalone serial_write(VAR) — rewrite to klog %s so the
        # spec gate test (no serial_write outside serial.c) passes.
        m = _RE_SW_VAR.match(line)
        if m:
            indent = m.group("indent")
            arg = m.group("arg").strip()
            out.append(
                f'{indent}klog(KLOG_INFO, {subsys}, "%s", {arg});\n'
            )
            n_changes += 1
            i += 1
            continue

        # Standalone serial_write("literal") — same idea.
        m = _RE_SW_LIT.match(line)
        if m:
            indent = m.group("indent")
            msg = m.group("msg")
            if msg.endswith("\\n"):
                msg = msg[:-2]
            out.append(
                f'{indent}klog(KLOG_INFO, {subsys}, "{msg}");\n'
            )
            n_changes += 1
            i += 1
            continue

        out.append(line)
        i += 1
    return out, n_changes


def main() -> int:
    paths = [REPO_ROOT / f for f in FILE_TABLE]
    total = 0
    for path in paths:
        if not path.exists():
            print(f"{path.relative_to(REPO_ROOT)}: missing")
            continue
        text = path.read_text()
        lines = text.splitlines(keepends=True)
        new_lines, n = consolidate_lines(lines, FILE_TABLE[
            str(path.relative_to(REPO_ROOT))
        ])
        if n > 0:
            path.write_text("".join(new_lines))
            print(f"{path.relative_to(REPO_ROOT)}: {n} consolidations")
            total += n
        else:
            print(f"{path.relative_to(REPO_ROOT)}: 0 changes")
    print(f"\nTotal: {total} consolidations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
