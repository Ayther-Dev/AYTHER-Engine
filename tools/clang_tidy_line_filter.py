#!/usr/bin/env python3
"""Emit clang-tidy's JSON line filter for lines added by a revision range."""

from __future__ import annotations

import json
import re
import subprocess
import sys


HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def added_line_filter(diff: str) -> list[dict[str, object]]:
    """Convert a zero-context unified diff into clang-tidy filter entries."""
    ranges: dict[str, list[list[int]]] = {}
    current_file: str | None = None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            current_file = line[6:]
            ranges.setdefault(current_file, [])
            continue
        match = HUNK.match(line)
        if current_file is None or match is None:
            continue
        start = int(match.group(1))
        count = int(match.group(2) or "1")
        if count:
            ranges[current_file].append([start, start + count - 1])

    return [
        {"name": name, "lines": lines}
        for name, lines in ranges.items() if lines
    ]


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <base-revision> <head-revision>", file=sys.stderr)
        return 2

    diff = subprocess.run(
        [
            "git", "diff", "--unified=0", "--no-color",
            sys.argv[1], sys.argv[2], "--",
            "*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.hxx",
        ],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout

    print(json.dumps(added_line_filter(diff), separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
