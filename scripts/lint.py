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
run it after a build. Usage: scripts/lint.py [--build build/linux] [--base REF] [--all-port] [--fresh]

A job that came out clean is remembered in <build>/lint-cache.json under a key made from the file,
every file it includes (ninja's dependency lists from the last build), its compile command, the
lines linted, the checks, .clang-tidy, clang-tidy's version and this script. A later run skips a
job whose key is remembered. A job whose includes are unknown, or stale because a file changed
since the last build, always runs. --fresh ignores the cache.
"""

import argparse
import concurrent.futures
import hashlib
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


class Includes:
    """What each source and header depends on, from the build's compile database and ninja's
    dependency lists. A path's dependencies are those of every object that includes it: for a
    source its own object, for a header every object that includes it (a superset of the header's
    own includes, which is safe for a cache key)."""

    def __init__(self, build):
        self.build = build
        self.commands = {}  # object path -> compile command
        for entry in json.loads((build / "compile_commands.json").read_text()):
            output = entry.get("output")
            if output:
                self.commands[str(Path(entry["directory"], output))] = entry.get(
                    "command") or " ".join(entry.get("arguments", []))
        self.objects_of = {}  # included path -> [object path, ...]
        self.deps_of = {}     # object path -> [included path, ...]
        self.stale = set()    # objects ninja marks as having an out-of-date dependency list
        self.mtimes = {}
        self.read_ninja_deps()

    def read_ninja_deps(self):
        result = subprocess.run(["ninja", "-C", str(self.build), "-t", "deps"],
                                capture_output=True, text=True)
        if result.returncode != 0:
            print(f"lint: no ninja dependency lists, so no cache: {result.stderr.strip()}")
            return
        target = None
        for row in result.stdout.splitlines():
            if not row.strip():
                continue
            if not row.startswith(" "):
                name, _, status = row.partition(": #deps")
                target = str((self.build / name).resolve())
                self.deps_of[target] = []
                if "(VALID)" not in status:
                    self.stale.add(target)
                continue
            if target is not None:
                path = row.strip()
                self.deps_of[target].append(path)
                self.objects_of.setdefault(path, []).append(target)

    def objects(self, path):
        """The objects whose dependencies cover path. The game is compiled from hook_src/, a
        renamed copy of decomp_src/, so a decomp_src/ source counts as its hook_src/ copy."""
        found = self.objects_of.get(path, [])
        decomp_src = str(self.build / "decomp_src") + os.sep
        if not found and path.startswith(decomp_src):
            copy = str(self.build / "hook_src") + os.sep + path.removeprefix(decomp_src)
            found = self.objects_of.get(copy, [])
        return found

    def out_of_date(self, obj):
        """True when obj's dependency list may be wrong: ninja says so, or a file it names (an
        edit may have added an include) changed after obj was built."""
        if obj in self.stale:
            return True
        built = self.mtime(obj)
        return built is None or any(self.mtime(dep) is None or self.mtime(dep) > built
                                    for dep in self.deps_of[obj] if not dep.startswith("/nix/store/"))

    def mtime(self, path):
        if path not in self.mtimes:
            try:
                self.mtimes[path] = os.stat(path).st_mtime_ns
            except OSError:
                self.mtimes[path] = None
        return self.mtimes[path]

    def key_parts(self, path):
        """[(dependency, compile command), ...] for path, or None when it cannot be trusted."""
        objects = sorted(set(self.objects(path)))
        if not objects or any(self.out_of_date(obj) for obj in objects):
            return None
        parts = set()
        for obj in objects:
            parts.add(("command", self.commands.get(obj, "")))
            parts.update(("file", dep) for dep in self.deps_of[obj])
        return sorted(parts)


class LintCache:
    """The keys of the jobs that came out clean; see the module docstring."""

    def __init__(self, build, includes, ignore_saved):
        self.path = build / "lint-cache.json"
        self.includes = includes
        self.digests = {}
        tidy_version = subprocess.run(["clang-tidy", "--version"], capture_output=True,
                                      text=True).stdout
        self.salt = "\n".join([tidy_version, self.digest(str(ROOT / ".clang-tidy")),
                               self.digest(__file__)])
        self.saved = set()
        if not ignore_saved:
            try:
                self.saved = set(json.loads(self.path.read_text())["clean"])
            except (OSError, ValueError, KeyError, TypeError):
                pass
        self.clean = set()

    def digest(self, path):
        """A file's content hash. Nix store paths are named by their content, so the path will do."""
        if path.startswith("/nix/store/"):
            return path
        if path not in self.digests:
            try:
                self.digests[path] = hashlib.sha256(Path(path).read_bytes()).hexdigest()
            except OSError:
                self.digests[path] = "missing"
        return self.digests[path]

    def key(self, path, lines, checks):
        """The job's key, or None when it must run every time."""
        parts = self.includes.key_parts(path)
        if parts is None:
            return None
        text = [self.salt, path, self.digest(path), json.dumps(lines), checks or ""]
        for kind, value in parts:
            text.append(f"{kind} {value} {self.digest(value) if kind == 'file' else ''}")
        return hashlib.sha256("\n".join(text).encode()).hexdigest()

    def hit(self, key):
        if key is not None and key in self.saved:
            self.clean.add(key)
            return True
        return False

    def mark_clean(self, key):
        if key is not None:
            self.clean.add(key)

    def save(self):
        """Keeps only this run's clean keys, so the file does not grow."""
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps({"clean": sorted(self.clean)}))
        temporary.replace(self.path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build/linux", help="the configured build directory")
    parser.add_argument("--base", default="HEAD", help="port/ is linted on the lines changed since this")
    parser.add_argument("--all-port", action="store_true", help="lint all of port/, not just changes")
    parser.add_argument("--fresh", action="store_true", help="ignore the cache of clean results")
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

    # An Android source has no flags in a desktop build; `--build build/android-arm64` lints it.
    android_build = "android" in build.name
    jobs = [job for job in jobs if android_build or not job[0].endswith("_android.cpp")]

    cache = LintCache(build, Includes(build), args.fresh)
    keyed = [(job, cache.key(*job)) for job in jobs]
    to_run = [(job, key) for job, key in keyed if not cache.hit(key)]

    failed = 0
    workers = os.cpu_count() or 4
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        results = pool.map(lambda item: (item[1], tidy(build, *item[0])), to_run)
        for key, findings in results:
            if findings:
                failed += 1
                print(findings, flush=True)
            else:
                cache.mark_clean(key)
    cache.save()
    skipped = len(jobs) - len(to_run)
    print(f"lint: {len(jobs)} files ({skipped} unchanged since a clean run), {failed} with findings")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
