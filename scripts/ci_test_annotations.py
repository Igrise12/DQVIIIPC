#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Print a GitHub Actions error annotation for each test the last ctest run failed.

    python3 -I scripts/ci_test_annotations.py BUILD_DIR

GitHub shows a job's log only to signed-in readers, but its annotations to everyone (the check-runs API), the same
reason `.github/problem-matchers/` turns compiler errors into annotations. A workflow runs this after a failed test
step: each failed test becomes one annotation titled with its name, holding the end of its output from
BUILD_DIR/Testing/Temporary/LastTest.log. Our own test output only; it never contains game data (CI has none).
Exits 0 even when nothing is found, so it never hides the test step's own failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

TAIL_CHARS = 3000  # per annotation; GitHub truncates long messages
MAX_ANNOTATIONS = 10  # GitHub keeps at most 10 error annotations per step


def escape_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property(text: str) -> str:
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def failed_tests(failed_log: Path) -> list[str]:
    """LastTestsFailed.log holds one "<index>:<name>" line per failed test."""
    names = []
    for line in failed_log.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            names.append(line.split(":", 1)[1].strip())
    return names


def output_of(last_log: str, name: str) -> str:
    """The "Output:" section of `name`'s block in LastTest.log, or the whole block when it has none."""
    marker = f" Testing: {name}\n"
    start = last_log.find(marker)
    if start < 0:
        return "(no output recorded in LastTest.log)"
    end = last_log.find(" Testing: ", start + len(marker))
    block = last_log[start:end if end >= 0 else len(last_log)]
    begin = block.find("\nOutput:\n")
    stop = block.find("<end of output>")
    if begin >= 0 and stop > begin:
        block = block[begin + len("\nOutput:\n") : stop]
        block = block.split("\n", 1)[1] if block.startswith("-" * 10) else block
    return block.strip() or "(empty output)"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    temporary = Path(argv[1]) / "Testing" / "Temporary"
    failed_log = temporary / "LastTestsFailed.log"
    if not failed_log.is_file():
        print(f"no {failed_log}: no failed tests recorded")
        return 0
    names = failed_tests(failed_log)
    last_log = temporary / "LastTest.log"
    text = last_log.read_text(encoding="utf-8", errors="replace") if last_log.is_file() else ""
    for name in names[:MAX_ANNOTATIONS]:
        tail = output_of(text, name)[-TAIL_CHARS:]
        print(f"::error title={escape_property('ctest failed: ' + name)}::{escape_data(tail)}")
    if len(names) > MAX_ANNOTATIONS:
        rest = ", ".join(names[MAX_ANNOTATIONS:])
        print(f"::error title={escape_property('ctest: more failed tests')}::{escape_data(rest)}")
    print(f"{len(names)} failed test(s) annotated")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
