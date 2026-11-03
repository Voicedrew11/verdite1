#!/usr/bin/env python3
"""The multiplayer settings, with headless instances in one coop room on a local service
(online/README.md: npm run dev): what only the host is shown, the host's tick rate and revive wait
followed by a guest, the name bars and the feed switched off and on, a room closed to new players
but open to a returning one, and a guest sent away and refused for the session.
docs/COOP_PLAN.md, "Multiplayer settings".

    scripts/coop_settings.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
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
from coop_pair import Game  # noqa: E402

PORTS = (27941, 27942, 27943)  # not the game's 27901, smoke.py's 27911 or the other pairs'


def rows_of(game):
    """Every row the options view shows now, by id."""
    rows = {}
    for page in game.run("options")["options"]["pages"]:
        for section in page["sections"]:
            for row in section["rows"]:
                rows[row["id"]] = row
    return rows


def main():
    checks = []

    def check(what, ok):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what)

    def turned_away(game, line):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if line in (game.home / "log.txt").read_text():
                return True
            time.sleep(0.25)
        return False

    a = Game("a", PORTS[0], profile="name=ALICE\n")
    a.wait("the host's world", lambda s: s["inPlay"])
    b = Game("b", PORTS[1], profile="name=BOB7\n")
    b.wait("the guest's arrival", lambda s: s["inPlay"])
    a.wait("the roster", lambda s: len(s["roster"]) == 2)
    b.wait("the roster", lambda s: len(s["roster"]) == 2)

    host_rows, guest_rows = rows_of(a), rows_of(b)
    check("the host is shown its room's rows", all(i in host_rows for i in ("online.room_open", "online.revive_wait_s")))
    check("a guest is not", not any(i in guest_rows for i in ("online.room_open", "online.revive_wait_s")))
    check("everyone is shown their own", all("online.name_bars" in r and "online.pickup_feed" in r for r in (host_rows, guest_rows)))
    check("the host sees its guest by name", host_rows.get("online.send_away.0", {}).get("note") == "BOB7")
    check("a guest cannot change the tick rate", not guest_rows["gameplay.tick_rate"]["enabled"])
    check("the host can", host_rows["gameplay.tick_rate"]["enabled"])

    a.run("option gameplay.tick_rate 30")
    state = b.wait("the host's tick rate", lambda s: s["tickVblanks"] == 2)
    check("a guest follows the host's tick rate", state["tickVblanks"] == 2)
    a.run("option online.revive_wait_s 10")
    state = b.wait("the host's revive wait", lambda s: s["reviveWaitS"] == 10)
    check("a guest follows the host's revive wait", state["reviveWaitS"] == 10)

    # A guest arrives on the host's cell, too near for a bar, so the host steps to wherever the guest looks.
    x, z = b.coop()["cell"]
    drawn = 0
    for dx, dz in ((0, 3), (0, -3), (3, 0), (-3, 0)):
        a.run(f"move {x + dx} {z + dz}")
        time.sleep(1.5)
        drawn = b.coop()["nameBarsDrawn"]
        if drawn > 0:
            break
    check("a guest draws the name bar of a host in view", drawn > 0)
    b.run("option online.name_bars 0")
    time.sleep(0.5)
    stopped = b.coop()["nameBarsDrawn"]
    time.sleep(1)
    check("name bars off draws none", b.coop()["nameBarsDrawn"] == stopped)
    b.run("option online.name_bars 1")
    b.wait("name bars in sight", lambda s: s["nameBarsDrawn"] > stopped)
    check("name bars in sight draws them again", True)

    a.run("option online.pickup_feed false")
    b.run("give 21 1")
    a.wait("the guest's find", lambda s: s["pack"].get("21") == 1)
    check("the feed off shows no line", a.coop()["feed"] == [])
    a.run("option online.pickup_feed true")
    b.run("give 22 1")
    state = a.wait("the feed's line", lambda s: s["feed"] != [])
    check("the feed on shows the next", "BOB7" in state["feed"][-1][0])

    a.run("option online.room_open false")
    c = Game("c", PORTS[2], profile="name=CAROL\n")
    check("a closed room turns a new player away", turned_away(c, "The host is not letting new players in."))
    state = c.wait("leaving", lambda s: s["role"] == "none")
    check("who was turned away is out of the room", state["role"] == "none")
    c.quit()
    b.quit()
    a.wait("the guest leaving", lambda s: len(s["roster"]) == 1)
    b = Game("b", PORTS[1])
    b.wait("the guest's return", lambda s: s["inPlay"])
    check("a closed room still lets a returning player in", True)
    a.run("option online.room_open true")
    a.wait("the roster", lambda s: len(s["roster"]) == 2)

    a.run("option online.send_away.0")
    check("a guest sent away is told", turned_away(b, "The host sent you away."))
    state = b.wait("leaving", lambda s: s["role"] == "none")
    check("a guest sent away is out of the room", state["role"] == "none")
    check("the party goes on without them", len(a.wait("the roster", lambda s: len(s["roster"]) == 1)["roster"]) == 1)
    check("nothing is lent to them any more", a.coop()["othersWear"] == [])
    b.quit()
    (b.home / "log.txt").write_text("")
    b = Game("b", PORTS[1])
    check("who was sent away is refused for the session", turned_away(b, "The host sent you away."))
    b.quit()
    a.quit()

    failed = [what for what, ok in checks if not ok]
    print(f"{len(checks) - len(failed)} of {len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--room", default=f"cooptest{random.randrange(1 << 30)}")
    parser.add_argument("--service", default="http://127.0.0.1:8787")
    parser.add_argument("--work", help="a new folder for the instances' files and logs, kept")
    args = parser.parse_args()
    coop_pair.BINARY = args.binary
    coop_pair.ROOM = args.room
    coop_pair.SERVICE = args.service
    coop_pair.WORK = Path(args.work or tempfile.mkdtemp(prefix="coop-settings-"))
    try:
        sys.exit(main())
    finally:
        coop_pair.kill_all()
