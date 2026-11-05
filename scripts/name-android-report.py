#!/usr/bin/env python3
"""Names the stack of a crash report from a phone, which cannot name its own (docs/CRASH_REPORTS.md).

    scripts/name-android-report.py report.txt SYMBOLS

SYMBOLS is the folder or the .zip holding libmain.so of the build the report's header names: the
release's -symbols.zip, or build/android-release/jni/arm64-v8a. Needs llvm-symbolizer (the nix shell).
"""

import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

FRAME = re.compile(r"^#(\d+) 0x[0-9a-f]+ (0x[0-9a-f]+) (.+)$")


def frames_of(report):
    """(number, address in the object, object's file name) for each line of the frames section."""
    in_frames = False
    for line in report.splitlines():
        if line.startswith("== "):
            in_frames = line == "== frames"
        elif in_frames and (frame := FRAME.match(line)):
            yield frame[1], frame[2], frame[3].rsplit("/", 1)[-1]


def name_frames(report, symbols):
    for number, address, object_name in frames_of(report):
        library = symbols / object_name
        if not library.exists():
            print(f"#{number} {object_name}+{address}")
            continue
        named = subprocess.run(["llvm-symbolizer", "--inlines", "--demangle", f"--obj={library}", address],
                               check=True, capture_output=True, text=True).stdout.split("\n")
        lines = [line for line in named if line]
        for at in range(0, len(lines) - 1, 2):
            print(f"#{number} {lines[at]}\n      {lines[at + 1]}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    report = Path(sys.argv[1]).read_text(errors="replace")
    symbols = Path(sys.argv[2])
    if symbols.is_dir():
        name_frames(report, symbols)
        return
    with tempfile.TemporaryDirectory() as folder, zipfile.ZipFile(symbols) as archive:
        archive.extractall(folder)
        name_frames(report, Path(folder))


if __name__ == "__main__":
    main()
