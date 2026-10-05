#!/usr/bin/env python3
"""Rejoin functions a linear sweep cut in half, using conditional branches as proof.

The sweep ends a function at any `jr` or `j` plus its delay slot. That is right
for `jr ra` and for a tail call, and wrong for the two forms PSY-Q emits inside a
function: a `jr` through a switch jump table, and a `j` to a shared epilogue. Both
leave the sweep starting a "new function" in the middle of a real one.

A MIPS conditional branch never leaves the function that issues it -- compilers
have no conditional tail call and the range is only +-128 KB anyway. So a
conditional branch that crosses a function boundary in the map is proof the map
is wrong, and the two functions plus everything laid out between them are one
function. Anything else the sweep split stays split.

This matters because the recompiler turns an in-function branch into a `goto` and
an out-of-function one into `Dispatcher.Call`, and the dispatcher only knows
function *entry points*: a branch into the middle of a swept function dies at run
time with "unmapped call: 0x...". Merging makes those targets ordinary labels.

Usage:
    python3 scripts/merge_branch_spans.py                 # every overlay in the config
    python3 scripts/merge_branch_spans.py fdat17 --dry-run
"""

import argparse
import bisect
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_disc import open_disc, resolve_image
from extract_file import find_entry
import verdite_game

OP_J = 2
OP_JAL = 3
OP_REGIMM = 1
COND_BRANCH_OPS = {4, 5, 6, 7}  # BEQ, BNE, BLEZ, BGTZ
REGIMM_BRANCHES = {0, 1, 16, 17}  # BLTZ, BGEZ, BLTZAL, BGEZAL


def load_jsonc(path: Path):
    """The recompiler's config allows // comments and trailing commas; json does not."""
    text = re.sub(r"^\s*//.*$", "", path.read_text(), flags=re.M)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


class FuncIndex:
    """Address -> containing function start, over a sorted non-overlapping map."""

    def __init__(self, funcs: list):
        self.funcs = sorted(funcs, key=lambda f: f["start"])
        self.starts = [f["start"] for f in self.funcs]

    def containing(self, addr: int):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        f = self.funcs[i]
        return f["start"] if f["start"] <= addr < f["end"] else None

    def owner(self, addr: int):
        """The last function start at or before addr, whether or not its size reaches."""
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.starts[i] if i >= 0 else None


def branch_target(pc: int, word: int):
    """Return the target of a conditional branch at pc, or None if it is not one."""
    op = word >> 26
    if op == OP_REGIMM:
        if ((word >> 16) & 31) not in REGIMM_BRANCHES:
            return None
    elif op not in COND_BRANCH_OPS:
        return None
    imm = word & 0xFFFF
    return pc + 4 + (imm - 0x10000 if imm & 0x8000 else imm) * 4


def find_jump_tables(module: dict, funcs: list) -> dict:
    """Map each switch `jr` to the table it indexes, by the address the code forms.

    The table's address is built by the code -- `lui`/`addiu` for the base, `addu`
    for the scaled index, `lw`, `jr` -- so it is written into the instruction
    stream and does not have to be guessed. It is not necessarily built next to
    the jump: fdat14 hoists the `lui`/`addiu` pair to the top of the function, 33
    instructions above the `jr` that uses it. So follow the registers over the
    whole function, the way the recompiler's own analyzer does.

    Each `jr` maps to (table, count). The count is the `sltiu` bound that guards
    the index, when one sits within 16 instructions of the `jr`; without it a
    table is read until a word stops looking like a label, and a pointer laid out
    after the table can pass for one.
    """
    lo, hi = module["base"], module["base"] + module["size"]
    tables = {}

    for func in funcs:
        value = {}  # register -> constant address it holds
        table = {}  # register -> table the register was loaded from
        bound = None  # (pc, n) of the last `sltiu`: the switch's case count
        for pc in range(func["start"], min(func["end"], hi), 4):
            word = module["word"](pc)
            op, rs, rt, rd = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            imm = word & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm

            if op == 11:  # sltiu
                bound = (pc, simm & 0xFFFFFFFF)
            if op == 15:  # lui
                value[rt], table[rt] = (imm << 16), None
            elif op == 9 and rs in value and value[rs] is not None:  # addiu off a lui
                value[rt], table[rt] = value[rs] + simm, None
            elif op == 35:  # lw -- the table read itself
                table[rt] = value.get(rs) + simm if value.get(rs) is not None else None
                value[rt] = None
            elif op == 0 and word & 0x3F == 8:  # jr
                if rs != 31 and table.get(rs) is not None and lo <= table[rs] < hi:
                    count = bound[1] if bound and pc - bound[0] <= 64 else None
                    tables[pc] = (table[rs], count)
            elif op == 0 and word & 0x3F in (32, 33):  # add/addu: base + scaled index
                known = [value.get(r) for r in (rs, rt) if value.get(r) is not None]
                value[rd], table[rd] = (known[0] if len(known) == 1 else None), None
            elif op == 0:
                value[rd], table[rd] = None, None
            else:
                value[rt], table[rt] = None, None
            value[0], table[0] = 0, None

    return tables


