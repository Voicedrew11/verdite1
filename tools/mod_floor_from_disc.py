#!/usr/bin/env python3
"""Copy one of the disc's floors into a mod's folder, as a floor kf.content.floor accepts (docs/MODDING.md).

Takes out what a mod's floor cannot have yet: the people and events, whose pictures and pages are disc
files, and the signs and inscriptions, which become the item --sign-item names (a Moonstone by default). The
copy is the player's own disc data: it stays on their machine, as replacing any disc file does. A start for
editing, and the floor the tests and the fuzzer play.

Usage: mod_floor_from_disc.py DATA/KF/B1 MODS/mymod/floors/crypt [--sign-item 41]
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

MIXA_CHUNKS = 8
OBJECT_CHUNK, EVENT_CHUNK = 4, 7
OBJECT_BYTES = 20
SIGN_IDS = {130, 131}   # KF_MAP_OBJECT_SIGNBOARD, KF_MAP_OBJECT_INSCRIPTION_PANEL
LIST_END = 255


def chunks(data, count):
    at = 0
    out = []
    for _ in range(count):
        size = struct.unpack_from("<I", data, at)[0]
        out.append((at + 4, size))
        at += 4 + size
    if at > len(data):
        raise ValueError("a chunk runs past the end of the file")
    return out


def strip_mixa(data, sign_item):
    """data with the signs swapped and the events ended, chunk sizes unchanged."""
    out = bytearray(data)
    found = chunks(out, MIXA_CHUNKS)
    objects_at, objects_size = found[OBJECT_CHUNK]
    for at in range(objects_at, objects_at + objects_size, OBJECT_BYTES):
        if out[at] == LIST_END:
            break
        if out[at] in SIGN_IDS:
            out[at] = sign_item
    out[found[EVENT_CHUNK][0]] = LIST_END
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("disc_floor", type=Path)
    parser.add_argument("dest", type=Path)
    parser.add_argument("--sign-item", type=int, default=41)
    args = parser.parse_args()
    if not 0 <= args.sign_item < 80:
        parser.error("--sign-item is a disc item, 0 to 79")
    args.dest.mkdir(parents=True, exist_ok=True)
    for name in ("MIX.TIM", "MIXB.DAT", "SND0.SEQ"):
        shutil.copy(args.disc_floor / name, args.dest / name)
    (args.dest / "MIXA.DAT").write_bytes(strip_mixa((args.disc_floor / "MIXA.DAT").read_bytes(), args.sign_item))
    return 0


if __name__ == "__main__":
    sys.exit(main())
