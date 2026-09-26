"""Read the recompiler's emitted C# as a call graph and a table of global writes.

There is no decompilation and no `.map` here, but `generated/*.cs` is a complete,
regular rendering of every instruction the sweep found, and two questions worth
answering are plain text in it:

  * **who calls whom** -- `KingsField2_game.func_XXXXXXXX(c, m);`, or a PSY-Q name
  * **who reads and writes which global** -- PSY-Q reaches a global through a
    `lui`/`addiu` pair, which the recompiler emits as a register loaded with
    `0xHHHH0000u` followed by an access through it with a constant displacement.

Every emitted statement may carry 0035's PGXP hook after it, which names registers
too, so it is cut off each line before anything reads it. See "The static model
read nothing" in docs/DEVELOPMENT.md.

Both are approximations and it is worth being precise about how they fail, since
the output is evidence and not proof.

`Dispatcher.Call(c, m, reg)` is an indirect jump -- a switch table, a driver
table, an overlay's per-frame slot -- and its target is not statically known. A
subtree that reaches one is marked `indirect`, and any claim about what it cannot
reach is only as good as that mark.

Global addresses are recovered by a tiny dataflow over the emitted assignments,
tracking only registers assigned a literal in the same function and invalidating
one the moment it is assigned anything else. That catches the `lui`/`addiu` idiom
that PSY-Q uses for statics, which is what matters, and misses anything reached
through a pointer, an array index or a struct base in a register -- so an empty
result means "not written through a literal address", never "not written".
"""

from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GENERATED = REPO / "generated"

RX_FUNC = re.compile(r"^\s*public static void (\w+)\(CpuContext c, IMemory m\)")
# KingsField2_game.func_XXXXXXXX(c, m) / KingsField2_game.SetRotMatrix(c, m); the
# class is per overlay, and 997 functions carry their PSY-Q names.
RX_CALL = re.compile(r"KingsField2(?:_\w+)?\.(\w+)\(c, m\)")
RX_INDIRECT = re.compile(r"Dispatcher\.Call\(c, m,")

# Every statement may be followed by 0035's PGXP hook, which names registers too;
# it is cut off before anything else reads the line.
RX_PGXP = re.compile(r"\s*if \(RecompOne\.Runtime\.Pgxp\..*$")

# c.At = 0x80070000u;  -- the lui half
RX_LOAD_HI = re.compile(r"^\s*c\.(\w+) = (0x[0-9A-Fa-f]{8})u;")
# { var _v = c.V1; c.V1 = c.V1 + 0x7714u;  -- the addiu half (ori too)
RX_ADD_IMM = re.compile(r"c\.(\w+) = c\.(\w+) ([+|-]) (0x[0-9A-Fa-f]+)u;")
# { var _a = (c.At - 0x1A34u); mem.WriteU32(_a, ...)  /  c.V0 = mem.ReadU16(_a)
RX_EA = re.compile(r"var _a = \(?c\.(\w+)(?:\s*([+-])\s*(0x[0-9A-Fa-f]+)u)?\)?;")
RX_STORE = re.compile(r"m(?:em)?\.Write(U8|U16|U32)\(_a,")
RX_LOAD = re.compile(r"m(?:em)?\.Read(U8|U16|U32)\(_a\)")
# any other assignment to a register kills our knowledge of it
RX_ASSIGN = re.compile(r"(?:^|[;{]\s*)c\.(\w+) = ")

WIDTH = {"U8": 1, "U16": 2, "U32": 4}


@dataclass
class Func:
    name: str
    overlay: str
    start: int                      # line number in the source file
    end: int
    calls: set[str] = field(default_factory=set)
    indirect: bool = False
    writes: dict[int, int] = field(default_factory=dict)   # address -> width
    reads: dict[int, int] = field(default_factory=dict)    # address -> width
    # Every address formed by a lui/addiu pair, used or not: the base of a table
    # indexed in a register is only visible here.
    refs: set[int] = field(default_factory=set)
    has_backedge: bool = False
    address: int = 0                # from the name, or the funcmap for a PSY-Q name

    @property
    def addr(self) -> int:
        return self.address


