#!/usr/bin/env python3
"""Harvest JAL targets from a PS1 executable and merge them into its function map.

A linear sweep segments code by scanning forward, so it misses any function whose
start it never lands on -- and calling one of those at runtime dies with
"unmapped call: 0x...". Every `jal` instruction encodes its target statically, so
scanning for them recovers the real function starts directly rather than
discovering them one crash at a time.

Sizes are recomputed from the sorted starts. An existing entry keeps its swept
size unless a newly found start lands inside it, in which case it is split.

Usage:
    python3 scripts/add_call_targets.py disc/GAME.cue OPEN.EXE \\
        config/funcmaps/open.json
    python3 scripts/add_call_targets.py disc/GAME.cue OPEN.EXE \\
        config/funcmaps/open.json --dry-run
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_disc import open_disc, resolve_image
from extract_file import find_entry, EXE_HEADER_SIZE, EXE_MAGIC

OP_J = 2
OP_JAL = 3


def read_exe(disc, name: str):
    """Return (text_base, text_bytes, entry_pc) for a PS-X EXE on the disc."""
    entry = find_entry(disc, name)
    data = disc.read(entry["lba"], entry["size"])
    if data[:8] != EXE_MAGIC:
        sys.exit(f"{name} is not a PS-X EXE")
    pc, _gp, t_addr, t_size = struct.unpack_from("<IIII", data, 0x10)
    text = data[EXE_HEADER_SIZE:EXE_HEADER_SIZE + t_size]
    return t_addr, text, pc


def containing(intervals: list, addr: int):
    """Return the start of the known function containing addr, or None."""
    for start, size in intervals:
        if start <= addr < start + size:
            return start
    return None


def scan_call_targets(base: int, text: bytes, intervals: list) -> set:
    """Collect function starts reachable by JAL, plus tail calls made with J.

    Every JAL target is a function start by definition. A J target is only a
    function start when the jump crosses out of its own function -- a tail call.
    A J landing inside the function that issued it is an ordinary local branch,
    and splitting a function there would sever its fallthrough.

    Only harvest a site that already sits inside a known function. A word in
    .data can decode as `jal` without being one -- GAME.EXE's table past
    0x80064B30 encodes `jal 0x80042A40` and `jal 0x80042FF0`, and treating
    those as calls split two real functions (see NOTES.md).
    """
    end = base + len(text)
    targets = set()
    for offset in range(0, len(text) - 3, 4):
        word = struct.unpack_from("<I", text, offset)[0]
        op = word >> 26
        if op not in (OP_J, OP_JAL):
            continue
        pc = base + offset
        if containing(intervals, pc) is None:
            continue
        target = (pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        # Targets out of range are BIOS vectors or other overlays; not ours to name.
        if not (base <= target < end) or target % 4:
            continue
        if op == OP_JAL:
            targets.add(target)
        elif containing(intervals, pc) != containing(intervals, target):
            targets.add(target)
    return targets


def merge(existing: list, new_starts: set, text_base: int, text_end: int) -> list:
    """Rebuild the function list so that starts are sorted and sizes never overlap."""
    known = {int(f["address"], 16): f for f in existing}
    starts = sorted(set(known) | {s for s in new_starts if text_base <= s < text_end})

    out = []
    for i, start in enumerate(starts):
        next_start = starts[i + 1] if i + 1 < len(starts) else text_end
        gap = next_start - start
        prior = known.get(start)
        if prior is not None:
            # Keep the swept size, but split it if a new start landed inside.
            size = min(int(prior["size"]), gap)
            name = prior["name"]
        else:
            size = gap
            name = f"func_{start:08X}"
        if size <= 0:
            continue
        out.append({"address": f"0x{start:08X}", "name": name, "size": size})
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("exe", help="executable name on the disc, e.g. OPEN.EXE")
    parser.add_argument("funcmap", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    disc = open_disc(resolve_image(args.image))
    base, text, entry_pc = read_exe(disc, args.exe)
    text_end = base + len(text)

    doc = json.loads(args.funcmap.read_text())
    before = doc.get("functions", [])

    intervals = [(int(f["address"], 16), int(f["size"])) for f in before]
    targets = scan_call_targets(base, text, intervals)
    targets.add(entry_pc)  # Exec jumps straight here; it must be a function start.

    known = {int(f["address"], 16) for f in before}
    added = sorted(t for t in targets if t not in known and base <= t < text_end)

    merged = merge(before, targets, base, text_end)
    doc["functions"] = merged

    print(f"{args.exe}: text 0x{base:08X}-0x{text_end:08X}  entry 0x{entry_pc:08X}")
    print(f"  jal targets in range : {len(targets)}")
    print(f"  already known        : {len(targets) - len(added)}")
    print(f"  newly added          : {len(added)}")
    print(f"  functions {len(before)} -> {len(merged)}")

    if args.dry_run:
        print("  (dry run, not written)")
        return 0

    args.funcmap.write_text(json.dumps(doc, indent=2))
    print(f"  wrote {args.funcmap}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
