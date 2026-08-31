#!/usr/bin/env python3
"""Enforce total and changed-line coverage thresholds from an LCOV report."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


LineCoverage = dict[str, dict[int, int]]


def repository_path(source: str, root: Path) -> str:
    path = Path(source)
    if path.is_absolute():
        try:
            path = path.resolve().relative_to(root.resolve())
        except ValueError:
            pass
    normalized = path.as_posix()
    parts = normalized.split("/")
    matching_roots = [index for index, part in enumerate(parts) if part == root.name]
    if matching_roots:
        normalized = "/".join(parts[matching_roots[-1] + 1 :])
    elif path.is_absolute():
        return normalized
    return normalized.removeprefix("./")


def parse_lcov(report: Path, root: Path) -> LineCoverage:
    coverage: LineCoverage = defaultdict(dict)
    current: str | None = None
    for raw_line in report.read_text(encoding="utf-8").splitlines():
        if raw_line.startswith("SF:"):
            current = repository_path(raw_line[3:], root)
        elif raw_line.startswith("DA:") and current is not None:
            line_text, hits_text, *_ = raw_line[3:].split(",")
            line = int(line_text)
            hits = int(hits_text)
            coverage[current][line] = max(coverage[current].get(line, 0), hits)
        elif raw_line == "end_of_record":
            current = None
    return dict(coverage)


def changed_lines(diff: str) -> dict[str, set[int]]:
    result: dict[str, set[int]] = defaultdict(set)
    current: str | None = None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            current = line[6:]
            continue
        if current is None or not line.startswith("@@"):
            continue
        match = re.search(r"\+(\d+)(?:,(\d+))?", line)
        if match is None:
            continue
        start = int(match.group(1))
        count = int(match.group(2) or "1")
        result[current].update(range(start, start + count))
    return dict(result)


def git_changed_lines(root: Path, base: str, prefixes: list[str]) -> dict[str, set[int]]:
    command = [
        "git",
        "-c",
        f"safe.directory={root.as_posix()}",
        "diff",
        "--unified=0",
        "--no-ext-diff",
        f"{base}...HEAD",
        "--",
        *prefixes,
    ]
    completed = subprocess.run(
        command,
        cwd=root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return changed_lines(completed.stdout)


def percentage(covered: int, total: int) -> float:
    return 100.0 if total == 0 else covered * 100.0 / total


def line_ranges(lines: list[int]) -> str:
    if not lines:
        return ""
    ranges: list[str] = []
    start = previous = lines[0]
    for line in lines[1:]:
        if line == previous + 1:
            previous = line
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = line
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ", ".join(ranges)


def evaluate(
    coverage: LineCoverage,
    changes: dict[str, set[int]],
    prefixes: list[str],
) -> tuple[int, int, int, int, dict[str, list[int]], dict[str, list[int]]]:
    scoped = {
        file_name: lines
        for file_name, lines in coverage.items()
        if any(file_name.startswith(prefix) for prefix in prefixes)
    }
    total = sum(len(lines) for lines in scoped.values())
    covered = sum(hits > 0 for lines in scoped.values() for hits in lines.values())
    uncovered = {
        file_name: sorted(line for line, hits in lines.items() if hits == 0)
        for file_name, lines in scoped.items()
        if any(hits == 0 for hits in lines.values())
    }

    changed_total = changed_covered = 0
    changed_uncovered: dict[str, list[int]] = {}
    for file_name, lines in scoped.items():
        relevant = sorted(set(lines).intersection(changes.get(file_name, set())))
        if not relevant:
            continue
        changed_total += len(relevant)
        changed_covered += sum(lines[line] > 0 for line in relevant)
        misses = [line for line in relevant if lines[line] == 0]
        if misses:
            changed_uncovered[file_name] = misses
    return total, covered, changed_total, changed_covered, uncovered, changed_uncovered


def render_section(title: str, files: dict[str, list[int]]) -> list[str]:
    lines = [title]
    if not files:
        lines.append("  none")
    else:
        lines.extend(f"  {path}: {line_ranges(values)}" for path, values in sorted(files.items()))
    return lines


def gate_failed(
    total: int,
    total_rate: float,
    minimum_total: float,
    changed_total: int,
    changed_rate: float,
    minimum_changed: float,
) -> bool:
    return (
        total == 0
        or total_rate < minimum_total
        or (changed_total > 0 and changed_rate < minimum_changed)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", required=True, choices=("rust", "cpp"))
    parser.add_argument("--lcov", required=True, type=Path)
    parser.add_argument("--config", type=Path, default=Path(".github/coverage-thresholds.json"))
    parser.add_argument("--base")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    root = Path.cwd().resolve()
    settings = json.loads(args.config.read_text(encoding="utf-8"))[args.component]
    prefixes = settings["source_prefixes"]
    coverage = parse_lcov(args.lcov, root)
    changes = git_changed_lines(root, args.base, prefixes) if args.base else {}
    # One source of truth. Summing the LF/LH summary lines instead would count a
    # file twice whenever llvm-cov emits a record per object, inflating the
    # denominator against the merged per-line data the rest of the gate uses.
    total, covered, new_total, new_covered, uncovered, new_uncovered = evaluate(
        coverage, changes, prefixes
    )
    total_rate = percentage(covered, total)
    new_rate = percentage(new_covered, new_total)
    minimum_total = float(settings["minimum_total"])
    minimum_changed = float(settings["minimum_changed"])

    lines = [
        f"{args.component.upper()} line coverage gate",
        f"Total: {covered}/{total} ({total_rate:.2f}%), minimum {minimum_total:.2f}%",
        (
            f"Changed: {new_covered}/{new_total} ({new_rate:.2f}%), "
            f"minimum {minimum_changed:.2f}%"
            if new_total
            else "Changed: no coverable changed lines"
        ),
        "",
        *render_section("Uncovered lines:", uncovered),
        "",
        *render_section("Uncovered changed lines:", new_uncovered),
    ]
    output = "\n".join(lines) + "\n"
    print(output, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")

    failed = gate_failed(
        total, total_rate, minimum_total, new_total, new_rate, minimum_changed
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
