#!/usr/bin/env python3
"""Extract a file from a PS1 disc image and, if it is a PS-X EXE, print its header.

The header is what the recompiler needs: `-base` for a linear sweep must be the
executable's real load address, not an assumed one.

Usage:
    python3 scripts/extract_file.py disc/GAME.cue GAME.EXE -o build/GAME.EXE
    python3 scripts/extract_file.py disc/GAME.cue GAME.EXE --header-only
"""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_disc import open_disc, parse_dir_record, resolve_image, walk, PVD_LBA

EXE_MAGIC = b"PS-X EXE"
EXE_HEADER_SIZE = 0x800


def find_entry(disc, name: str):
    """Locate a file by name or full path, case-insensitively."""
    pvd = disc.read_sector(PVD_LBA)
    root = parse_dir_record(pvd[156:190])
    if root is None:
        sys.exit("could not read the root directory record")

    wanted = name.upper().lstrip("/")
    matches = [
        e for e in walk(disc, root["lba"], root["size"])
        if not e["is_dir"]
        and (e["name"].upper() == wanted or e["path"].upper().lstrip("/") == wanted)
    ]
    if not matches:
        sys.exit(f"not found on disc: {name}")
    if len(matches) > 1:
        paths = ", ".join(m["path"] for m in matches)
        sys.exit(f"ambiguous name {name!r}; use a full path. matches: {paths}")
    return matches[0]


def describe_exe(data: bytes, label: str) -> None:
    """Print the PS-X EXE header fields relevant to recompilation."""
    if data[:8] != EXE_MAGIC:
        print(f"  {label}: not a PS-X EXE (magic {data[:8]!r})")
        return

    pc, gp, t_addr, t_size = struct.unpack_from("<IIII", data, 0x10)
    d_addr, d_size, b_addr, b_size = struct.unpack_from("<IIII", data, 0x20)
    s_addr, s_size = struct.unpack_from("<II", data, 0x30)

    print(f"  magic        : PS-X EXE")
    print(f"  entry pc     : 0x{pc:08X}")
    print(f"  initial gp   : 0x{gp:08X}")
    print(f"  text addr    : 0x{t_addr:08X}   <-- use as -base")
    print(f"  text size    : 0x{t_size:X} ({t_size} bytes)")
    print(f"  text range   : 0x{t_addr:08X} - 0x{t_addr + t_size:08X}")
    if d_size:
        print(f"  data addr    : 0x{d_addr:08X}  size 0x{d_size:X}")
    if b_size:
        print(f"  bss  addr    : 0x{b_addr:08X}  size 0x{b_size:X}")
    print(f"  stack        : 0x{s_addr:08X}  size 0x{s_size:X}")
    print(f"  payload      : {EXE_HEADER_SIZE} byte header + "
          f"{len(data) - EXE_HEADER_SIZE} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="path to a .cue or .bin/.iso")
    parser.add_argument("name", help="file name or full path on the disc")
    parser.add_argument("-o", "--out", type=Path, help="write the file here")
    parser.add_argument("--header-only", action="store_true",
                        help="print the PS-X EXE header without writing a file")
    args = parser.parse_args()

    if not args.image.exists():
        sys.exit(f"not found: {args.image}")

    disc = open_disc(resolve_image(args.image))
    entry = find_entry(disc, args.name)

    print(f"{entry['path']}  lba={entry['lba']}  "
          f"size={entry['size']} (0x{entry['size']:X})")

    data = disc.read(entry["lba"], entry["size"])
    describe_exe(data, entry["path"])

    if args.out and not args.header_only:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(data)
        print(f"  wrote        : {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
