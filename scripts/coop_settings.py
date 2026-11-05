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

    solo = Game("solo", PORTS[2], room=None)
    deadline = time.monotonic() + 60
    while solo.run("state")["state"]["context"] != "menu" and time.monotonic() < deadline:   # past the opening's script
        solo.run("press start 100")
        time.sleep(1)
    time.sleep(1)
    check("alone the pause menu has no MULTIPLAYER row", len([r for r in solo.run("menu")["rows"] if r["input"]]) == 9)
    solo.quit()

    a = Game("a", PORTS[0], profile="name=ALICE\n")
    a.wait("the host's world", lambda s: s["inPlay"])
    b = Game("b", PORTS[1], profile="name=BOB7\n")
    b.wait("the guest's arrival", lambda s: s["inPlay"])
    a.wait("the roster", lambda s: len(s["roster"]) == 2)
    b.wait("the roster", lambda s: len(s["roster"]) == 2)

    def open_multiplayer(game):
        """The pause menu's rows, then the rows of what its row under PORT SETTINGS opens."""
        game.run("press start 100")
        time.sleep(1)
        top = [row for row in game.run("menu")["rows"] if row["input"]]
        game.run("press up 100")      # round to Return
        time.sleep(0.4)
        game.run("press up 100")      # MULTIPLAYER
        time.sleep(0.4)
        game.run("press confirm 100")
        time.sleep(1)
        return top, [row for row in game.run("menu")["rows"] if row["input"]]

    def leave_menu(game, presses):
        for _ in range(presses):
            game.run("press back 100")
            time.sleep(0.4)

    top, page = open_multiplayer(b)
    check("in a room the pause menu has a row more, on the picture",
          len(top) == 10 and max(row["box"][1] + row["box"][3] for row in top) <= 240)
    check("a guest's MULTIPLAYER has this player's own rows", len(page) == 2)
    leave_menu(b, 2)
    top, page = open_multiplayer(a)
    check("a host's MULTIPLAYER opens the same", len(top) == 10 and len(page) == 2)
    a.run("press strafe_right 100")
    time.sleep(0.5)
    check("a host's next page is its room's", len([row for row in a.run("menu")["rows"] if row["input"]]) == 6)
    leave_menu(a, 2)

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

    # Neither has turned Messages on: a room has them on regardless.
    a.wait("the host's Message Stone", lambda s: s["messageStone"])
    b.wait("the guest's Message Stone", lambda s: s["messageStone"])
    check("everyone in a room is offered the Message Stone", True)

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
    check("a guest draws a crown on the host's name", b.coop()["crownsDrawn"] > 0)
    check("no crown is drawn for a guest's name", a.coop()["crownsDrawn"] == 0)
    check("both draw the other's dot around the compass", a.coop()["compassDotsDrawn"] > 0 and b.coop()["compassDotsDrawn"] > 0)
    b.run("option online.name_bars 0")
    time.sleep(0.5)
    stopped = b.coop()["nameBarsDrawn"]
    time.sleep(1)
    check("name bars off draws none", b.coop()["nameBarsDrawn"] == stopped)
    b.run("option online.name_bars 1")
    b.wait("name bars in sight", lambda s: s["nameBarsDrawn"] > stopped)
    check("name bars in sight draws them again", True)

    # Past the guest's cell window (14 cells across), in sight draws no bar and through walls draws one.
    # The guest's facing is not in the state, so each of the four ways out is tried until the bar shows.
    x, z = b.coop()["cell"]

    def bars_past_window(mode):
        b.run(f"option online.name_bars {mode}")
        time.sleep(1)
        before = b.coop()["nameBarsDrawn"]
        time.sleep(1.5)
        return b.coop()["nameBarsDrawn"] - before

    through, far = 0, None
    for dx, dz in ((12, 0), (-12, 0), (0, 12), (0, -12)):
        cell = [x + dx, z + dz]
        a.run(f"move {cell[0]} {cell[1]}")
        try:
            a.wait("the host past the guest's window", lambda s, cell=cell: s["cell"] == cell, seconds=10)
        except RuntimeError:
            continue
        through = bars_past_window(2)
        if through > 0:
            far = cell
            break
    check("name bars through walls draw one past the window", through > 0)
    if far is not None:
        a.wait("the host past the guest's window", lambda s: s["cell"] == far)
        check("name bars in sight draw none at that cell", bars_past_window(1) == 0)

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

    coop_pair.check_nothing_refused(check)
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
