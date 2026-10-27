#!/usr/bin/env python3
"""The title menu, driven through the agent channel by headless instances on a local service
(online/README.md: npm run dev): its top page, a new game from it, a private world hosted from it and
joined by its code, a public world joined from the list, a code no one hosts, and the way back to the
title when the host leaves, and CONTINUE loading a card slot. docs/COOP_PLAN.md, "The title menu".

    scripts/title_pair.py build/linux/verdite1 [--service URL] [--work DIR]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import smoke  # noqa: E402

PORTS = (27931, 27932)  # not the game's 27901, smoke.py's 27911 or coop_pair.py's
BINARY = ""
WORK = Path()
SERVICE = ""
STARTED = []
TAP_S = 0.35   # a press and its release, so the next press is a new one


def kill_all():
    for process in STARTED:
        if process.poll() is None:
            process.kill()


class Game:
    def __init__(self, name, port, extra_args=(), extra_env=None):
        self.name = name
        self.home = WORK / name
        (self.home / "saves").mkdir(parents=True, exist_ok=True)
        # Reported, not halted on: libjuice's picohash.h is UB under UBSan once two peers connect.
        env = dict(os.environ, KF_SHELL=str(port), XDG_DATA_HOME=str(self.home), KF_ONLINE_SERVICE=SERVICE,
                   UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1", ASAN_OPTIONS="abort_on_error=0:detect_leaks=0",
                   **(extra_env or {}))
        self.log = open(self.home / "log.txt", "a")
        self.process = subprocess.Popen(
            [BINARY, "--data", str(ROOT / "data/en"), "--saves", str(self.home / "saves"), "--headless", *extra_args],
            env=env, stdout=self.log, stderr=subprocess.STDOUT)
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

    def title(self):
        return self.run("title")

    def tap(self, button):
        self.run(f"press {button} 100")
        time.sleep(TAP_S)

    def wait(self, what, read, test, seconds=60):
        deadline = time.monotonic() + seconds
        while True:
            state = read()
            if test(state):
                return state
            if time.monotonic() > deadline:
                raise RuntimeError(f"{self.name}: {what} never came: {json.dumps(state)}")
            time.sleep(0.25)

    def to_title(self):
        """Presses Start through the attract scene until the menu is up."""
        deadline = time.monotonic() + 90
        while not self.title()["open"]:
            if time.monotonic() > deadline:
                raise RuntimeError(f"{self.name}: the title menu never came up")
            self.tap("start")
        return self.wait("the menu's first frame", self.title, lambda t: t["rows"])

    def choose(self, text):
        """Moves the cursor to the row with this text and confirms it."""
        for _ in range(60):
            rows = self.title()["rows"]
            selected = next((i for i, row in enumerate(rows) if row["selected"]), 0)
            target = next((i for i, row in enumerate(rows) if row["text"] == text), None)
            if target is None:
                raise RuntimeError(f"{self.name}: no row {text}: {json.dumps(rows)}")
            if selected == target:
                self.tap("confirm")
                return
            self.tap("down" if target > selected else "up")
        raise RuntimeError(f"{self.name}: the cursor never reached {text}")

    def coop(self):
        return self.run("coop")

    def quit(self):
        try:
            self.channel.sendall(b"quit\n")
        except OSError:
            pass
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()


def main():
    checks = []

    def check(what, ok, detail=None):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what + ("" if ok or detail is None else f": {json.dumps(detail)}"))

    a = Game("a", PORTS[0])
    b = None
    try:
        top = a.to_title()
        texts = [row["text"] for row in top["rows"]]
        check("the top page is CONTINUE, NEW GAME, COOP", texts == ["CONTINUE", "NEW GAME", "COOP"], texts)
        check("with no save, CONTINUE is off and the cursor on NEW GAME",
              not top["rows"][0]["enabled"] and top["rows"][1]["selected"], top["rows"])

        a.choose("COOP")
        a.choose("HOST")
        a.choose("NEW WORLD")   # the first is PRIVATE
        code_page = a.title()
        code = code_page["code"]
        check("a new private world shows an 8-digit code", len(code) == 8 and code.isdigit(), code_page)
        a.choose("START")
        host = a.wait("the host's world", a.coop, lambda s: s["inPlay"])
        check("A hosts the world from the title", host["role"] == "host" and not a.title()["open"], host)

        b = Game("b", PORTS[1])
        b.to_title()
        b.choose("COOP")
        b.choose("JOIN BY CODE")
        b.run("title type " + code)
        check("the join page shows the code typed", b.title()["code"] == code, b.title())
        b.choose("JOIN")
        guest = b.wait("the guest's arrival", b.coop, lambda s: s["inPlay"])
        check("B joins A's world by its code", guest["role"] == "guest" and not b.title()["open"], guest)
        host = a.wait("B's kit", a.coop, lambda s: s["pack"].get("0") == 2)
        check("B's kit joins the pack", host["gold"] == 300, host)

        print("A quits; B goes back to the title")
        a.quit()
        a = None
        back = b.wait("the title", b.title, lambda t: t["open"] and t["rows"])
        check("B is back at the title menu", [row["text"] for row in back["rows"]] == ["CONTINUE", "NEW GAME", "COOP"], back)
        check("B left the room", b.coop()["role"] == "none")

        b.choose("COOP")
        b.choose("JOIN BY CODE")
        b.run("title type " + code)
        b.choose("JOIN")
        failed = b.wait("the refusal", b.title, lambda t: t["note"] != "" and t["heading"] == "ROOM CODE")
        check("a code no one hosts is refused, saying so", failed["note"] == "NO ONE IS HOSTING THAT ROOM", failed)
        b.tap("back")
        b.tap("back")
        b.tap("back")

        a = Game("a", PORTS[0])
        a.to_title()
        a.choose("COOP")
        a.choose("HOST")
        rows = a.title()["rows"]
        check("A's private world is offered again under its code", any(row["text"] == code for row in rows), rows)
        a.tap("down")   # NEW WORLD, PUBLIC
        a.tap("confirm")
        host = a.wait("the public world", a.coop, lambda s: s["inPlay"])
        check("A hosts a public world", host["role"] == "host", host)

        b.choose("COOP")
        b.choose("PUBLIC ROOMS")
        listing = b.wait("A's room on the list", b.title, lambda t: any(row["text"] == host["name"] for row in t["rows"]))
        row = next(row for row in listing["rows"] if row["text"] == host["name"])
        check("the list shows A's room: its host, floor and players", row["value"] == "FLOOR 1  1/4" and row["enabled"], row)
        b.choose(host["name"])
        guest = b.wait("the guest's arrival", b.coop, lambda s: s["inPlay"])
        check("B joins the public world from the list", guest["role"] == "guest", guest)
    finally:
        for game in (a, b):
            if game:
                game.quit()

    # NEW GAME starts a game: the menu is gone and the player stands on floor 1.
    a = Game("a", PORTS[0])
    try:
        a.to_title()
        a.choose("NEW GAME")
        state = a.wait("a new game", lambda: smoke.state(a.channel, a.reader), lambda s: s.get("inGame"), seconds=120)
        check("NEW GAME starts a game", not a.title()["open"], state)
    finally:
        a.quit()

    # CONTINUE: a quick save's file is a card with the save in its first slot, so one made with a
    # Wind Blade Bracelet in the pack, put in as the card, is slot 1.
    c = Game("c", PORTS[0], ["--skip-intro"], {"KF_TEST_GIVE": "50", "KF_TEST_QUICK": "save@20"})
    try:
        quick = c.home / "saves" / "BISLPS-00017KF.quick"
        c.wait("the quick save", lambda: quick.exists(), lambda made: made)
    finally:
        c.quit()
    d_saves = WORK / "d" / "saves"
    d_saves.mkdir(parents=True, exist_ok=True)
    (d_saves / "BISLPS-00017KF").write_bytes(quick.read_bytes())
    d = Game("d", PORTS[1])
    try:
        top = d.to_title()
        check("with a save, the cursor starts on CONTINUE", top["rows"][0]["enabled"] and top["rows"][0]["selected"], top)
        d.tap("confirm")
        slots = d.title()["rows"]
        check("CONTINUE lists slot 1 and two empty ones",
              [row["enabled"] for row in slots] == [True, False, False] and slots[0]["text"].startswith("1  FLOOR 1")
              and slots[1]["text"] == "2  EMPTY", slots)
        d.tap("confirm")
        loaded = d.wait("slot 1 in play", d.coop, lambda s: s["pack"].get("50") == 1, seconds=120)
        check("CONTINUE loads slot 1", not d.title()["open"], loaded)
    finally:
        d.quit()

    failed = [what for what, ok in checks if not ok]
    if failed:
        for log in sorted(WORK.glob("*/log.txt")):
            print(f"--- {log.parent.name}'s log, last lines:")
            print("".join(log.read_text(errors="replace").splitlines(keepends=True)[-15:]))
    print("FAIL" if failed else "PASS", f"({len(checks) - len(failed)} of {len(checks)})")
    return 1 if failed else 0


def parse():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary")
    parser.add_argument("--service", default="http://127.0.0.1:8787")
    parser.add_argument("--work", help="where the instances keep their folders (default: a new temporary one)")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse()
    BINARY = str(Path(args.binary).resolve())
    WORK = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="title-pair-"))
    SERVICE = args.service
    print(f"instances in {WORK}, service {SERVICE}")
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - reported, then every instance stopped
        print(f"FAIL {error}")
        sys.exit(1)
    finally:
        kill_all()
