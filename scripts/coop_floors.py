#!/usr/bin/env python3
"""Headless instances in one coop room on a local service (online/README.md: npm run dev), for who
runs each floor: a guest arriving on the host's floor draws the host's actors, a pickup taken on
either machine is gone on the other, a floor a guest ran alone is kept in the host's world, an
owner walking off hands its floor live to the heir, a more senior arrival takes it, and a floor
whose owner drops runs from its record on the next player there. docs/COOP_PLAN.md, "Floor
ownership".

    scripts/coop_floors.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
"""
import argparse
import random
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import coop_pair  # noqa: E402

PORTS = (27931, 27932, 27933)  # not coop_pair.py's nor title_pair.py's
NEAR_UNITS = 3 * 2000  # three cells: a puppet drawn a few updates behind its owner
FLOOR1_ENEMIES = (25, 15)  # a few cells from floor 1's enemies at (24..26, 10..11)


def part(game):
    return game.coop()["floorPart"]


def wait_part(game, wanted, floor=None, seconds=30):
    """The coop state once the game does `wanted` with its floor, on `floor` if given."""
    return game.wait(f"floor part {wanted}", lambda s: s["floorPart"] == wanted and floor in (None, s["floor"]), seconds)


def actors(game):
    return {a["slot"]: a for a in game.run("actors")["actors"]}


def near(a, b):
    return all(abs(x - y) <= NEAR_UNITS for x, y in zip(a["pos"], b["pos"]))


def wait_actors(game, what, test, seconds=20):
    deadline = time.monotonic() + seconds
    while True:
        found = actors(game)
        if test(found):
            return found
        if time.monotonic() > deadline:
            raise RuntimeError(f"{game.name}: {what} never came: {found}")
        time.sleep(0.25)


def same_actors(runner, watcher):
    """Whether the watcher draws the runner's awake actors, each near where the runner has it."""
    ran, drawn = actors(runner), actors(watcher)
    return bool(ran) and ran.keys() == drawn.keys() and all(near(ran[s], drawn[s]) for s in ran)


def wait_same(runner, watcher, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if same_actors(runner, watcher):
            return True
        time.sleep(0.2)
    return False


def object_id(game, slot):
    return game.run(f"object {slot}")["id"]


def first_object(game, skip=()):
    """A placed object slot on this floor that holds something."""
    for slot in range(160):
        if slot not in skip and object_id(game, slot) != 255:
            return slot
    raise RuntimeError(f"{game.name}: no object on this floor")


def wait_object_gone(game, slot, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if object_id(game, slot) == 255:
            return True
        time.sleep(0.2)
    return False


def main():
    checks = []

    def check(what, ok):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what)

    games = []

    def start(name, port):
        game = coop_pair.Game(name, port)
        games.append(game)
        return game

    try:
        a = start("a", PORTS[0])
        a.wait("the host's world", lambda s: s["inPlay"])
        check("A runs its floor", wait_part(a, "running")["floorPart"] == "running")
        b = start("b", PORTS[1])
        b.wait("the guest's arrival", lambda s: s["inPlay"])
        check("B, arriving on A's floor, draws it", wait_part(b, "watching")["floorPart"] == "watching")
        check("A still runs it", part(a) == "running")

        a.run("move %d %d" % FLOOR1_ENEMIES)
        b.run("move %d %d" % FLOOR1_ENEMIES)
        wait_actors(a, "awake enemies", bool)
        check("B draws A's awake actors where A has them", wait_same(a, b))

        taken_by_b = first_object(a)
        b.run(f"object {taken_by_b} take")
        check("an object B takes is gone on A", wait_object_gone(a, taken_by_b))
        taken_by_a = first_object(a, skip=(taken_by_b,))
        a.run(f"object {taken_by_a} take")
        check("an object A takes is gone on B", wait_object_gone(b, taken_by_a))

        b.run("warp 2")
        check("B runs floor 2, where no one is", wait_part(b, "running", 2)["floor"] == 2)
        on_floor2 = first_object(b)
        b.run(f"object {on_floor2} take")
        time.sleep(1)
        b.run("warp 1")
        b.wait("back on floor 1", lambda s: s["floor"] == 1)
        check("B, back on floor 1, draws A's", wait_part(b, "watching", 1)["floorPart"] == "watching")

        a.run("move %d %d" % FLOOR1_ENEMIES)
        b.run("move %d %d" % FLOOR1_ENEMIES)
        wait_actors(a, "awake enemies", bool)
        wait_same(a, b)
        before = actors(a)
        a.run("warp 2")
        check("B, A's heir, runs floor 1 when A walks off", wait_part(b, "running", 1)["floor"] == 1)
        after = actors(b)
        kept = [s for s in before if s in after and near(before[s], after[s])]
        print(f"A's awake actors as it left: {sorted(before)}; B's after: {sorted(after)}")
        check("the floor went on live: B's actors are A's, where A left them", len(kept) >= max(1, len(before) // 2))
        check("A runs floor 2 from its record", wait_part(a, "running", 2)["floor"] == 2)
        check("what B took on floor 2 alone is kept in A's world", object_id(a, on_floor2) == 255)

        a.run("warp 1")
        a.wait("back on floor 1", lambda s: s["floor"] == 1)
        check("A, more senior, takes floor 1", wait_part(a, "running", 1)["floor"] == 1)
        check("B gives it and draws A's", wait_part(b, "watching")["floorPart"] == "watching")
        a.run("move %d %d" % FLOOR1_ENEMIES)
        b.run("move %d %d" % FLOOR1_ENEMIES)
        wait_actors(a, "awake enemies", bool)
        check("B draws A's actors again", wait_same(a, b))

        # A third player: B runs floor 2 with C on it, then drops.
        c = start("c", PORTS[2])
        c.wait("the third's arrival", lambda s: s["inPlay"])
        wait_part(c, "watching")
        b.run("warp 2")
        wait_part(b, "running", 2)
        c.run("warp 2")
        check("C, arriving after B, draws B's floor 2", wait_part(c, "watching", 2)["floor"] == 2)
        print("B drops")
        b.process.kill()
        games.remove(b)
        check("C runs floor 2 from its record when B drops", wait_part(c, "running", 2, seconds=60)["floor"] == 2)
        check("the floor B ran is still the world's: what B took is gone", object_id(c, on_floor2) == 255)
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
    parser.add_argument("--room", default=f"floortest{random.randrange(1 << 30)}")
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
