#!/usr/bin/env python3
"""Two headless instances in one coop room on a local service (online/README.md: npm run dev), for
death in coop: a player who dies goes down, sees through the other's eyes on the other's floor, is
out of the world, and comes back beside them once they are out of a fight; when everyone is down
the party goes back to its last save, the world with it; a host alone is a party of one.
docs/COOP_PLAN.md, "Death is the party's".

    scripts/coop_death.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
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

PORTS = (27951, 27952)  # not coop_pair.py's, title_pair.py's, coop_floors.py's nor coop_hits.py's
FIGHT = (41, 65)  # two of floor 1's enemies wake here
KILL = "hurt 9999"
DEATH = 1  # kf::OnlineSound::Death, an index of the coop state's soundsHeard


def state(game):
    return game.run("state")["state"]


def watch(game, until, seconds):
    """Polls `game`'s coop state until `until(states so far)` holds; every state seen."""
    seen = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        seen.append(game.coop() | {"hp": state(game)["hp"], "maxHp": state(game)["maxHp"]})
        if until(seen):
            break
        time.sleep(0.2)
    return seen


def beside(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1])) <= 1


def main():
    checks = []

    def check(what, ok):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what)

    games = []
    try:
        a = coop_pair.Game("a", PORTS[0])
        games.append(a)
        opened = a.wait("the host's world", lambda s: s["inPlay"])
        save_point = (opened["floor"], opened["cell"])
        coop_floors.wait_part(a, "running")
        b = coop_pair.Game("b", PORTS[1])
        games.append(b)
        b.wait("the guest's arrival", lambda s: s["inPlay"])
        coop_floors.wait_part(b, "watching")
        a_name = a.coop()["name"]

        # Something taken after the world's last save, which a wipe puts back.
        taken = coop_floors.first_object(a)
        a.run(f"object {taken} take")
        coop_floors.wait_object_gone(b, taken)

        # B dies alone on floor 2: down, it sees through A's eyes on floor 1, and comes back beside A.
        b.run("warp 2")
        coop_floors.wait_part(b, "running", 2)
        b.run(KILL)
        seen = watch(b, lambda s: any(x["down"] for x in s) and not s[-1]["down"], 40)
        down = [x for x in seen if x["down"]]
        check(f"B, dead on floor 2, goes down seeing through A's eyes ({down[-1]['eyes'] if down else 'never down'})",
              bool(down) and all(x["eyes"] in ("", a_name) for x in down) and any(x["eyes"] == a_name for x in down))
        check("down, B follows A onto floor 1", any(x["floor"] == 1 for x in down))
        back, a_now = seen[-1], a.coop()
        check(f"B comes back beside A, whole ({back['floor']} {back['cell']} by A's {a_now['cell']}, "
              f"{back['hp']} of {back['maxHp']} hp)",
              not back["down"] and back["floor"] == 1 and beside(back["cell"], a_now["cell"]) and back["hp"] == back["maxHp"])

        # A by the enemies, chased: B dies; while A is in the fight B stays down, out of the world; once A is
        # clear of it, B comes back.
        a.run("move %d %d" % FIGHT)
        a.wait("A in a fight", lambda s: s["fighting"], 60)
        deaths_heard = a.coop()["soundsHeard"][DEATH]
        b.run(KILL)
        b.wait("B down", lambda s: s["down"], 10)
        time.sleep(1)
        check(f"A hears B die on its floor ({deaths_heard} deaths heard before, {a.coop()['soundsHeard'][DEATH]} after)",
              a.coop()["soundsHeard"][DEATH] > deaths_heard)
        targets = {actor["target"] for actor in coop_floors.actors(a).values()}
        drawn = a.run("players")["players"]
        check(f"down, B is out of A's world: no knight drawn ({len(drawn)}), no enemy after it (targets {targets})",
              not drawn and targets <= {0})
        held = watch(b, lambda s: False, 5)
        a_fought = a.coop()["fighting"]
        check(f"B stays down while A is in a fight (A fighting: {a_fought})",
              all(x["down"] for x in held) and a_fought and state(a)["hp"] > 0)
        a.run("move %d %d" % tuple(save_point[1]))
        seen = watch(b, lambda s: not s[-1]["down"], 40)
        check("once A is clear of the fight, B comes back beside it", bool(seen) and not seen[-1]["down"]
              and beside(seen[-1]["cell"], a.coop()["cell"]))

        # Both die at once: the party goes back to its last save, the world with it.
        b.run(KILL)
        a.run(KILL)
        for game in (a, b):
            game.wait("the party back at its last save", lambda s: s["life"] == 1 and not s["down"], 30)
        time.sleep(1)
        a_back, b_back = a.coop(), b.coop()
        check(f"everyone down: both back up at the save point (A {a_back['floor']} {a_back['cell']}, "
              f"B {b_back['floor']} {b_back['cell']}, saved at {save_point})",
              (a_back["floor"], a_back["cell"]) == save_point and (b_back["floor"], b_back["cell"]) == save_point)
        check("and whole", all(state(game)["hp"] == state(game)["maxHp"] for game in (a, b)))
        check(f"the object A took after the save is back on both (slot {taken})",
              coop_floors.object_id(a, taken) != 255 and coop_floors.object_id(b, taken) != 255)
        coop_floors.wait_part(a, "running", save_point[0])
        check("B draws A's floor again after the wipe", coop_floors.wait_part(b, "watching")["floorPart"] == "watching")

        # A alone is a party of one.
        b.quit()
        games.remove(b)
        time.sleep(2)
        a.run("move %d %d" % FIGHT)
        a.run(KILL)
        alone = a.wait("A alone back at its last save", lambda s: s["life"] == 2 and not s["down"], 30)
        check(f"A alone dies: back at the save point ({alone['floor']} {alone['cell']})",
              (alone["floor"], alone["cell"]) == save_point)
    finally:
        for game in games:
            game.quit()

    failed = [what for what, ok in checks if not ok]
    if failed:
        for log in sorted(coop_pair.WORK.glob("*/log.txt")):
            print(f"--- {log.parent.name}'s log, last lines:")
            print("".join(log.read_text(errors="replace").splitlines(keepends=True)[-25:]))
    print("FAIL" if failed else "PASS", f"({len(checks) - len(failed)} of {len(checks)})")
    return 1 if failed else 0


def parse():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--room", default=f"deathtest{random.randrange(1 << 30)}")
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
