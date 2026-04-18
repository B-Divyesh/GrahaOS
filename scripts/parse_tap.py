#!/usr/bin/env python3
"""parse_tap.py — Phase 12 test-harness TAP 1.4 parser.

Reads a serial-log file captured from a `make test` QEMU run, extracts
the TAP blocks written by /bin/ktest (one block per test, bracketed by
`# TAP BEGIN <name>` / `# TAP END <name>`), and emits a machine-readable
summary.json with schema_version=1 that matches the contract in
specs/phase-12-test-harness.yml §actor_workflows.AW-12.3.

Exit code: 0 iff total failed == 0 and gate_failures == 0.

Usage:
    parse_tap.py <serial_log_path> <summary_json_out_path>
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional


TAP_BEGIN_RE = re.compile(r"^# TAP BEGIN (\S+)\s*$")
TAP_END_RE = re.compile(r"^# TAP END (\S+)\s*$")
TAP_DONE_RE = re.compile(r"^# TAP DONE\s*$")
TAP_EXIT_RE = re.compile(r"^# exit=(-?\d+)\s*$")
PLAN_RE = re.compile(r"^1\.\.(\d+)\s*$")
OK_RE = re.compile(r"^ok\s+(\d+)(?:\s+-\s+(.*?))?\s*$")
SKIP_RE = re.compile(r"^ok\s+(\d+)(?:\s+-\s+(.*?))?\s*#\s*SKIP\s+(.*?)\s*$")
NOT_OK_RE = re.compile(r"^not ok\s+(\d+)(?:\s+-\s+(.*?))?\s*$")
BAIL_RE = re.compile(r"^Bail out!\s+(.*)$")
REASON_RE = re.compile(r"^#\s+(.*)$")


@dataclass
class TestResult:
    name: str
    file: Optional[str] = None
    tap_plan: int = 0
    tap_passed: int = 0
    tap_failed: int = 0
    duration_seconds: Optional[float] = None
    status: str = "passed"   # passed | failed | incomplete | bailed
    gate: bool = True        # Phase 12: every test is a gate test
    last_failure_message: Optional[str] = None
    exit_code: Optional[int] = None
    skipped: int = 0


@dataclass
class Summary:
    schema_version: int = 1
    run_started_utc: Optional[str] = None
    run_ended_utc: Optional[str] = None
    kernel_git_sha: Optional[str] = None
    duration_seconds: Optional[float] = None
    total: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    gate_failures: int = 0
    tests: list[TestResult] = field(default_factory=list)


def parse_tap_block(lines: list[str], test: TestResult) -> None:
    """Scan a list of TAP lines for one test and fill `test` in place."""
    for line in lines:
        m = BAIL_RE.match(line)
        if m:
            test.status = "bailed"
            test.tap_failed += 1
            test.last_failure_message = f"bail out: {m.group(1)}"
            return

        m = PLAN_RE.match(line)
        if m:
            test.tap_plan = int(m.group(1))
            continue

        # SKIP_RE must run before OK_RE (more specific).
        m = SKIP_RE.match(line)
        if m:
            test.tap_passed += 1
            test.skipped += 1
            continue

        m = NOT_OK_RE.match(line)
        if m:
            test.tap_failed += 1
            test.status = "failed"
            name = (m.group(2) or "").strip()
            test.last_failure_message = f"not ok: {name}"
            continue

        m = OK_RE.match(line)
        if m:
            test.tap_passed += 1
            continue

        m = REASON_RE.match(line)
        if m and test.last_failure_message and "(" not in test.last_failure_message:
            # Attach the first '#' reason line to the most recent failure.
            text = m.group(1).strip()
            if text and not text.startswith(("ktest", "TAP", "passed", "exit=")):
                test.last_failure_message += f" [{text}]"
            continue

    # Plan / count sanity.
    if test.tap_plan > 0 and test.tap_passed + test.tap_failed != test.tap_plan:
        # Under-run — e.g. test crashed before completing its plan.
        if test.status == "passed":
            test.status = "incomplete"
        if not test.last_failure_message:
            test.last_failure_message = (
                f"plan {test.tap_plan} but saw {test.tap_passed + test.tap_failed}"
            )


def parse_log(log_text: str) -> list[TestResult]:
    """Extract per-test TAP blocks from the serial log."""
    results: list[TestResult] = []
    current_lines: list[str] = []
    current_name: Optional[str] = None
    in_block = False

    for raw in log_text.splitlines():
        # Strip ANSI escape sequences that the Limine menu may leave.
        line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw).rstrip()

        if not in_block:
            # Between blocks, # exit=N applies to the most recently
            # ended test (ktest emits END then exit).
            m = TAP_EXIT_RE.match(line)
            if m and results:
                results[-1].exit_code = int(m.group(1))
                if results[-1].exit_code != 0 and results[-1].status == "passed":
                    results[-1].status = "failed"
                    results[-1].last_failure_message = (
                        results[-1].last_failure_message
                        or f"nonzero exit={results[-1].exit_code}"
                    )
                continue
            m = TAP_BEGIN_RE.match(line)
            if m:
                current_name = m.group(1)
                current_lines = []
                in_block = True
            continue

        # In a block.
        m = TAP_END_RE.match(line)
        if m:
            name = m.group(1)
            if name != current_name:
                # Markers misaligned — treat as incomplete.
                tr = TestResult(
                    name=current_name or "<unknown>",
                    file=None,
                    status="incomplete",
                    last_failure_message=f"END marker mismatch: saw {name}",
                )
                parse_tap_block(current_lines, tr)
                results.append(tr)
                in_block = False
                current_lines = []
                current_name = None
                continue
            tr = TestResult(name=name, file=f"bin/tests/{name}.tap")
            parse_tap_block(current_lines, tr)
            results.append(tr)
            in_block = False
            current_lines = []
            current_name = None
            continue

        m = TAP_EXIT_RE.match(line)
        if m:
            # Consumed at END time — attach retroactively.
            if results:
                results[-1].exit_code = int(m.group(1))
                if results[-1].exit_code != 0 and results[-1].status == "passed":
                    # Test passed all asserts but exited non-zero.
                    results[-1].status = "failed"
                    results[-1].last_failure_message = (
                        results[-1].last_failure_message
                        or f"nonzero exit={results[-1].exit_code}"
                    )
            continue

        current_lines.append(line)

    if in_block and current_name:
        # Log ended mid-block — incomplete.
        tr = TestResult(
            name=current_name,
            file=f"bin/tests/{current_name}.tap",
            status="incomplete",
            last_failure_message="END marker missing",
        )
        parse_tap_block(current_lines, tr)
        results.append(tr)

    return results


def kernel_git_sha(repo_root: Path) -> Optional[str]:
    head = repo_root / ".git" / "HEAD"
    if not head.exists():
        return None
    try:
        ref = head.read_text().strip()
        if ref.startswith("ref: "):
            path = repo_root / ".git" / ref[5:]
            if path.exists():
                return path.read_text().strip()[:12]
        return ref[:12]
    except OSError:
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("serial_log")
    ap.add_argument("summary_out")
    ap.add_argument("--started-utc")
    ap.add_argument("--ended-utc")
    ap.add_argument("--duration-seconds", type=float)
    ap.add_argument("--repo-root", default=os.getcwd())
    ap.add_argument(
        "--quiet", action="store_true",
        help="Suppress the human-readable summary printed to stdout.",
    )
    args = ap.parse_args()

    log_path = Path(args.serial_log)
    if not log_path.exists():
        print(f"ERROR: serial log not found: {log_path}", file=sys.stderr)
        return 2

    log_text = log_path.read_text(errors="replace")
    tests = parse_log(log_text)

    # Aggregate.
    total = sum(t.tap_plan or (t.tap_passed + t.tap_failed) for t in tests)
    passed = sum(t.tap_passed for t in tests)
    failed = sum(t.tap_failed for t in tests)
    skipped = sum(t.skipped for t in tests)
    gate_failures = sum(
        1 for t in tests if t.gate and t.status in ("failed", "incomplete", "bailed")
    )

    summary = Summary(
        schema_version=1,
        run_started_utc=args.started_utc,
        run_ended_utc=args.ended_utc or datetime.datetime.now(datetime.timezone.utc).isoformat(),
        kernel_git_sha=kernel_git_sha(Path(args.repo_root)),
        duration_seconds=args.duration_seconds,
        total=total,
        passed=passed,
        failed=failed,
        skipped=skipped,
        gate_failures=gate_failures,
        tests=tests,
    )

    out_path = Path(args.summary_out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(asdict(summary), indent=2, default=str) + "\n"
    )

    if not args.quiet:
        if tests:
            for t in tests:
                mark = {
                    "passed": "[PASS]",
                    "failed": "[FAIL]",
                    "incomplete": "[INCOMPLETE]",
                    "bailed": "[BAIL]",
                }.get(t.status, "[?]")
                plan = t.tap_plan or (t.tap_passed + t.tap_failed)
                print(f"  {mark} {t.name}: {t.tap_passed}/{plan} "
                      f"(failed={t.tap_failed})"
                      + (f"  — {t.last_failure_message}"
                         if t.last_failure_message else ""))
        else:
            print("  (no TAP blocks found in serial log — harness regression?)")
        print()
        print(f"Summary: {passed}/{total} assertions passed, "
              f"{failed} failed, {skipped} skipped, "
              f"{gate_failures} gate failure(s).")
        print(f"Summary JSON: {out_path}")

    # Exit 0 iff zero failures.
    if tests and failed == 0 and gate_failures == 0:
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
