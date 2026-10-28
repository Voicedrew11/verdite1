#!/usr/bin/env python3
"""Two headless instances in one coop room on a local service (online/README.md: npm run dev): the
host's world, a guest's arrival, the pack settled both ways and in the feed, the guest's own game
put back when the host quits and in the next run after the guest quits, and the host's file.
docs/COOP_PLAN.md, "The coop save".

    scripts/coop_pair.py build/linux/verdite1 [--room R] [--service URL] [--work DIR]
"""
import argparse
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import smoke  # noqa: E402

PORTS = (27921, 27922)  # not the game's 27901, nor smoke.py's 27911
BINARY = ""
WORK = Path()
ROOM = ""
SERVICE = ""
STARTED = []


def kill_all():
    for process in STARTED:
        if process.poll() is None:
            process.kill()


class Game:
    def __init__(self, name, port, room="", give=None):
        """room: the test's room by default, None for none."""
        room = ROOM if room == "" else room
        self.name = name
        self.home = WORK / name
        (self.home / "saves").mkdir(parents=True, exist_ok=True)
        # Reported, not halted on: libjuice's picohash.h is UB under UBSan once two peers connect.
        env = dict(os.environ, KF_SHELL=str(port), XDG_DATA_HOME=str(self.home),
                   KF_ONLINE_SERVICE=SERVICE,
                   UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1", ASAN_OPTIONS="abort_on_error=0:detect_leaks=0")
        if give:
            env["KF_TEST_GIVE"] = give
        self.log = open(self.home / "log.txt", "a")
        self.process = subprocess.Popen(
            [BINARY, "--data", str(ROOT / "data/en"), "--saves", str(self.home / "saves"), "--headless",
             "--skip-intro", *(["--online-room", room] if room else [])], env=env, stdout=self.log,
            stderr=subprocess.STDOUT)
        STARTED.append(self.process)
        deadline = time.monotonic() + 60
        while True:
            try:
                self.channel = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError(f"{name}: no command channel")
                time.sleep(0.2)
        self.reader = self.channel.makefile("r")
        problem = smoke.walk_setup(self.channel, self.reader)
        if problem:
            raise RuntimeError(f"{name}: {problem}")

    def run(self, line):
        return smoke.command(self.channel, self.reader, line)

    def coop(self):
        return self.run("coop")

    def wait(self, what, test, seconds=30):
        deadline = time.monotonic() + seconds
        while True:
            state = self.coop()
            if test(state):
                return state
            if time.monotonic() > deadline:
                raise RuntimeError(f"{self.name}: {what} never came: {json.dumps(state)}")
            time.sleep(0.25)

    def quit(self):
        try:
            self.channel.sendall(b"quit\n")
        except OSError:
            pass
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()


def show(label, state):
    keep = {k: state[k] for k in ("role", "inPlay", "pack", "gold", "worn", "othersWear", "floor", "cell", "highestFloor")}
    print(f"{label}: {json.dumps(keep)}")


def main():
    checks = []

    def check(what, ok):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what)

    a = Game("a", PORTS[0])
    try:
        host = a.wait("the host's world", lambda s: s["inPlay"])
        show("A alone", host)
        check("A hosts", host["role"] == "host")
        check("A's new world has one kit", host["pack"] == {"0": 1, "43": 1} and host["gold"] == 150)
        a.run("give 20 1")   # a Knight Plate found
        a.wait("the plate", lambda s: s["pack"].get("20") == 1)

        b = Game("b", PORTS[1])
        try:
            own = b.coop()
            guest = b.wait("the guest's arrival", lambda s: s["inPlay"])
            show("B arrived", guest)
            check("B is a guest", guest["role"] == "guest")
            host = a.wait("B's kit", lambda s: s["pack"].get("0") == 2)
            show("A with B", host)
            check("B's kit joins the pack", host["pack"] == {"0": 2, "20": 1, "43": 2} and host["gold"] == 300)
            check("B sees the party's pack", b.wait("the pack", lambda s: s["pack"] == host["pack"])["gold"] == 300)
            check("B arrived beside A", guest["floor"] == host["floor"] and guest["cell"] == host["cell"])
            check("each sees the other's sword", host["othersWear"] == [[0, 255, 255, 255, 255, 255, 255]]
                  and b.coop()["othersWear"] == [[0, 255, 255, 255, 255, 255, 255]])

            b.run("give 43 -1")   # B uses an herb
            host = a.wait("B's herb used", lambda s: s["pack"].get("43") == 1)
            b_name = b.coop()["name"]
            check("A's feed says B used an herb", [b_name + " USED ", 43, ""] in host["feed"])
            check("B's own feed leaves it out", not any(row[0].startswith(b_name) for row in b.coop()["feed"]))
            b.run("give 255 -100")   # and spends gold
            host = a.wait("B's gold spent", lambda s: s["gold"] == 200)
            check("the feed says B used gold", [b_name + " USED 100 GOLD", 255, ""] in host["feed"])
            check("A has B's herb and gold", host["pack"].get("43") == 1 and host["gold"] == 200)
            a.run("give 43 5")
            guest = b.wait("A's herbs", lambda s: s["pack"].get("43") == 6)
            check("B has A's herbs", guest["gold"] == 200)
            # The last herb, used by both within a round trip: both heal, and the pack stays at 0.
            a.run("give 43 -6")
            b.run("give 43 -6")
            host = a.wait("no herbs", lambda s: "43" not in s["pack"])
            guest = b.wait("no herbs", lambda s: "43" not in s["pack"])
            time.sleep(2)
            check("the herbs stay at 0 on both", "43" not in a.coop()["pack"] and "43" not in b.coop()["pack"])
        finally:
            print("A quits; B is left alone")
            a.quit()
            a = None
        guest = b.wait("B's own game back", lambda s: not s["inPlay"], seconds=60)
        show("B back", guest)
        check("B's own game is back", guest["pack"] == own["pack"] and guest["gold"] == own["gold"])
        b.quit()
    finally:
        if a:
            a.quit()

    # B quits while in the room; its own game, set aside, comes back in its next run.
    a = Game("a", PORTS[0])
    b = Game("b", PORTS[1], give="50")   # B's own game holds a Wind Blade Bracelet
    try:
        a.wait("the host's world", lambda s: s["inPlay"])
        b.wait("the guest's arrival", lambda s: s["inPlay"])
        check("B's bracelet stays in its own game", "50" not in b.coop()["pack"])
        b.run("give 43 3")
        a.wait("B's herbs", lambda s: s["pack"].get("43") == 3)
        b.quit()
        b = Game("b", PORTS[1], room=None)
        alone = b.wait("B's own game back", lambda s: s["pack"] == {"0": 1, "43": 1, "50": 1})
        check("B's own game came back after quitting in the room", alone["role"] == "none" and alone["gold"] == 150)
        check("the set-aside file is gone", not (WORK / "b" / "saves" / "BISLPS-00017KF.aside").exists())
        b.quit()
    finally:
        a.quit()

    world = WORK / "a" / "saves" / f"coop-{ROOM}.kfcoop"
    check("A's world is in its file", world.exists())
    a = Game("a", PORTS[0])
    try:
        host = a.wait("the host's world", lambda s: s["inPlay"])
        show("A again", host)
        check("the world kept the pack", host["pack"] == {"0": 2, "20": 1, "43": 3} and host["gold"] == 200)
    finally:
        a.quit()
    failed = [what for what, ok in checks if not ok]
    if failed:
        for log in sorted(WORK.glob("*/log.txt")):
            print(f"--- {log.parent.name}'s log, last lines:")
            print("".join(log.read_text(errors="replace").splitlines(keepends=True)[-15:]))
    print("FAIL" if failed else "PASS", f"({len(checks) - len(failed)} of {len(checks)})")
    return 1 if failed else 0


def parse():
    global BINARY, WORK, ROOM, SERVICE
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--room", default=f"cooptest{random.randrange(1 << 30)}")
    parser.add_argument("--service", default="http://127.0.0.1:8787")
    parser.add_argument("--work", help="a new folder for the instances' files and logs, kept")
    args = parser.parse_args()
    BINARY, ROOM, SERVICE = args.binary, args.room, args.service
    return args.work


if __name__ == "__main__":
    kept = parse()
    with tempfile.TemporaryDirectory() as work:
        WORK = Path(kept or work)
        try:
            sys.exit(main())
        finally:
            kill_all()
