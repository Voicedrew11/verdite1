#!/usr/bin/env python3
"""Build patches/Text/MessageGlyphs.cs from the glyph tables in scripts/.

Cuts every KF/TALK picture on the disc, as shipped (Japanese) and with the PPF
applied (English), into runs of inked columns the way patches/Text/PictureText.cs
does, and checks the TSV tables against those runs. Prints counts, refusal
reasons and file names only; no decoded text is ever printed or written apart
from the generated table's hash -> glyph entries. Standard library only.
"""
import argparse
import collections
import os
import re
import struct
import sys

SECTOR = 2352
USER_OFF = 24
USER = 2048
PPF_MARKER = b"@BEGIN_FILE_ID.DIZ"
PPF_BLOCK_OFF = 0x9320
PICTURE_DIR = "KF/TALK/"
W, H = 256, 128
FNV_OFF = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
HASH_RE = re.compile(r"[0-9a-f]{16}")
INT_RE = re.compile(r"-?[0-9]+")

# Japanese: fixed lines; English: lines found by ink and placed by baseline.
JAPANESE = {"name": "Japanese", "fixed": True, "top": 9, "pitch": 15, "height": 12, "rowbytes": 2}
ENGLISH = {"name": "English", "fixed": False, "height": 16, "up": 11, "down": 4, "rowbytes": 4}


# ---------------------------------------------------------------- disc / ISO 9660

