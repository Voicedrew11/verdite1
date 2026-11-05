#!/usr/bin/env python3
"""Three headless instances in one coop room on a local service (online/README.md: npm run dev),
one sending what a modified client might (the agent's forge): each is refused and counted, no one
leaves the room, and a floor whose snapshot is refused goes on from what the taker holds.
docs/COOP_PLAN.md, "What is refused".

    scripts/coop_forged.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
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

PORTS = (27941, 27942, 27943)  # not the other coop scripts'


def refused(game, kind):
    return game.coop()["refused"][kind]


def wait_refused(game, kind, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if refused(game, kind) > 0:
            return True
        time.sleep(0.1)
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

    def everyone_stays(when):
        states = [game.coop() for game in games]
        check(f"no one is put out of the room ({when})",
              all(state["inPlay"] and state["role"] != "none" for state in states))

    try:
        a = start("a", PORTS[0])
        a.wait("the host's world", lambda s: s["inPlay"])
        coop_floors.wait_part(a, "running")
        b = start("b", PORTS[1])
        b.wait("B's arrival", lambda s: s["inPlay"])
        coop_floors.wait_part(b, "watching")
        c = start("c", PORTS[2])
        c.wait("C's arrival", lambda s: s["inPlay"])
        coop_floors.wait_part(c, "watching")
        check("honest arrivals refuse nothing", not any(any(g.coop()["refused"].values()) for g in games))

        for kind, counted in (("message", "message"), ("writes", "floorWrites"), ("frame", "frame"), ("pose", "pose")):
            a.run(f"forge {kind}")
            check(f"a forged {kind} is refused by B", wait_refused(b, counted))
        everyone_stays("after forged packets")

        b.run("forge character")
        check("a forged character is refused by the host", wait_refused(a, "character"))
        b.run("give 43 1")
        check("the guest who sent it still plays: its pack still reaches the host",
              bool(a.wait("B's herb", lambda s: s["pack"].get("43", 0) >= 1)))

        b.run("forge saves")
        b.run("give 43 1")
        check("the host still answers after a flood of saves",
              bool(a.wait("B's second herb", lambda s: s["pack"].get("43", 0) >= 2)))
        everyone_stays("after forged character and saves")

        # A damaged snapshot of A's floor: shown to an arrival, then handed to its heir.
        a.run("forge snapshots")
        c.run("warp 2")
        coop_floors.wait_part(c, "running", 2)
        c.run("warp 1")
        c.wait("C back on floor 1", lambda s: s["floor"] == 1)
        check("a damaged snapshot of a floor shown is refused", wait_refused(c, "floorState"))
        check("the arrival draws the owner's floor all the same",
              coop_floors.wait_part(c, "watching", 1)["floorPart"] == "watching")
        a.run("move %d %d" % coop_floors.FLOOR1_ENEMIES)
        c.run("move %d %d" % coop_floors.FLOOR1_ENEMIES)
        coop_floors.wait_actors(a, "awake enemies", bool)
        check("and its actors, where the owner has them", coop_floors.wait_same(a, c))

        a.run("warp 2")
        check("a damaged handover is refused", wait_refused(b, "floorState"))
        check("the heir runs the floor all the same", coop_floors.wait_part(b, "running", 1)["floor"] == 1)
        check("the third player draws the heir's", coop_floors.wait_part(c, "watching", 1)["floorPart"] == "watching")
        everyone_stays("after damaged snapshots")
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
    parser.add_argument("--room", default=f"forgedtest{random.randrange(1 << 30)}")
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
