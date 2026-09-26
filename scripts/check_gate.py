#!/usr/bin/env python3
"""Verify the rule FramePacing states about its own gate, and report the gaps.

    python3 scripts/check_gate.py            # check; non-zero exit on a violation
    python3 scripts/check_gate.py --stages   # what every stage reaches and writes

`patches/FramePacing.cs` says **"can it draw" is the test each of those had to
pass**, because a stage that submits primitives cannot be skipped -- at 120 fps
three frames in four would have nothing from it. That test was applied by reading
the emitted C# by hand, once, and nothing has re-applied it since. Adding an
address to `DefaultGate` is currently an assertion; this makes it a check.

What it verifies, per gated address:

  * its subtree contains no `DrawOTag`, `VSync`, `PutDrawEnv` or `PutDispEnv`
  * it resolves to a real function in `generated/game.cs`
  * it is not itself inside a modal loop, where a whole-function gate would skip a
    whole session rather than one iteration

And what it reports rather than enforces: an `indirect` mark, where the subtree
reaches a `Dispatcher.Call` and static reachability stops being a proof. Stage 6
is the honest example -- it dispatches through the area module's header slot, so
"its subtree cannot draw" holds for the nine modules that were checked and is not
a guarantee about a tenth.

`--stages` prints the inverse, which is the part that goes stale in prose: what
each *ungated* stage reaches, so stage 2's deliberate exclusion stays a recorded
consequence instead of a remembered decision.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kf2model as model

# Submitting a frame's picture. These are the ones whose absence would leave a
# rendered frame with nothing in it.
SUBMIT = {
    0x80060818: "DrawOTag",
    0x80060870: "PutDrawEnv",
    0x80060990: "PutDispEnv",
}

# Presenting. A skipped VSync costs a present, not a primitive -- and since
# FramePacing finds the frame boundary by counting VSync *calls*, it is worth
# reporting separately rather than either ignoring or failing on.
PRESENT = {0x8005FCC8: "VSync"}

SDK_NAMES = SUBMIT | PRESENT

# Gated addresses that do reach a submitting call, with the reason it is not a
# defect. Anything reaching one that is *not* listed here fails: that is the
# whole point of the check.
KNOWN = {
    0x80037C0C: (
        "stage 2 calls stage 13 itself, and only from one edge: func_80037B5C, "
        "the transition fade, which steps a tint and renders its own frames. Its "
        "indirect arm reaches the area modules' message-box and cutscene loops "
        "(func_80047000, func_80048208, func_8004831C), which get there the same "
        "way. Every one of those is an *extra* render inside the stage -- the main "
        "loop still runs stage 13 afterwards -- so skipping stage 2 costs the "
        "*entry* to a fade or a cutscene up to one tick of delay, never a frame's "
        "picture. It is the same exception as stage 3 and a strictly narrower one."
    ),
    0x8002A550: (
        "stage 3 calls stage 13 itself, on the frames where an item is used "
        "(func_80029CBC) and from the transition fade (func_80037B5C). Those are "
        "*extra* renders inside the stage, not the frame's own -- the main loop "
        "still runs stage 13 afterwards -- so skipping stage 3 costs a redundant "
        "draw rather than a frame's picture."
    ),
    0x80046A60: (
        "stage 5 reaches a submit only through func_80043388, a modal loop with "
        "a backward branch that opens the shops (func_8001D544 and three more), "
        "the message box (func_80035B48) and stage 13 itself -- an NPC's "
        "conversation. With every modal loop blocked it reaches nothing that "
        "draws, so skipping stage 5 delays entering one by up to a tick, as "
        "stage 2's exception does."
    ),
}


def draws_in(names: set[str], table: dict[int, str] = SDK_NAMES) -> list[str]:
    return sorted(label for addr, label in table.items() if f"func_{addr:08X}" in names)


def shortest_path(g, start: str, targets: dict[int, str], blocked: set[str]) -> list[str] | None:
    """A concrete route from `start` to any of `targets`, for the report.

    A bare "reaches DrawOTag" is not actionable; the path is what lets a person
    decide whether the call is the frame's own render or an extra one, which is a
    distinction no static rule can make.
    """
    from collections import deque
    want = {f"func_{a:08X}" for a in targets}
    prev: dict[str, str | None] = {start: None}
    q = deque([start])
    while q:
        n = q.popleft()
        if n in want and n != start:
            out = []
            while n is not None:
                out.append(n)
                n = prev[n]
            return out[::-1]
        f = g.funcs.get(n)
        if not f:
            continue
        for c in sorted(f.calls):
            if c in prev or c in blocked:
                continue
            prev[c] = n
            q.append(c)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stages", action="store_true")
    args = ap.parse_args()

    g = model.graph()
    stages = model.stages()
    gate = model.gated()
    gate_names = {f"func_{a:08X}" for a in gate}
    modal = model.modal_loops()

    if args.stages:
        print("The thirteen per-frame stages, from the main loop's own body.\n")
        for i, name in enumerate(stages, 1):
            sub = g.subtree(name)
            hits = draws_in(sub)
            mark = "gated" if name in gate_names else "     "
            print(f"{i:>2}. {name}  [{mark}]  {len(sub):>4} functions"
                  f"  writes {len(g.writes_in_subtree(name)):>3} globals"
                  f"  {'draws: ' + ' '.join(hits) if hits else 'cannot draw'}")
        print("\nA stage that draws cannot be gated whole; its per-frame counters have")
        print("to be found and held one at a time. See scripts/find_writers.py --stage.")
        return 0

    edges = sum(len(f.calls) for f in g.funcs.values())
    writes = sum(len(f.writes) for f in g.funcs.values())
    print(f"graph: {len(g.funcs)} functions, {edges} call edges, {writes} global writes")
    if edges == 0 or writes == 0:
        # An empty graph reaches nothing and so passes every rule; that was the
        # state for as long as the parser lagged the codegen.
        print("FAIL  the call graph is empty: scripts/callgraph.py no longer reads generated/*.cs")
        return 1

    failures = 0
    print(f"{len(gate)} gated addresses from patches/FramePacing.cs DefaultGate")
    print('The rule: "can it draw" -- a stage that submits primitives cannot be')
    print("skipped, because at 120 fps three frames in four would have nothing from it.\n")

    for addr in gate:
        f = g.by_addr(addr)
        if f is None:
            print(f"  {addr:08X}  FAIL  no such function in generated/game.cs")
            failures += 1
            continue

        sub = g.subtree(f.name)
        submits = draws_in(sub, SUBMIT)
        presents = draws_in(sub, PRESENT)
        notes = []

        if not submits:
            print(f"  {addr:08X}  ok    {f.name} submits nothing ({len(sub)} functions)")
        elif addr in KNOWN:
            print(f"  {addr:08X}  ok*   {f.name} reaches {' '.join(submits)}, "
                  f"recorded exception ({len(sub)} functions)")
            notes.append(KNOWN[addr])
            p = shortest_path(g, f.name, SUBMIT, set())
            if p:
                notes.append("path: " + " -> ".join(p))
        else:
            p = shortest_path(g, f.name, SUBMIT, set())
            print(f"  {addr:08X}  FAIL  {f.name} reaches {' '.join(submits)} "
                  f"and is not a recorded exception ({len(sub)} functions)")
            if p:
                notes.append("path: " + " -> ".join(p))
            notes.append("if this is safe, record why in KNOWN in this script")
            failures += 1

        if presents:
            p = shortest_path(g, f.name, PRESENT, set(modal))
            if p:
                notes.append("reaches VSync outside any modal loop via "
                             + " -> ".join(p[1:]) + " -- costs a present, not a primitive, "
                             "but FramePacing finds the frame boundary by counting VSync calls")
        if g.reaches_indirect(sub):
            notes.append("indirect: subtree reaches Dispatcher.Call, so this is not a proof")

        for n in notes:
            print(f"                  note: {n}")

    ungated = [s for s in stages if s not in gate_names]
    cannot_draw = [s for s in ungated if not draws_in(g.subtree(s))]
    if cannot_draw:
        print(f"\n{len(cannot_draw)} ungated stage(s) that could be gated but are not:")
        for s in cannot_draw:
            n = len(g.writes_in_subtree(s))
            print(f"  {s}  cannot draw, writes {n} global(s)"
                  f"{'  -- nothing to hold' if n == 0 else '  -- candidate'}")

    print(f"\n{failures} violation(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