class Disc:
    def __init__(self, data):
        self.data = data

    def read(self, lba, size):
        out = bytearray()
        for i in range((size + USER - 1) // USER):
            off = (lba + i) * SECTOR + USER_OFF
            out += self.data[off:off + USER]
        return bytes(out[:size])

    def user_sector(self, lba):
        off = lba * SECTOR + USER_OFF
        return self.data[off:off + USER]


def parse_iso(disc):
    pvd = disc.user_sector(16)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise SystemExit("no ISO 9660 primary volume descriptor at sector 16")
    root = pvd[156:190]
    root_lba = struct.unpack_from("<I", root, 2)[0]
    root_size = struct.unpack_from("<I", root, 10)[0]
    files = []

    def walk(lba, size, path, depth):
        if depth > 16:
            return
        data = disc.read(lba, size)
        pos = 0
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // USER + 1) * USER  # records do not cross sectors
                continue
            rec = data[pos:pos + ln]
            pos += ln
            ext = struct.unpack_from("<I", rec, 2)[0]
            dsize = struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            if nlen == 1 and name in (b"\x00", b"\x01"):
                continue  # "." and ".."
            nm = name.decode("ascii", "replace").split(";")[0]
            p = f"{path}/{nm}" if path else nm
            if flags & 2:
                walk(ext, dsize, p, depth + 1)
            else:
                files.append({"path": p, "lba": ext, "size": dsize})

    walk(root_lba, root_size, "", 0)
    return files


# ---------------------------------------------------------------- PPF3

def load_ppf(path):
    d = open(path, "rb").read()
    if d[:5] != b"PPF30":
        raise SystemExit("not a PPF3 file")
    info = {
        "description": d[6:56].rstrip(b" \x00").decode("latin-1", "replace"),
        "blockcheck": d[57], "undo": d[58],
    }
    pos = 60
    block = None
    if info["blockcheck"]:
        block = d[pos:pos + 1024]
        pos += 1024
    recs = []
    while pos + 9 <= len(d) and d[pos:pos + len(PPF_MARKER)] != PPF_MARKER:
        off = struct.unpack_from("<Q", d, pos)[0]
        ln = d[pos + 8]
        pos += 9
        data = d[pos:pos + ln]
        if len(data) != ln:
            raise SystemExit("PPF record runs past end of file")
        pos += ln
        if info["undo"]:
            pos += ln
        recs.append((off, data))
    info["block"] = block
    info["records"] = len(recs)
    return info, recs


def apply_ppf(orig, recs):
    patched = bytearray(orig)
    for off, data in recs:
        if off + len(data) > len(patched):
            raise SystemExit("PPF record past end of disc")
        patched[off:off + len(data)] = data
    return patched


# ---------------------------------------------------------------- TIM -> ink

def lit(c):
    """True when a 15-bit colour's luma is above 128 (ink)."""
    r = (c & 31) * 255 // 31
    g = ((c >> 5) & 31) * 255 // 31
    b = ((c >> 10) & 31) * 255 // 31
    return (r * 299 + g * 587 + b * 114) // 1000 > 128


def u32(buf, p):
    return struct.unpack_from("<I", buf, p)[0]


def tim_ink(buf):
    """Ink of the first TIM's top W x H pixels, row-major bytes (1 = ink)."""
    if len(buf) < 8 or u32(buf, 0) != 0x10:
        return None, "not a TIM"
    flags = u32(buf, 4)
    if flags & 3 != 0 or not flags & 8:
        return None, "not a 4-bit TIM with CLUT"
    try:
        p = 8
        blen, _cx, _cy, cw, _ch = struct.unpack_from("<IHHHH", buf, p)
        if blen < 12 or p + blen > len(buf):
            return None, "truncated TIM"
        pal = struct.unpack_from("<%dH" % cw, buf, p + 12)
        p += blen
        _ilen, _ix, _iy, iw, ih = struct.unpack_from("<IHHHH", buf, p)
    except struct.error:
        return None, "truncated TIM"
    if iw * 4 != W or ih < H:
        return None, "not a 256-wide picture"
    row = iw * 2
    start = p + 12
    if start + row * H > len(buf):
        return None, "truncated TIM"
    table = [lit(c) for c in pal]
    ink = bytearray()
    for y in range(H):
        base = start + y * row
        for b in buf[base:base + row]:  # low nibble is the left pixel
            lo, hi = b & 15, b >> 4
            ink.append(table[lo] if lo < len(table) else 0)
            ink.append(table[hi] if hi < len(table) else 0)
    return ink, None


# ---------------------------------------------------------------- cutting (as PictureText.cs)

def fnv(h, b):
    return ((h ^ b) * FNV_PRIME) & MASK64


def pair_key(lk, rk, gap):
    k = FNV_OFF
    for v in (lk, rk):
        for b in range(8):
            k = fnv(k, (v >> (8 * b)) & 0xFF)
    return fnv(k, gap & 0xFF)


def fixed_bands(ink, f):
    bands = []
    y0 = f["top"]
    while y0 + f["height"] <= H:
        bands.append((y0, y0 + f["height"] - 1))
        y0 += f["pitch"]
    inside = set()
    for a, b in bands:
        inside.update(range(a, b + 1))
    for y in range(H):
        if y not in inside and any(ink[y * W:(y + 1) * W]):
            return None
    return bands


def found_bands(ink, fit):
    bands = []
    start = -1
    for y in range(H + 1):
        any_ink = y < H and any(ink[y * W:(y + 1) * W])
        if any_ink and start < 0:
            start = y
        elif not any_ink and start >= 0:
            if bands and start - bands[-1][1] == 2 and y - bands[-1][0] <= fit:
                bands[-1] = (bands[-1][0], y - 1)
            else:
                bands.append((start, y - 1))
            start = -1
    return bands


def runs_of(ink, y0, y1):
    cols = [any(ink[y * W + x] for y in range(y0, y1 + 1)) for x in range(W)]
    runs = []
    start = None
    for x, on in enumerate(cols + [False]):
        if on and start is None:
            start = x
        elif not on and start is not None:
            runs.append((start, x - 1))
            start = None
    return runs


def baseline(ink, y0, y1, runs):
    votes = collections.Counter()
    for L, R in runs:
        bottom = y0
        for y in range(y0, y1 + 1):
            if any(ink[y * W + L:y * W + R + 1]):
                bottom = y
        votes[bottom] += 1
    return max(votes.items(), key=lambda kv: (kv[1], kv[0]))[0]


def key_of(ink, L, R, top, f):
    width = R - L + 1
    k = fnv(FNV_OFF, width & 0xFF)
    rows = []
    for r in range(f["height"]):
        y = top + r
        bits = 0
        if 0 <= y < H:
            for x in range(L, R + 1):
                if ink[y * W + x]:
                    bits |= 1 << ((x - L) & 31)
        bits &= 0xFFFFFFFF
        rows.append(bits)
        for b in range(f["rowbytes"]):
            k = fnv(k, (bits >> (8 * b)) & 0xFF)
    return k, width, tuple(rows)


def cut(ink, f):
    """Bands in order, each a list of (L, R, key, width, rows); and the error that stopped the cut."""
    if f["fixed"]:
        bands = fixed_bands(ink, f)
        if bands is None:
            return [], "ink between the lines"
    else:
        bands = found_bands(ink, f["height"])
    out = []
    for y0, y1 in bands:
        runs = runs_of(ink, y0, y1)
        if not runs:
            continue
        if f["fixed"]:
            top = y0
        else:
            base = baseline(ink, y0, y1, runs)
            top = base - f["up"]
            if y0 < top or y1 > base + f["down"]:
                return out, "ink outside the line"
        out.append([(L, R) + key_of(ink, L, R, top, f) for L, R in runs])
    return out, None


def read_picture(ink, f, runs_tab, pairs_tab, scan=False):
    """(status, unknown run keys). Status is 'ok' or the first refusal reason.
    With scan, unknown runs are collected and skipped rather than stopping."""
    bands, err = cut(ink, f)
    if err == "ink between the lines":
        return err, []
    unknown = []
    for keys in bands:
        i = 0
        while i < len(keys):
            L, R, k, _, _ = keys[i]
            if i + 1 < len(keys) and pairs_tab:
                nL, _, nk, _, _ = keys[i + 1]
                if pair_key(k, nk, nL - R - 1) in pairs_tab:
                    i += 2
                    continue
            if k not in runs_tab:
                if not scan:
                    return "unknown run", [k]
                unknown.append(k)
            i += 1
    if err:
        return err, unknown
    if not bands:
        return "blank", unknown
    return ("unknown run" if unknown else "ok"), unknown


def observe(pictures, f):
    """Runs and adjacent-run pairs seen in the cut of every picture."""
    obs_runs = collections.defaultdict(set)
    obs_pairs = collections.defaultdict(set)
    for ink in pictures:
        bands, _ = cut(ink, f)
        for keys in bands:
            for i, (L, R, k, w, rows) in enumerate(keys):
                obs_runs[k].add((w, rows))
                if i + 1 < len(keys):
                    nL, _, nk, _, _ = keys[i + 1]
                    gap = nL - R - 1
                    obs_pairs[pair_key(k, nk, gap)].add((k, nk, gap))
    return obs_runs, obs_pairs


# ---------------------------------------------------------------- TSV tables

def load_table(path, tag, errors):
    runs, pairs = {}, {}
    try:
        lines = open(path, encoding="utf-8").read().split("\n")
    except OSError:
        errors.append(f"{tag}: cannot read table")
        return runs, pairs
    for n, line in enumerate(lines, 1):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "pair":
            if len(parts) < 5 or not HASH_RE.fullmatch(parts[1]) or not HASH_RE.fullmatch(parts[2]) \
                    or not INT_RE.fullmatch(parts[3]) or not parts[4]:
                errors.append(f"{tag} line {n}: malformed pair line")
                continue
            lk, rk, gap = int(parts[1], 16), int(parts[2], 16), int(parts[3])
            pk = pair_key(lk, rk, gap)
            if pk in pairs:
                errors.append(f"{tag} line {n}: duplicate pair")
            pairs[pk] = (lk, rk, gap, "\t".join(parts[4:]))
        else:
            if len(parts) < 3 or not HASH_RE.fullmatch(parts[0]) or not INT_RE.fullmatch(parts[1]) \
                    or not parts[2]:
                errors.append(f"{tag} line {n}: malformed run line")
                continue
            k = int(parts[0], 16)
            if k in runs:
                errors.append(f"{tag} line {n}: duplicate run hash")
            runs[k] = (int(parts[1]), "\t".join(parts[2:]))
    return runs, pairs


def cs_str(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 0x20 <= o < 0x7F:
            out.append(ch)
        else:
            for unit in struct.unpack(">%dH" % (len(ch.encode("utf-16-be")) // 2), ch.encode("utf-16-be")):
                out.append("\\u%04x" % unit)
    return '"' + "".join(out) + '"'


# ---------------------------------------------------------------- checks

def check_language(f, tsv, pictures, names, errors, print_names=True):
    """Round trip, collisions, coverage for one font. pictures: ink per name (None = not a TIM)."""
    tag = f["name"]
    runs_tab, pairs_tab = tsv
    real = [p for p in pictures if p is not None]
    obs_runs, obs_pairs = observe(real, f)

    print(f"[{tag}] runs seen: {len(obs_runs)}, adjacent pairs seen: {len(obs_pairs)}")
    coll = [k for k, s in obs_runs.items() if len(s) > 1]
    pcoll = [k for k, s in obs_pairs.items() if len(s) > 1]
    print(f"[{tag}] hash collisions: {len(coll)} run, {len(pcoll)} pair")
    for k in coll:
        errors.append(f"{tag}: run hash collision {k:016x}")
    for k in pcoll:
        errors.append(f"{tag}: pair hash collision {k:016x}")

    miss_runs = []
    width_bad = []
    for k, (w, _) in runs_tab.items():
        if k not in obs_runs:
            miss_runs.append(k)
        elif w not in {ww for ww, _ in obs_runs[k]}:
            width_bad.append(k)
    miss_pairs = []
    for pk, (lk, rk, _, _) in pairs_tab.items():
        if pk not in obs_pairs or lk not in obs_runs or rk not in obs_runs:
            miss_pairs.append(pk)
    print(f"[{tag}] round trip: runs {len(runs_tab) - len(miss_runs)}/{len(runs_tab)} found, "
          f"width ok {len(runs_tab) - len(miss_runs) - len(width_bad)}, "
          f"pairs {len(pairs_tab) - len(miss_pairs)}/{len(pairs_tab)} found")
    for k in miss_runs:
        errors.append(f"{tag}: table run not on disc {k:016x}")
    for k in width_bad:
        errors.append(f"{tag}: table width differs from disc {k:016x}")
    for pk in miss_pairs:
        errors.append(f"{tag}: table pair not on disc {pk:016x}")

    # Coverage.
    counts = collections.Counter()
    refused = []
    unknown_occ = collections.Counter()
    unknown_pics = 0
    read_ok = 0
    for name, ink in zip(names, pictures):
        if ink is None:
            refused.append(name)
            continue
        status, _ = read_picture(ink, f, runs_tab, pairs_tab)
        if status == "ok":
            read_ok += 1
        else:
            counts[status] += 1
            refused.append(name)
        _, unk = read_picture(ink, f, runs_tab, pairs_tab, scan=True)
        if unk:
            unknown_pics += 1
            unknown_occ.update(unk)
    tim_refused = [n for n, p in zip(names, pictures) if p is None]
    print(f"[{tag}] pictures read fully: {read_ok} / {len(names)}")
    for kind, c in sorted(counts.items()):
        if c:
            print(f"    refused, {kind}: {c}")
    if tim_refused:
        print(f"    refused, not a usable TIM: {len(tim_refused)}")
    if unknown_occ:
        print(f"[{tag}] unknown runs (all pictures, pair-aware): distinct {len(unknown_occ)}, "
              f"occurrences {sum(unknown_occ.values())}, in {unknown_pics} pictures")
    else:
        print(f"[{tag}] unknown runs: none")
    if print_names and refused:
        print(f"[{tag}] refused files:")
        for n in refused:
            print(f"    {n}")
    return read_ok, refused


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--disc", default="disc/KingsField1.bin")
    ap.add_argument("--ppf", default="disc/KF Jap to Eng v1.0.ppf")
    ap.add_argument("--out", default="patches/Text/MessageGlyphs.cs")
    ap.add_argument("--check", action="store_true", help="verify only; do not write")
    args = ap.parse_args()

    def path(p):
        return p if os.path.isabs(p) else os.path.join(root, p)

    disc_path = path(args.disc)
    if not os.path.isfile(disc_path):
        print(f"disc not found: {args.disc}")
        return 2
    disc_bytes = open(disc_path, "rb").read()
    disc = Disc(disc_bytes)
    files = parse_iso(disc)
    wanted = sorted((f for f in files if f["path"].upper().startswith(PICTURE_DIR.upper())),
                    key=lambda f: f["path"].upper())
    names = [f["path"] for f in wanted]
    print(f"ISO files: {len(files)}; {PICTURE_DIR} files: {len(wanted)}")

    errors = []
    jp_pics = []
    for f in wanted:
        ink, why = tim_ink(disc.read(f["lba"], f["size"]))
        jp_pics.append(ink)
    print(f"Japanese pictures usable as TIMs: {sum(p is not None for p in jp_pics)} / {len(wanted)}")

    en_pics = None
    ppf_path = path(args.ppf)
    if os.path.isfile(ppf_path):
        info, recs = load_ppf(ppf_path)
        if info["block"] is not None and disc_bytes[PPF_BLOCK_OFF:PPF_BLOCK_OFF + 1024] != info["block"]:
            errors.append("PPF block check does not match the disc: English skipped")
        else:
            patched = Disc(apply_ppf(disc_bytes, recs))
            en_pics = []
            for f in wanted:
                ink, _ = tim_ink(patched.read(f["lba"], f["size"]))
                en_pics.append(ink)
            print(f"PPF applied: {info['records']} records; English pictures usable as TIMs: "
                  f"{sum(p is not None for p in en_pics)} / {len(wanted)}")
    else:
        print("PPF not found: English checks skipped")

    jp_runs, jp_pairs = load_table(path("scripts/msg_glyphs_jp.tsv"), "jp tsv", errors)
    en_runs, en_pairs = load_table(path("scripts/msg_glyphs_en.tsv"), "en tsv", errors)
    print(f"Tables: jp {len(jp_runs)} runs, {len(jp_pairs)} pairs; en {len(en_runs)} runs, {len(en_pairs)} pairs")

    print()
    jp_read, _ = check_language(JAPANESE, (jp_runs, jp_pairs), jp_pics, names, errors)
    en_read = None
    if en_pics is not None:
        print()
        en_read, _ = check_language(ENGLISH, (en_runs, en_pairs), en_pics, names, errors)
        print()
        cross_en = sum(1 for p in en_pics if p is not None and read_picture(p, JAPANESE, jp_runs, jp_pairs)[0] == "ok")
        cross_jp = sum(1 for p in jp_pics if p is not None and read_picture(p, ENGLISH, en_runs, en_pairs)[0] == "ok")
        print(f"Cross: English pictures the Japanese font reads: {cross_en} / {len(wanted)}")
        print(f"Cross: Japanese pictures the English font reads: {cross_jp} / {len(wanted)}")
    else:
        print("\nEnglish table not verified against the disc (no PPF).")

    print()
    if errors:
        print(f"ERRORS: {len(errors)}")
        for e in errors:
            print("  " + e)
        print("not written")
        return 1
    if args.check:
        print("check passed; not written")
        return 0

    out_path = path(args.out)
    lines = [
        "// Generated by scripts/msg_glyphs.py from scripts/msg_glyphs_jp.tsv and msg_glyphs_en.tsv; do not edit.",
        "namespace Kf1;",
        "",
        "static class MessageGlyphs",
        "{",
    ]
    tables = [
        ("JapaneseRuns", jp_runs, None),
        ("JapanesePairs", None, jp_pairs),
        ("EnglishRuns", en_runs, None),
        ("EnglishPairs", None, en_pairs),
    ]
    for name, runs, pairs in tables:
        lines.append(f"    public static readonly Dictionary<ulong, string> {name} = new()")
        lines.append("    {")
        if runs is not None:
            for k in sorted(runs):
                lines.append(f"        [0x{k:016x}UL] = {cs_str(runs[k][1])},")
        else:
            for pk in sorted(pairs):
                lines.append(f"        [0x{pk:016x}UL] = {cs_str(pairs[pk][3])},")
        lines.append("    };")
        lines.append("")
    lines[-1:] = ["}"]
    data = "\n".join(lines) + "\n"
    data.encode("ascii")  # must be pure ASCII
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="ascii", newline="\n") as fh:
        fh.write(data)
    print(f"generated {os.path.relpath(out_path, root)}: {len(lines)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