def table_entries(module: dict, start: int, stops: set, text_lo: int, text_hi: int,
                  count=None) -> list:
    """Read a table until a word stops looking like a label, or the next table begins."""
    entries = []
    at = start
    while at + 4 <= module["base"] + module["size"]:
        if count is not None and len(entries) == count:
            break
        if at != start and at in stops:
            break
        word = module["word"](at)
        if word % 4 or not text_lo <= word < text_hi:
            break
        entries.append(word)
        at += 4
    return entries


def merge_pass(funcs: list, base: int, text: bytes, module: dict) -> tuple:
    """Merge every run of functions tied together by a branch or a switch table.

    Merging two functions has to swallow everything laid out between them as well
    -- a switch's case bodies sit between the dispatch and the epilogue -- so the
    unit of merging is a contiguous run, not a pair.
    """
    index = FuncIndex(funcs)
    end = base + len(text)

    def join(here: int, target: int):
        """Record that one function covers both a site and something it jumps to."""
        there = index.owner(target)
        if there is None or here == there:
            return None
        return (min(here, there), max(here, target))

    joins = []  # (low start, high address) pairs proving one function spans both
    for offset in range(0, len(text) - 3, 4):
        word = struct.unpack_from("<I", text, offset)[0]
        pc = base + offset
        target = branch_target(pc, word)
        if target is None or not (base <= target < end):
            continue
        here = index.containing(pc)
        if here is None or here == index.containing(target):
            continue
        joins.append(join(here, target))

    # A switch table's entries are all labels in the one function that indexes it,
    # so they carry the same proof a branch does -- and reach cases no branch does.
    tables = find_jump_tables(module, index.funcs)
    stops = {at for at, _ in tables.values()}
    for jr, (start, count) in tables.items():
        here = index.containing(jr)
        if here is None:
            continue
        for entry in table_entries(module, start, stops, base, end, count):
            if index.containing(entry) != here:
                joins.append(join(here, entry))

    joins = [j for j in joins if j is not None]
    if not joins:
        return funcs, []

    # The furthest address each function is known to branch to.
    reach = {}
    for low, high in joins:
        reach[low] = max(reach.get(low, low), high)

    # Sweep the sorted map once, absorbing every function that starts at or before
    # the furthest address an already-absorbed function branches to.
    out, notes = [], []
    current, limit = None, -1
    for f in index.funcs:
        if current is not None and f["start"] <= limit:
            current["end"] = max(current["end"], f["end"])
            current["absorbed"].append(f["start"])
        else:
            if current is not None:
                out.append(current)
            current = dict(f, absorbed=[])
            limit = -1
        limit = max(limit, reach.get(f["start"], -1))
    if current is not None:
        out.append(current)

    for f in out:
        if f["absorbed"]:
            notes.append((f["start"], f["end"], f["absorbed"]))
        del f["absorbed"]
    return out, notes


def falls_into(word, start: int, lo: int) -> bool:
    """Whether the instruction before start runs on into it.

    It does unless the last non-nop instruction before start is an unconditional
    jump (`j`, `jr`, `b`), or that jump's delay slot. An absorbed start reached
    this way is the merged function's own next instruction, not a lost entry.
    """
    def jump(w):
        op = w >> 26
        return (op == OP_J or (op == 0 and w & 0x3F == 8)
                or (op == 4 and (w >> 16) & 0x3FF == 0))
    at = start - 4
    while at >= lo and word(at) == 0:
        at -= 4
    if at < lo:
        return False
    return not (jump(word(at)) or (at - 4 >= lo and jump(word(at - 4))))


