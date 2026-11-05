#!/usr/bin/env python3
"""Two headless instances in one coop room on a local service (online/README.md: npm run dev), for
hits across machines: a guest's hit on an actor the host runs lands on the host's, a kill by either
player's hit gives both of them its experience, and each player's swing is heard and spell drawn on
the other's machine. docs/COOP_PLAN.md, "Hits across machines" and "Other players' spells and sounds".

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
LIGHTNING_LANDED = {32, 33}  # KF_EFFECT_KIND_LIGHTNING_IMPACT, _RADIAL_BLAST
# A walk among floor 2's casters that wakes them and comes into reach of their fire balls.
CASTERS = ((92, 4), (92, 12), (96, 14), (88, 16))
SWUNG_AT = (25, 12)  # where the three at (24..26, 10..11) reach a player and swing
SWING = 0  # kf::OnlineSound::Swing, an index of the coop state's soundsHeard
FIRE_BALL = 5  # KF_MAGIC_FIRE_BALL


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


def swing(game, seconds=10):
    """Presses Attack until `game`'s player swings (not while it is hit or fading in); whether it did."""
    made = game.coop()["soundsMade"][SWING]
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        game.run("press attack")
        time.sleep(0.5)
        if game.coop()["soundsMade"][SWING] != made:
            return True
    return False


def wait_state(game, test, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        state = game.run("state")["state"]
        if test(state):
            return state
        time.sleep(0.1)
    return None


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

        # B opens its menu there: the world goes on, and once B dies the menu closes by itself.
        b.run("press start")
        opened = wait_state(b, lambda state: state["context"] == "menu", 10)
        lowest, closed = None, None
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            state = b.run("state")["state"]
            if state["context"] == "menu":
                lowest = state["hp"] if lowest is None else min(lowest, state["hp"])
            elif lowest is not None:
                closed = state
                break
            time.sleep(0.1)
        check(f"B, in its menu, is hurt to 0 and the menu closes (lowest {lowest} hp, then {closed and closed['context']})",
              bool(opened) and lowest == 0 and closed is not None)
        # Down, B comes back beside A once A is out of a fight (docs/COOP_PLAN.md, "Death is the party's").
        b.wait("B back beside A", lambda s: not s["down"], 90)

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
        # A down player gets nothing from a kill, so B is up for it.
        b.wait("B up", lambda s: not s["down"], 90)
        a_xp, b_xp = experience(a), experience(b)
        killed = strike_until(a, slot, lambda: hp_of(a, slot) == 0)
        time.sleep(1)
        check(f"A's own kill of actor {slot} gives B experience ({b_xp} before, {experience(b)} after)",
              killed and experience(b) > b_xp)

        # Each player's swing is heard and its spell drawn on the other's machine, whoever runs the
        # floor (docs/COOP_PLAN.md, "Other players' spells and sounds").
        for caster, watcher, name in ((b, a, "B"), (a, b, "A")):
            heard = watcher.coop()["soundsHeard"][SWING]
            swung = swing(caster)
            check(f"{name}'s swing is heard on the other machine (swung: {swung})",
                  swung and bool(wait_state(watcher, lambda state: watcher.coop()["soundsHeard"][SWING] > heard, 5)))
            caster.run("cast")
            seen = set()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not seen:
                seen = {effect["kind"] for effect in watcher.run("effects")["effects"] if effect["from"] == "another player"}
                time.sleep(0.05)
            check(f"{name}'s Fire Ball is drawn on the other machine (kinds {sorted(seen)})", FIRE_BALL in seen)

        # Floor 2's casters, which A runs: B draws their effects in flight.
        a.run("warp 2")
        coop_floors.wait_part(a, "running", 2)
        b.run("warp 2")
        coop_floors.wait_part(b, "watching", 2)
        # B walks among them until they throw at it; it may die of them and come back beside A, and
        # what it felt is counted either way.
        seen, felt = set(), 0
        for cell in CASTERS * 2:
            if seen and felt:
                break
            now = b.coop()
            if now["floor"] == 2 and not now["down"]:
                b.run("move %d %d" % cell)
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline and not (seen and felt):
                seen |= {effect["kind"] for effect in b.run("effects")["effects"] if effect["from"] == "owner"}
                felt = b.coop()["hitsFelt"]
                time.sleep(0.1)
        check(f"B draws the effects A's actors launch (kinds {sorted(seen)})", bool(seen))
        check(f"their projectiles, meeting B on A, are felt on B's machine ({felt})", felt > 0)

        # A bolt's impact and blast are kinds past the magic table: once refused with everything
        # sent beside them, an honest player's spells and a floor's actors.
        b.wait("B up to cast", lambda s: not s["down"], 90)
        b.run("cast lightning")
        seen = set()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and not seen & LIGHTNING_LANDED:
            seen |= {effect["kind"] for effect in a.run("effects")["effects"] if effect["from"] == "another player"}
            time.sleep(0.03)
        check(f"B's Lightning Bolt lands on the other machine too (kinds {sorted(seen)})", bool(seen & LIGHTNING_LANDED))
    finally:
        for game in games:
            game.quit()

    coop_pair.check_nothing_refused(check)
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
