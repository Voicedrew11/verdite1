#!/usr/bin/env python3
"""Inspect a PS1 disc image: print SYSTEM.CNF and walk the ISO9660 filesystem.

The output is what drives the game's recompiler config -- SYSTEM.CNF names the
boot executable and its load address, and the file listing (with LBAs and sizes)
is what you need to declare overlays.

Usage:
    python3 scripts/inspect_disc.py disc/GAME.cue
    python3 scripts/inspect_disc.py disc/GAME.bin
"""

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verdite_game

# PS1 discs are usually MODE2/2352: 2352-byte sectors carrying 2048 bytes of
# user data after a 24-byte sync+header+subheader. Plain 2048-byte images
# (MODE1/2048) also show up in the wild.
USER_DATA = 2048
LAYOUTS = [(2352, 24), (2048, 0), (2336, 8)]
PVD_LBA = 16


class Disc:
    def __init__(self, path: Path, sector_size: int, data_offset: int):
        self.path = path
        self.sector_size = sector_size
        self.data_offset = data_offset
        self._fh = path.open("rb")

    def read_sector(self, lba: int) -> bytes:
        self._fh.seek(lba * self.sector_size + self.data_offset)
        return self._fh.read(USER_DATA)

    def read(self, lba: int, length: int) -> bytes:
        out = bytearray()
        while len(out) < length:
            chunk = self.read_sector(lba)
            if not chunk:
                break
            out += chunk
            lba += 1
        return bytes(out[:length])


def resolve_image(path: Path) -> Path:
    """A .cue is a text index; pull the first FILE line out of it."""
    if path.suffix.lower() != ".cue":
        return path
    text = path.read_text(errors="replace")
    match = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r"FILE\s+(\S+)", text)
    if not match:
        sys.exit(f"no FILE entry found in cue sheet: {path}")
    binpath = path.parent / match.group(1)
    if not binpath.exists():
        sys.exit(f"cue references missing image: {binpath}")
    return binpath


def open_disc(path: Path) -> Disc:
    """Detect sector layout by looking for the ISO9660 magic at sector 16."""
    size = path.stat().st_size
    for sector_size, data_offset in LAYOUTS:
        if size % sector_size:
            continue
        with path.open("rb") as fh:
            fh.seek(PVD_LBA * sector_size + data_offset)
            header = fh.read(6)
        if header[1:6] == b"CD001":
            return Disc(path, sector_size, data_offset)
    sys.exit(f"could not identify a PS1/ISO9660 layout in {path}")


def parse_dir_record(rec: bytes):
    """Decode one ISO9660 directory record. Returns None for padding."""
    if len(rec) < 33 or rec[0] == 0:
        return None
    lba = struct.unpack_from("<I", rec, 2)[0]
    length = struct.unpack_from("<I", rec, 10)[0]
    flags = rec[25]
    name_len = rec[32]
    name = rec[33:33 + name_len]
    if name == b"\x00":
        name = b"."
    elif name == b"\x01":
        name = b".."
    # Strip the ";1" ISO version suffix.
    decoded = name.decode("ascii", errors="replace").split(";")[0]
    return {
        "name": decoded,
        "lba": lba,
        "size": length,
        "is_dir": bool(flags & 0x02),
    }


def walk(disc: Disc, lba: int, size: int, prefix: str = "", depth: int = 0):
    """Recursively yield entries from a directory extent."""
    if depth > 8:
        return
    data = disc.read(lba, size)
    offset = 0
    while offset < len(data):
        rec_len = data[offset]
        if rec_len == 0:
            # Directory records never straddle a sector boundary; a zero length
            # means padding to the end of this sector.
            offset = (offset // USER_DATA + 1) * USER_DATA
            continue
        entry = parse_dir_record(data[offset:offset + rec_len])
        offset += rec_len
        if entry is None or entry["name"] in (".", ".."):
            continue
        path = f"{prefix}/{entry['name']}"
        yield {**entry, "path": path}
        if entry["is_dir"]:
            yield from walk(disc, entry["lba"], entry["size"], path, depth + 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="path to a .cue or .bin/.iso")
    args = parser.parse_args()

    if not args.image.exists():
        sys.exit(f"not found: {args.image}")

    disc = open_disc(resolve_image(args.image))
    print(f"image        : {disc.path}")
    print(f"sector layout: {disc.sector_size} bytes/sector, "
          f"data at +{disc.data_offset}")

    pvd = disc.read_sector(PVD_LBA)
    volume_id = pvd[40:72].decode("ascii", errors="replace").strip()
    print(f"volume id    : {volume_id}")

    root = parse_dir_record(pvd[156:190])
    if root is None:
        sys.exit("could not read the root directory record")

    entries = list(walk(disc, root["lba"], root["size"]))

    print("\n=== SYSTEM.CNF ===")
    cnf = next((e for e in entries if e["name"].upper() == "SYSTEM.CNF"), None)
    if cnf is None:
        print("  (not present -- is this a PS1 disc?)")
    else:
        text = disc.read(cnf["lba"], cnf["size"]).decode("ascii", errors="replace")
        for line in text.splitlines():
            if line.strip():
                print(f"  {line.strip()}")

    print(f"\n=== filesystem ({len(entries)} entries) ===")
    print(f"{'LBA':>8}  {'SIZE':>10}  {'SIZE(hex)':>12}  NAME")
    for entry in sorted(entries, key=lambda e: e["lba"]):
        if entry["is_dir"]:
            size_dec, size_hex = "<DIR>", ""
        else:
            size_dec, size_hex = str(entry["size"]), f"0x{entry['size']:X}"
        print(f"{entry['lba']:>8}  {size_dec:>10}  {size_hex:>12}  {entry['path']}")

    config = verdite_game.get("recompilerConfig", "the recompiler config")
    print("\nNote: the JSON config and the CLI disagree on number bases.")
    print(f"  {config} : 'lba', 'size', 'skip', 'offset' are decimal JSON")
    print("                    numbers; 'base' is a hex STRING (\"0x80011000\").")
    print("  --generate-function-file : -size, -skip, -offset are HEX; -lba is decimal.")
    print("Use the SIZE column for the config and SIZE(hex) for the CLI.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
