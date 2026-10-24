#!/usr/bin/env python3
"""Writes the disc images libverdite's disc_image tests read: a made-up 33-sector disc (no game
data) as a raw MODE2/2352 .bin, as a 2048-byte .iso, and as CHDs made from them with chdman, one
per CD codec. Run it again only to change the fixtures; the outputs are committed.

    python3 tools/make_disc_fixtures.py
"""
import pathlib
import subprocess
import tempfile

OUT = pathlib.Path(__file__).resolve().parent.parent / "libverdite" / "tests" / "data" / "disc"
# Not a multiple of the 8 frames in a hunk, so the last hunk is partial. Some sizes (8, 20, 24)
# give a CHD whose map fails its own CRC in chdman 0.289 and libchdr alike: chdman's map writer.
SECTORS = 33
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])


def payload(sector: int) -> bytes:
    # The last four sectors are noise, the rest mostly runs.
    state = 0x9E3779B9 ^ sector
    noise = bytearray()
    for _ in range(2048):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        noise.append(state >> 24)
    if sector >= SECTORS - 4:
        return bytes(noise)
    return bytes(noise[:128]) + bytes((sector + i // 64) & 0xFF for i in range(1920))


def bcd(value: int) -> int:
    return (value // 10) << 4 | value % 10


def edc(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xD8018001 if crc & 1 else 0)
    return crc


def xtime(value: int) -> int:
    return ((value << 1) ^ (0x1D if value & 0x80 else 0)) & 0xFF


ECC_HIGH = [0] * 256
for _value in range(256):
    ECC_HIGH[xtime(_value) ^ _value] = _value


def ecc_bytes(data: bytes, offsets) -> tuple[int, int]:
    # As libchdr's ecc_compute_bytes; in Mode 2 the header's four bytes count as zero.
    low = high = 0
    for offset in offsets:
        byte = 0 if offset < 4 else data[offset]
        low = xtime(low ^ byte)
        high ^= byte
    low = ECC_HIGH[xtime(low) ^ high]
    return low, high ^ low


def raw_sector(sector: int, data: bytes) -> bytes:
    # Real EDC and ECC: chdman's CD codecs drop ECC they can regenerate, the path worth testing,
    # and cannot read back a raw sector whose ECC is wrong.
    lba = sector + 150
    header = bytes([bcd(lba // 4500), bcd(lba // 75 % 60), bcd(lba % 75), 2])
    subheader = bytes([0, 0, 0x08, 0]) * 2  # Form 1 data
    body = bytearray(SYNC + header + subheader + data)
    body += edc(body[16:2072]).to_bytes(4, "little")
    body += bytes(2352 - len(body))
    data = body[12:]  # what P and Q are computed over
    for row in range(86):
        body[2076 + row], body[2076 + 86 + row] = ecc_bytes(data, (row + 86 * c for c in range(24)))
    data = body[12:]
    for row in range(52):
        offsets = ((86 * (row >> 1) + (row & 1) + 88 * c) % 2236 for c in range(43))
        body[2248 + row], body[2248 + 52 + row] = ecc_bytes(data, offsets)
    return bytes(body)


def chdman(*args: str) -> None:
    subprocess.run(["chdman", *args, "-f"], check=True, capture_output=True)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "made_up.bin").write_bytes(b"".join(raw_sector(s, payload(s)) for s in range(SECTORS)))
    (OUT / "made_up.iso").write_bytes(b"".join(payload(s) for s in range(SECTORS)))
    (OUT / "made_up.cue").write_text('FILE "made_up.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    cue = str(OUT / "made_up.cue")
    for codec in ("cdlz", "cdzl", "cdfl", "cdzs"):
        chdman("createcd", "-i", cue, "-o", str(OUT / f"made_up_{codec}.chd"), "-c", codec)
    chdman("createcd", "-i", str(OUT / "made_up.iso"), "-o", str(OUT / "made_up_iso.chd"))
    # Only the differences from made_up_cdlz.chd: unreadable on its own.
    with tempfile.TemporaryDirectory() as scratch:
        changed = pathlib.Path(scratch) / "changed.bin"
        sectors = [payload(s) for s in range(SECTORS)]
        sectors[3] = bytes(reversed(sectors[3]))
        changed.write_bytes(b"".join(raw_sector(s, data) for s, data in enumerate(sectors)))
        (pathlib.Path(scratch) / "changed.cue").write_text(
            'FILE "changed.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        chdman("createcd", "-i", str(pathlib.Path(scratch) / "changed.cue"),
               "-o", str(OUT / "made_up_child.chd"), "-op", str(OUT / "made_up_cdlz.chd"))
        # Audio first: a CD whose data is not its first track.
        (pathlib.Path(scratch) / "audio.bin").write_bytes(bytes(i * 7 & 0xFF for i in range(2352 * SECTORS)))
        (pathlib.Path(scratch) / "audio.cue").write_text(
            'FILE "audio.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')
        chdman("createcd", "-i", str(pathlib.Path(scratch) / "audio.cue"), "-o", str(OUT / "audio_first.chd"))


if __name__ == "__main__":
    main()
