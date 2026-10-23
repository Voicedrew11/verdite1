#!/usr/bin/env python3
"""Run clang-tidy on our code, and only ours.

  libverdite/   every file, every line: it is new code and stays clean
  port/         the lines changed since BASE (default HEAD: what is not committed yet), and new
                files whole. port/ is carried from kf1-enhanced and sushi-shi; their old lines
                are not ours to clean up here
  patches/      every line the patches add to the decomp's game code, with only the bug checks
                (GAME_CHECKS): the patches follow the decomp's style, not ours
  decomp/, third_party/, the decomp's own lines: never

Needs a configured build (compile_commands.json) whose decomp_src/ has the current patches, so
run it after a build. Usage: scripts/lint.py [--build build/linux] [--base REF] [--all-port]
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
GAME_CHECKS = ",".join([
    "-*",
    "bugprone-*",
    "-bugprone-easily-swappable-parameters",
    "-bugprone-narrowing-conversions",
    "-bugprone-implicit-widening-of-multiplication-result",
    "-bugprone-reserved-identifier",
    "clang-analyzer-*",
])
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
DIAGNOSTIC = re.compile(r"^(.*?):(\d+):(\d+): (warning|error|note): ")


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          text=True).stdout


def added_lines(diff_text, root):
    """{absolute path: [line, ...]} for the lines a unified diff adds, paths under root."""
    lines = {}
    path = None
    number = 0
    for row in diff_text.splitlines():
        if row.startswith("+++ "):
            name = row[4:]
            path = None if name == "/dev/null" else str(root / name.removeprefix("b/"))
        elif row.startswith("--- ") or row.startswith("diff "):
            continue
        elif match := HUNK.match(row):
            number = int(match.group(1))
        elif path is None:
            continue
        elif row.startswith("+"):
            lines.setdefault(path, []).append(number)
            number += 1
        elif row.startswith(" "):
            number += 1
    return {path: rows for path, rows in lines.items() if Path(path).suffix in SOURCE_SUFFIXES}


def ranges(numbers):
    """[1, 2, 3, 7] -> [[1, 3], [7, 7]]"""
    out = []
    for n in sorted(set(numbers)):
        if out and out[-1][1] == n - 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return out


def tidy(build, path, lines, checks):
    """Runs clang-tidy on one file; lines None means the whole file. Returns its findings."""
    entry = {"name": path} if lines is None else {"name": path, "lines": ranges(lines)}
    command = ["clang-tidy", "--quiet", "-p", str(build), "--header-filter=.*",
               "--line-filter=" + json.dumps([entry]), path]
    if checks:
        command.insert(1, "--checks=" + checks)
    result = subprocess.run(command, capture_output=True, text=True)
    # clang-tidy keeps a finding when any of its notes passes the line filter, so an analyzer
    # path through our lines brings in a finding in the decomp's own code: keep a finding only
    # when it is on our lines itself (or the file does not compile)
    kept = []
    keep = False
    for row in result.stdout.splitlines():
        if match := DIAGNOSTIC.match(row):
            if match.group(4) == "note":
                kept += [row] if keep else []
                continue
            keep = ("clang-diagnostic-error" in row or
                    (match.group(1) == path and (lines is None or int(match.group(2)) in lines)))
        if keep:
            kept.append(row)
    if result.returncode != 0 and not result.stdout.strip():
        return result.stderr.strip()
    return "\n".join(kept)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build/linux", help="the configured build directory")
    parser.add_argument("--base", default="HEAD", help="port/ is linted on the lines changed since this")
    parser.add_argument("--all-port", action="store_true", help="lint all of port/, not just changes")
    args = parser.parse_args()

    build = (ROOT / args.build).resolve()
    if not (build / "compile_commands.json").exists():
        sys.exit(f"{build}/compile_commands.json is missing: configure and build first")

    jobs = []  # (path, lines or None, checks or None)
    for path in git("ls-files", "--cached", "--others", "--exclude-standard", "libverdite").split():
        if Path(path).suffix in SOURCE_SUFFIXES:
            jobs.append((str(ROOT / path), None, None))

    port_files = git("ls-files", "--others", "--exclude-standard", "port").split()
    if args.all_port:
        port_files += git("ls-files", "port").split()
    for path in port_files:
        if Path(path).suffix in SOURCE_SUFFIXES:
            jobs.append((str(ROOT / path), None, None))
    if not args.all_port:
        diff = git("diff", "-U0", "--no-ext-diff", args.base, "--", "port")
        for path, lines in added_lines(diff, ROOT).items():
            jobs.append((path, lines, None))

    game = build / "decomp_src"
    for patch in sorted((ROOT / "patches").rglob("*.patch")):
        for path, lines in added_lines(patch.read_text(), game).items():
            jobs.append((path, lines, GAME_CHECKS))

    failed = 0
    workers = os.cpu_count() or 4
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        results = pool.map(lambda job: (job[0], tidy(build, *job)), jobs)
        for path, findings in results:
            if findings:
                failed += 1
                print(findings, flush=True)
    print(f"lint: {len(jobs)} files, {failed} with findings")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