def jal_targets(base: int, text: bytes, index) -> set:
    """Every address the module calls with `jal`. These have to stay entry points.

    Only sites inside a known function count: a data word that decodes as `jal`
    is not a call (add_call_targets applies the same rule).
    """
    targets = set()
    for offset in range(0, len(text) - 3, 4):
        word = struct.unpack_from("<I", text, offset)[0]
        if word >> 26 == OP_JAL:
            pc = base + offset
            if index.containing(pc) is None:
                continue
            targets.add((pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
    return targets


def jump_reachable(base: int, text: bytes, start: int, end: int) -> set:
    """Addresses reached by a `j` or a branch issued from inside [start, end)."""
    found = set()
    for pc in range(start, end, 4):
        word = struct.unpack_from("<I", text, pc - base)[0]
        if word >> 26 == OP_J:
            found.add((pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
        else:
            target = branch_target(pc, word)
            if target is not None:
                found.add(target)
    return found


def load_module(disc, overlay: dict) -> dict:
    """The overlay's bytes as the recompiler sees them, with a word accessor."""
    entry = find_entry(disc, overlay["file"])
    start = overlay.get("offset", 0) + overlay.get("skip", 0)
    size = overlay.get("size") or entry["size"] - start
    data = disc.read(entry["lba"] + start // 2048, size + start % 2048)
    data = data[start % 2048 : start % 2048 + size]
    base = int(overlay["base"], 16)
    return {
        "base": base,
        "size": len(data),
        "data": data,
        "word": lambda a: struct.unpack_from("<I", data, a - base)[0],
    }


def process(disc, overlay: dict, dry_run: bool, root: Path, config: Path) -> int:
    module = load_module(disc, overlay)
    base, text = module["base"], module["data"]
    path = config.resolve().parent / overlay["funcMap"]
    doc = load_jsonc(path)

    funcs = [
        {"start": int(f["address"], 16), "end": int(f["address"], 16) + int(f["size"]), "name": f["name"]}
        for f in doc["functions"]
    ]
    before = len(funcs)

    all_notes = []
    while True:
        funcs, notes = merge_pass(funcs, base, text, module)
        if not notes:
            break
        all_notes += notes

    print(f"{overlay['name']:8s} base=0x{base:08X}  functions {before} -> {len(funcs)}")
    if not all_notes:
        print("    nothing to merge")
        return 0

    for start, end, absorbed in all_notes:
        inner = " ".join(f"{a:08X}" for a in absorbed)
        print(f"    func_{start:08X} now spans to 0x{end:08X}, absorbing {inner}")

    # Every start a merge swallowed has to survive as something the recompiler can
    # still reach: a label it emits from a `j`/branch, or an entry in a switch
    # table it resolves. A `jal` to one is unfixable -- a label is not callable --
    # and either way the merge would trade one unmapped call for another.
    index = FuncIndex(funcs)
    calls = jal_targets(base, text, index)
    tables = find_jump_tables(module, index.funcs)
    stops = {at for at, _ in tables.values()}
    problems = 0

    for start, end, absorbed in all_notes:
        labels = jump_reachable(base, text, start, end)
        for jr, (at, count) in tables.items():
            if start <= jr < end:
                labels.update(table_entries(module, at, stops, base, base + len(text), count))
        for addr in absorbed:
            if addr in calls:
                print(f"    WARNING: 0x{addr:08X} is a `jal` target and cannot become a label")
                problems += 1
            elif addr not in labels and not falls_into(module["word"], addr, start):
                print(f"    WARNING: 0x{addr:08X} is now unreachable inside func_{start:08X}")
                problems += 1

    # A table entry that still points outside the function indexing it means the
    # merge did not go far enough, and the recompiler will read a shorter table
    # than the code indexes -- silently, and only some cases will misdispatch.
    for jr, (at, count) in tables.items():
        here = index.containing(jr)
        stray = [e for e in table_entries(module, at, stops, base, base + len(text), count)
                 if index.containing(e) != here]
        if stray:
            where = " ".join(f"{e:08X}" for e in stray)
            print(f"    WARNING: table at 0x{at:08X} for the `jr` at 0x{jr:08X} escapes its function: {where}")
            problems += 1

    if problems:
        return 1

    if dry_run:
        print("    (dry run, not written)")
        return 0

    doc["functions"] = [
        {"address": f"0x{f['start']:08X}", "name": f["name"], "size": f["end"] - f["start"]}
        for f in sorted(funcs, key=lambda f: f["start"])
    ]
    path.write_text(json.dumps(doc, indent=2) + "\n")
    print(f"    wrote {path.relative_to(root)}")
    return 0


def main() -> int:
    root, game = verdite_game.load()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("overlays", nargs="*", help="overlay names; default is all of them")
    parser.add_argument("--config", type=Path, default=root / game["recompilerConfig"])
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    config = load_jsonc(args.config)
    disc = open_disc(resolve_image(root / game["disc"]))

    wanted = set(args.overlays)
    status = 0
    for overlay in config["overlays"]:
        if wanted and overlay["name"] not in wanted:
            continue
        if "funcMap" not in overlay:
            continue
        status |= process(disc, overlay, args.dry_run, root, args.config)
    return status


if __name__ == "__main__":
    sys.exit(main())
