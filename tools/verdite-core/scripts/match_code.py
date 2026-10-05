#!/usr/bin/env python3
"""Find a function's counterpart in another game's executable, by structure.

match_overlays.py carries a routine between three links of the *same* code, where
the instruction words differ only in relocated addresses. Across two games that
is not true: the studio's engine is one lineage, but a routine a year later has
moved registers, gained a branch, changed a record stride or been inlined into its
caller. So this compares what survives those changes, each part scored on its own
so a reader can see why two routines matched:

  * the opcode sequence (an instruction's class, registers dropped)
  * the GTE commands, in order, and the GTE registers moved in and out; a call to
    a named libgte routine counts as the commands it runs (compared as a bag
    then, since the call's place in the sequence is lost)
  * the field offsets loaded and stored through a pointer (a record's layout)
  * the small constants (masks, counts, shifts)
  * size, loop count and the shape of the calls

How it fails. Library code links differently in each game (another PSY-Q), so a
match inside the library is weak evidence; a routine split or merged differently
by the two linear sweeps scores low against its own counterpart; and an inlined
GTE macro in one game against a libgte call in the other changes the GTE
sequence and the call shape at once. A high score is a candidate to read, never
an identification on its own.

Each game is a repo root holding config/verdite.json (the disc, the recompiler
config and the function maps). Side A defaults to the repo this is run in.

Usage:
    # the five nearest counterparts in B of a function in A
    match_code.py match --b ~/Desktop/verdite3 0x80030540

    # align two routines' calls, and their callees', two levels deep
    match_code.py tree --b ~/Desktop/verdite3 0x800342D8 0x800422B8 --depth 2

    # score known pairs, one "addrA addrB" a line
    match_code.py pairs --b ~/Desktop/verdite3 pairs.txt

    # one function's fingerprint
    match_code.py show 0x80030540

    # an executable already extracted, instead of reading the disc
    match_code.py match --exe-a scratch/GAME.EXE --b ../other --exe-b ../other/scratch/GAME.EXE 0x...
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verdite_game  # noqa: E402

GTE = {
    0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL", 0x12: "MVMVA",
    0x13: "NCDS", 0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS", 0x1C: "CC", 0x1E: "NCS",
    0x20: "NCT", 0x28: "SQR", 0x29: "DCPL", 0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4",
    0x30: "RTPT", 0x3D: "GPF", 0x3E: "GPL", 0x3F: "NCCT",
}

OPS = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
    0x0E: "xori", 0x0F: "lui", 0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw",
    0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl",
    0x2B: "sw", 0x2E: "swr", 0x32: "lwc2", 0x3A: "swc2",
}
FUNCTS = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break", 0x10: "mfhi", 0x11: "mthi",
    0x12: "mflo", 0x13: "mtlo", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or",
    0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}
LOADS = {0x20: 1, 0x21: 2, 0x22: 4, 0x23: 4, 0x24: 1, 0x25: 2, 0x26: 4, 0x32: 4}
STORES = {0x28: 1, 0x29: 2, 0x2A: 4, 0x2B: 4, 0x2E: 4, 0x3A: 4}
IMM_OPS = {0x0A, 0x0B, 0x0C, 0x0D}
# Opcodes whose low 16 bits are an address-bearing immediate (match_overlays').
MASK_IMM_OPS = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x20, 0x21, 0x22, 0x23,
                0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A}
ZERO, AT, GP, SP = 0, 1, 28, 29
EXE_HEADER = 0x800

# libgte entry points by their PSY-Q name, as the GTE commands they run: one game
# calls the library where the other inlines the macro, and both should read alike.
LIBGTE = {
    "RotTransPers": ["RTPS"], "RotTransPers3": ["RTPT"], "RotTransPers4": ["RTPT", "RTPS"],
    "RotTrans": ["MVMVA"], "NormalClip": ["NCLIP"], "AverageZ3": ["AVSZ3"], "AverageZ4": ["AVSZ4"],
    "NormalColor": ["NCS"], "NormalColor3": ["NCT"], "NormalColorDpq": ["NCDS"],
    "NormalColorDpq3": ["NCDT"], "NormalColorCol": ["NCCS"], "NormalColorCol3": ["NCCT"],
    "ColorDpq": ["CDP"], "ColorCol": ["CC"], "DpqColor": ["DPCS"], "DpqColor3": ["DPCT"],
    "DpqColorLight": ["DCPL"], "LightColor": ["MVMVA"], "Intpl": ["INTPL"],
    "OuterProduct0": ["OP"], "OuterProduct12": ["OP"], "Square0": ["SQR"], "Square12": ["SQR"],
    "MulMatrix0": ["MVMVA"] * 3, "MulMatrix": ["MVMVA"] * 3, "MulMatrix2": ["MVMVA"] * 3,
    "ApplyMatrix": ["MVMVA"], "ApplyMatrixSV": ["MVMVA"], "ApplyRotMatrix": ["MVMVA"],
}

WEIGHTS = {"ops": 0.35, "gte": 0.20, "fields": 0.20, "imms": 0.10, "size": 0.10, "calls": 0.05}


def strip_jsonc(text: str) -> str:
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            out.append(ch)
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
            continue
        else:
            out.append(ch)
        i += 1
    return re.sub(r",(\s*[\]}])", r"\1", "".join(out))


def token(w: int) -> str:
    op = w >> 26
    if op == 0:
        return FUNCTS.get(w & 0x3F, f"sp{w & 0x3F:02x}")
    if op == 1:
        return ("bltz", "bgez", "bltzal", "bgezal")[((w >> 16) & 1) | ((w >> 19) & 2)]
    if op == 0x12:
        if (w >> 25) & 1:
            return "gte:" + GTE.get(w & 0x3F, f"{w & 0x3F:02x}")
        return ("mfc2", "?", "cfc2", "?", "mtc2", "?", "ctc2")[(w >> 21) & 7] if (w >> 21) & 0x1F < 7 else "cop2"
    return OPS.get(op, f"op{op:02x}")


class Function:
    def __init__(self, addr: int, name: str, words: tuple[int, ...]):
        self.addr, self.name, self.words = addr, name, words
        self.size = len(words) * 4
        self.ops = [token(w) for w in words]
        self.norm = tuple(normalize(w) for w in words)
        self.gte: list[str] = []
        self.gte_regs: Counter = Counter()
        self.fields: set[tuple[str, int, int]] = set()
        self.imms: set[int] = set()
        self.calls: list[int] = []
        self.jalr = 0
        self.loops = 0
        lui_regs: set[int] = set()
        for i, w in enumerate(words):
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 3:
                self.calls.append(0x80000000 | ((w & 0x3FFFFFF) << 2))
            elif op == 0 and (w & 0x3F) == 9:
                self.jalr += 1
            elif op == 0x12:
                if (w >> 25) & 1:
                    cmd = GTE.get(w & 0x3F, f"{w & 0x3F:02x}")
                    if cmd == "MVMVA":
                        cmd += f"(sf{(w >> 19) & 1} mx{(w >> 17) & 3} v{(w >> 15) & 3} cv{(w >> 13) & 3} lm{(w >> 10) & 1})"
                    self.gte.append(cmd)
                elif rs in (0, 2, 4, 6):
                    kind = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}[rs]
                    self.gte_regs[f"{kind}:{rd}"] += 1
            elif op in (0x32, 0x3A):
                self.gte_regs[f"{OPS[op]}:{rt}"] += 1
            if op in LOADS or op in STORES:
                if rs not in (ZERO, GP, SP, AT) and rs not in lui_regs:
                    kind = "ld" if op in LOADS else "st"
                    self.fields.add((kind, LOADS.get(op) or STORES[op], simm))
            if op in IMM_OPS or (op == 0x09 and rs not in lui_regs and abs(simm) < 0x1000):
                if abs(simm) < 0x1000 or op in (0x0C, 0x0D):
                    self.imms.add(imm if op in (0x0C, 0x0D) else simm)
            if op == 0 and (w & 0x3F) in (0, 2, 3) and w:
                self.imms.add(0x10000 + ((w >> 6) & 31))  # a shift amount, kept apart
            if op in (1, 4, 5, 6, 7) and simm < 0:
                self.loops += 1
            # Track registers holding a lui'd upper half (a global's address).
            dest = None
            if op == 0x0F:
                lui_regs.add(rt)
                continue
            if op == 0:
                dest = rd
            elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E) or op in LOADS:
                dest = rt
                if op in (0x09, 0x0D) and rs in lui_regs and rs == rt:
                    continue  # lui+addiu/ori into the same register: still an address
            if dest is not None:
                lui_regs.discard(dest)

    def summary(self) -> dict:
        return {
            "address": f"0x{self.addr:08X}", "name": self.name, "size": self.size,
            "loops": self.loops, "calls": [f"0x{c:08X}" for c in self.calls], "jalr": self.jalr,
            "gte": self.gte, "gte_regs": dict(sorted(self.gte_regs.items())),
            "fields": sorted(f"{k}{w}+0x{o:X}" if o >= 0 else f"{k}{w}-0x{-o:X}" for k, w, o in self.fields),
            "imms": sorted(f"sa{v - 0x10000}" if v >= 0x10000 else hex(v) for v in self.imms),
        }


def normalize(w: int) -> int:
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000
    if op == 0x0F or op in MASK_IMM_OPS:
        return w & 0xFFFF0000
    return w


class Image:
    """One overlay of one game: its words and its function map."""

    def __init__(self, root: Path, overlay: str, exe: Path | None):
        self.root = root
        cfg = json.loads((root / "config" / "verdite.json").read_text())
        rc = json.loads(strip_jsonc((root / cfg["recompilerConfig"]).read_text()))
        ov = next((o for o in rc.get("overlays", []) if o.get("name") == overlay), None)
        if ov is None:
            sys.exit(f"{root}: no overlay named {overlay!r} in {cfg['recompilerConfig']}")
        self.base = int(ov["base"], 16)
        if exe is not None:
            data = exe.read_bytes()
        else:
            from extract_file import find_entry
            from inspect_disc import open_disc, resolve_image
            disc = open_disc(resolve_image(root / cfg["disc"]))
            entry = find_entry(disc, ov["file"])
            data = disc.read(entry["lba"], entry["size"])
        off = ov.get("offset", 0) + ov.get("skip", 0)
        end = ov.get("offset", 0) + ov["size"] if "size" in ov and "offset" in ov else len(data)
        payload = data[off:end]
        if exe is not None and data[:8] == b"PS-X EXE":
            payload = data[EXE_HEADER:]
            self.base = struct.unpack_from("<I", data, 0x18)[0]
        n = len(payload) // 4
        self.words = struct.unpack(f"<{n}I", payload[:n * 4])
        fm_dir = root / cfg.get("funcmaps", "config/funcmaps")
        fm_path = root / "config" / ov["funcMap"] if "funcMap" in ov else fm_dir / f"{overlay}.json"
        self.funcs: dict[int, Function] = {}
        for f in json.loads(fm_path.read_text())["functions"]:
            a, size = int(f["address"], 16), f["size"]
            i = (a - self.base) // 4
            if size <= 0 or i < 0 or i + size // 4 > len(self.words):
                continue
            self.funcs[a] = Function(a, f["name"], self.words[i:i + size // 4])
        # A call into libgte counts as the GTE commands it runs (MVMVA without its fields).
        for fn in self.funcs.values():
            seq = []
            for c in fn.calls:
                callee = self.funcs.get(c)
                if callee is not None and callee.name in LIBGTE:
                    seq += LIBGTE[callee.name]
            if seq:
                fn.gte = [g.split("(")[0] for g in fn.gte] + seq
                fn.gte_called = True

    def get(self, addr: int) -> Function:
        if addr not in self.funcs:
            sys.exit(f"0x{addr:08X} is not a function start in {self.root.name}'s map")
        return self.funcs[addr]


def jaccard(a: set, b: set) -> float:
    return 1.0 if not a and not b else len(a & b) / len(a | b)


def score(a: Function, b: Function) -> dict:
    if a.norm == b.norm:
        parts = dict.fromkeys(WEIGHTS, 1.0)
        parts["total"], parts["identical"] = 1.0, True
        return parts
    ga, gb = a.gte, b.gte
    if getattr(a, "gte_called", False) or getattr(b, "gte_called", False):
        ga, gb = sorted(g.split("(")[0] for g in ga), sorted(g.split("(")[0] for g in gb)
    parts = {
        "ops": difflib.SequenceMatcher(None, a.ops, b.ops, autojunk=False).ratio(),
        "gte": 1.0 if not ga and not gb else
               difflib.SequenceMatcher(None, ga, gb, autojunk=False).ratio(),
        "fields": jaccard(a.fields, b.fields),
        "imms": jaccard(a.imms, b.imms),
        "size": min(a.size, b.size) / max(a.size, b.size),
        "calls": 1.0 / (1.0 + abs(len(a.calls) - len(b.calls))),
    }
    # Two routines with no GTE work agree on nothing by it; leave the term out.
    keys = [k for k in WEIGHTS if k != "gte" or a.gte or b.gte]
    parts["total"] = sum(WEIGHTS[k] * parts[k] for k in keys) / sum(WEIGHTS[k] for k in keys)
    parts["identical"] = False
    return parts


def fmt(parts: dict) -> str:
    if parts.get("identical"):
        return "1.000 identical"
    return f"{parts['total']:.3f} (" + " ".join(f"{k} {parts[k]:.2f}" for k in WEIGHTS) + ")"


def candidates(a: Function, img: Image, top: int):
    out = []
    for f in img.funcs.values():
        r = min(a.size, f.size) / max(a.size, f.size)
        if r < 0.3:
            continue
        out.append((score(a, f), f))
    out.sort(key=lambda t: -t[0]["total"])
    return out[:top]


def align(a: Function, b: Function, A: Image, B: Image, threshold: float):
    """Pair the two ordered call lists: the order-preserving alignment that
    maximises the summed score, pairing only callees scoring at least the
    threshold; a call repeated back to back counts once."""
    def calls(f, img):
        out = []
        for c in f.calls:
            if c in img.funcs and (not out or out[-1] != c):
                out.append(c)
        return out
    ca, cb = calls(a, A), calls(b, B)
    n, m = len(ca), len(cb)
    S = [[score(A.funcs[x], B.funcs[y]) for y in cb] for x in ca]
    best = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            v = max(best[i + 1][j], best[i][j + 1])
            t = S[i][j]["total"]
            if t >= threshold:
                v = max(v, t + best[i + 1][j + 1])
            best[i][j] = v
    pairs, i, j = [], 0, 0
    while i < n or j < m:
        if i < n and j < m and S[i][j]["total"] >= threshold and \
                abs(best[i][j] - (S[i][j]["total"] + best[i + 1][j + 1])) < 1e-9:
            pairs.append((ca[i], cb[j], S[i][j]))
            i, j = i + 1, j + 1
        elif j < m and (i == n or abs(best[i][j] - best[i][j + 1]) < 1e-9):
            pairs.append((None, cb[j], None))
            j += 1
        else:
            pairs.append((ca[i], None, None))
            i += 1
    return pairs


def name_of(img: Image, a: int | None) -> str:
    if a is None:
        return "-"
    f = img.funcs.get(a)
    return f"{f.name if f else hex(a)} ({f.size if f else '?'})"


def cmd_match(args, A, B):
    rows = []
    for s in args.addr:
        a = A.get(int(s, 16))
        cands = candidates(a, B, args.top)
        rows.append({"a": a.summary()["address"], "candidates": [
            {"b": f"0x{f.addr:08X}", "name": f.name, "size": f.size, "gte": len(f.gte), **p}
            for p, f in cands]})
        if not args.json:
            print(f"{a.name} ({a.size} bytes, {len(a.gte)} GTE, {len(a.calls)} calls):")
            for p, f in cands:
                print(f"  {f.name:24s} {f.size:6d}  gte {len(f.gte):3d}  calls {len(f.calls):3d}  {fmt(p)}")
    if args.json:
        print(json.dumps(rows, indent=1))


def cmd_tree(args, A, B):
    out = []

    def walk(a: Function, b: Function, depth: int, ind: str):
        for x, y, p in align(a, b, A, B, args.threshold):
            row = {"depth": args.depth - depth, "a": name_of(A, x), "b": name_of(B, y),
                   "score": p["total"] if p else None}
            out.append(row)
            if not args.json:
                print(f"{ind}{row['a']:34s} {row['b']:34s} {fmt(p) if p else ''}")
            if p and depth > 1 and x != a.addr:
                walk(A.funcs[x], B.funcs[y], depth - 1, ind + "  ")

    a, b = A.get(int(args.a_addr, 16)), B.get(int(args.b_addr, 16))
    if not args.json:
        print(f"{a.name} <-> {b.name}: {fmt(score(a, b))}")
    walk(a, b, args.depth, "  ")
    if args.json:
        print(json.dumps(out, indent=1))


def cmd_pairs(args, A, B):
    out = []
    for line in Path(args.file).read_text().splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        x, y = (int(t, 16) for t in line.split()[:2])
        a, b = A.get(x), B.get(y)
        p = score(a, b)
        rank = 1 + sum(1 for f in B.funcs.values() if f is not b and score(a, f)["total"] > p["total"]) \
            if args.rank else None
        out.append({"a": f"0x{x:08X}", "b": f"0x{y:08X}", "rank": rank, **p})
        if not args.json:
            print(f"{a.name:24s} {b.name:24s} {fmt(p)}" + (f"  rank {rank}" if rank else ""))
    if args.json:
        print(json.dumps(out, indent=1))


def cmd_show(args, A, _B):
    s = A.get(int(args.addr, 16)).summary()
    if args.json:
        print(json.dumps(s, indent=1))
        return
    for k, v in s.items():
        print(f"{k:9s} {v}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--a", type=Path, help="game A's repo root (default: this repo)")
    p.add_argument("--b", type=Path, help="game B's repo root")
    p.add_argument("--overlay-a", default="game")
    p.add_argument("--overlay-b", default="game")
    p.add_argument("--exe-a", type=Path, help="an extracted PS-X EXE for A, instead of the disc")
    p.add_argument("--exe-b", type=Path)
    p.add_argument("--json", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("match")
    m.add_argument("addr", nargs="+")
    m.add_argument("--top", type=int, default=5)
    t = sub.add_parser("tree")
    t.add_argument("a_addr")
    t.add_argument("b_addr")
    t.add_argument("--depth", type=int, default=1)
    t.add_argument("--threshold", type=float, default=0.5)
    r = sub.add_parser("pairs")
    r.add_argument("file")
    r.add_argument("--rank", action="store_true", help="also rank B among all of B's functions (slow)")
    s = sub.add_parser("show")
    s.add_argument("addr")
    args = p.parse_args()

    root_a = (args.a or verdite_game.find_root()).resolve()
    A = Image(root_a, args.overlay_a, args.exe_a)
    B = None
    if args.cmd != "show":
        if args.b is None:
            p.error("--b is required")
        B = Image(args.b.resolve(), args.overlay_b, args.exe_b)
    {"match": cmd_match, "tree": cmd_tree, "pairs": cmd_pairs, "show": cmd_show}[args.cmd](args, A, B)
    return 0


if __name__ == "__main__":
    sys.exit(main())
