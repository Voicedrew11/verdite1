#!/usr/bin/env python3
"""Two headless instances in one coop room on a local service (online/README.md: npm run dev), for
hits across machines: a guest's hit on an actor the host runs lands on the host's, a kill by either
player's hit gives both of them its experience. docs/COOP_PLAN.md, "Hits across machines".

    scripts/coop_hits.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
"""
import argparse
import random
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import coop_floors  # noqa: E402
import coop_pair  # noqa: E402

PORTS = (27941, 27942)  # not coop_pair.py's, title_pair.py's nor coop_floors.py's
FIGHT = (41, 65)  # two of floor 1's enemies wake here
SWUNG_AT = (25, 12)  # where the three at (24..26, 10..11) reach a player and swing


def experience(game):
    return game.run("state")["state"]["experience"]


def hp_of(game, slot):
    actor = coop_floors.actors(game).get(slot)
    return actor["hp"] if actor else None


def strike_until(game, slot, test, seconds=60):
    """Strikes `slot` from `game` until `test()` holds; whether it did."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if test():
            return True
        game.run(f"hit {slot}")
        time.sleep(0.15)
    return test()


def awake_slot(runner, watcher):
    """An actor awake on both, at full health on the runner."""
    def alive(actors):
        return {slot: actor for slot, actor in actors.items() if actor["hp"] > 0}

    found = alive(coop_floors.wait_actors(runner, "awake living enemies", lambda actors: alive(actors), seconds=60))
    coop_floors.wait_same(runner, watcher)
    def away(slot):
        return sum(abs(p // 2000 - c) for p, c in zip(found[slot]["pos"][::2], FIGHT))

    return min(found, key=away)


def main():
    checks = []

    def check(what, ok):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what)

    games = []
    try:
        a = coop_pair.Game("a", PORTS[0])
        games.append(a)
        a.wait("the host's world", lambda s: s["inPlay"])
        coop_floors.wait_part(a, "running")
        b = coop_pair.Game("b", PORTS[1])
        games.append(b)
        b.wait("the guest's arrival", lambda s: s["inPlay"])
        coop_floors.wait_part(b, "watching")
        # B alone beside A's enemies, A where it started: their swings are felt on B's machine.
        b.run("move %d %d" % SWUNG_AT)
        b.wait("beside enemies", lambda state: state["cell"] == list(SWUNG_AT))
        hp = b.run("state")["state"]["hp"]
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and b.run("state")["state"]["hp"] >= hp:
            time.sleep(0.25)
        hurt = b.run("state")["state"]["hp"]
        targets = {actor["target"] for actor in coop_floors.actors(a).values()}
        check(f"enemies A runs go after B and hurt B on B's machine ({hp} hp before, {hurt} after, targets {targets})",
              hurt < hp)

        a.run("move %d %d" % FIGHT)
        b.run("move %d %d" % FIGHT)
        for game in (a, b):
            game.wait("the fight", lambda state: state["cell"] == list(FIGHT))
        time.sleep(1)

        slot = awake_slot(a, b)
        before = hp_of(a, slot)
        b.run(f"hit {slot}")
        landed = strike_until(b, slot, lambda: (hp_of(a, slot) or 0) < before, seconds=10)
        check(f"B's hit on actor {slot}, which A runs, lands on A's ({before} hp before)", landed)

        a_xp, b_xp = experience(a), experience(b)
        killed = strike_until(b, slot, lambda: hp_of(a, slot) == 0)
        check("B's hits kill it on A", killed)
        time.sleep(1)
        check(f"B's kill gives B experience ({b_xp} before, {experience(b)} after)", experience(b) > b_xp)
        check(f"and gives A, on the floor, experience ({a_xp} before, {experience(a)} after)", experience(a) > a_xp)

        slot = awake_slot(a, b)
        a_xp, b_xp = experience(a), experience(b)
        killed = strike_until(a, slot, lambda: hp_of(a, slot) == 0)
        time.sleep(1)
        check(f"A's own kill of actor {slot} gives B experience ({b_xp} before, {experience(b)} after)",
              killed and experience(b) > b_xp)

    finally:
        for game in games:
            game.quit()

    failed = [what for what, ok in checks if not ok]
    if failed:
        for log in sorted(coop_pair.WORK.glob("*/log.txt")):
            print(f"--- {log.parent.name}'s log, last lines:")
            print("".join(log.read_text(errors="replace").splitlines(keepends=True)[-20:]))
    print("FAIL" if failed else "PASS", f"({len(checks) - len(failed)} of {len(checks)})")
    return 1 if failed else 0


def parse():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--room", default=f"hittest{random.randrange(1 << 30)}")
    parser.add_argument("--service", default="http://127.0.0.1:8787")
    parser.add_argument("--work", help="a new folder for the instances' files and logs, kept")
    args = parser.parse_args()
    coop_pair.BINARY, coop_pair.ROOM, coop_pair.SERVICE = args.binary, args.room, args.service
    return args.work


if __name__ == "__main__":
    kept = parse()
    with tempfile.TemporaryDirectory() as work:
        coop_pair.WORK = Path(kept or work)
        try:
            sys.exit(main())
        finally:
            coop_pair.kill_all()