class Graph:
    def __init__(self, overlays: list[str] | None = None):
        self.funcs: dict[str, Func] = {}
        self.callers: dict[str, set[str]] = defaultdict(set)
        self._by_addr: dict[int, Func] = {}
        for path in sorted(GENERATED.glob("*.cs")):
            overlay = path.stem
            if overlay in ("Entry", "Stubs"):
                continue
            if overlays and overlay not in overlays:
                continue
            self._parse(path, overlay)
        for f in self.funcs.values():
            self._by_addr.setdefault(f.addr, f)
            for callee in f.calls:
                self.callers[callee].add(f.name)

    # -- parsing --------------------------------------------------------------

    def _parse(self, path: Path, overlay: str) -> None:
        lines = path.read_text(errors="replace").splitlines()
        names = _funcmap(overlay)
        current: Func | None = None
        literal: dict[str, int] = {}
        labels: set[str] = set()

        for n, line in enumerate(lines, 1):
            m = RX_FUNC.match(line)
            if m:
                if current:
                    current.end = n - 1
                name = m.group(1)
                addr = names.get(name)
                if addr is None and name.startswith("func_"):
                    addr = int(name[5:13], 16)
                if addr is None:
                    current = None          # a helper the recompiler emitted, not a function
                    continue
                # An overlay redefinition of the same address (game vs open) keeps
                # the first; every address-based claim here names its overlay.
                current = self.funcs.setdefault(name, Func(name, overlay, n, n, address=addr))
                literal, labels = {}, set()
                continue

            if current is None:
                continue
            current.end = n
            line = RX_PGXP.sub("", line)

            for c in RX_CALL.finditer(line):
                current.calls.add(c.group(1))
            if RX_INDIRECT.search(line):
                current.indirect = True

            if line.lstrip().startswith("L") and line.rstrip().endswith(": ;"):
                labels.add(line.strip().split(":")[0])
            elif "goto L" in line:
                target = line.split("goto ")[1].split(";")[0].strip()
                if target in labels:
                    current.has_backedge = True     # jumps to a label already seen

            self._track(line, literal, current)

        if current:
            current.end = len(lines)

    @staticmethod
    def _track(line: str, literal: dict[str, int], f: Func) -> None:
        """One line of the tiny dataflow. Order matters: an access reads the state
        this line's assignment would clobber, so accesses are handled first."""
        ea = RX_EA.search(line)
        if ea:
            reg, sign, off = ea.groups()
            base = literal.get(reg)
            if base is not None:
                delta = int(off, 16) if off else 0
                addr = (base - delta if sign == "-" else base + delta) & 0xFFFFFFFF
                if 0x80000000 <= addr < 0x80800000:
                    for rx, table in ((RX_STORE, f.writes), (RX_LOAD, f.reads)):
                        a = rx.search(line)
                        if a:
                            table[addr] = max(table.get(addr, 0), WIDTH[a.group(1)])

        m = RX_LOAD_HI.match(line)
        if m:
            literal[m.group(1)] = int(m.group(2), 16)
            return

        m = RX_ADD_IMM.search(line)
        if m:
            dst, src, op, imm = m.groups()
            base = literal.get(src)
            if base is None:
                literal.pop(dst, None)
            elif op == "|":
                literal[dst] = base | int(imm, 16)
            else:
                literal[dst] = (base - int(imm, 16) if op == "-" else base + int(imm, 16)) & 0xFFFFFFFF
            if dst in literal:
                f.refs.add(literal[dst])
            return

        for m in RX_ASSIGN.finditer(line):
            literal.pop(m.group(1), None)

    # -- queries --------------------------------------------------------------

    def by_addr(self, addr: int) -> Func | None:
        return self._by_addr.get(addr)

    def subtree(self, name: str, limit: int = 100_000) -> set[str]:
        """Every function reachable from `name`, itself included."""
        seen, stack = {name}, [name]
        while stack and len(seen) < limit:
            f = self.funcs.get(stack.pop())
            if not f:
                continue
            for callee in f.calls:
                if callee not in seen:
                    seen.add(callee)
                    stack.append(callee)
        return seen

    def subtree_blocked(self, name: str, blocked: set[str]) -> set[str]:
        """Reachability that refuses to enter any function in `blocked`.

        Used to ask "can this reach a drawing routine *without* going through a
        modal loop", which is the difference between a gate that would drop
        primitives and one that only decides whether a sub-loop is entered.
        """
        seen, stack = {name}, [name]
        while stack:
            f = self.funcs.get(stack.pop())
            if not f:
                continue
            for callee in f.calls:
                if callee in seen or callee in blocked:
                    continue
                seen.add(callee)
                stack.append(callee)
        return seen

    def reaches_indirect(self, names: set[str]) -> bool:
        return any(self.funcs[n].indirect for n in names if n in self.funcs)

    def writers(self, addr: int) -> list[str]:
        return sorted(f.name for f in self.funcs.values() if addr in f.writes)

    def readers(self, addr: int) -> list[str]:
        return sorted(f.name for f in self.funcs.values() if addr in f.reads)

    def touching(self, lo: int, hi: int) -> dict[str, tuple[list[int], list[int], list[int]]]:
        """Every function reaching into [lo, hi) through a literal address: the
        addresses it reads, the ones it writes, and the ones it only forms -- a
        table base it then indexes, which the dataflow cannot follow further."""
        out = {}
        for f in self.funcs.values():
            r = sorted(a for a in f.reads if lo <= a < hi)
            w = sorted(a for a in f.writes if lo <= a < hi)
            b = sorted(a for a in f.refs if lo <= a < hi and a not in f.reads and a not in f.writes)
            if r or w or b:
                out[f.name] = (r, w, b)
        return out

    def writes_in_subtree(self, name: str) -> dict[int, set[str]]:
        out: dict[int, set[str]] = defaultdict(set)
        for n in self.subtree(name):
            f = self.funcs.get(n)
            if not f:
                continue
            for addr in f.writes:
                out[addr].add(n)
        return out


def _funcmap(overlay: str) -> dict[str, int]:
    """Name -> address for an overlay, so a PSY-Q-named function has one too."""
    import json
    path = REPO / "config" / "funcmaps" / f"{overlay}.json"
    if not path.exists():
        return {}
    return {e["name"]: int(e["address"], 16) for e in json.loads(path.read_text())["functions"]}
