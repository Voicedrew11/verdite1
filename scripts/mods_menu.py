#!/usr/bin/env python3
"""The title menu's mods, headless through the agent channel: a mod's own menu; a save keeping the list of
mods it was made with and warning before CONTINUE and F4 load it when the mods differ; and COOP warning
that mods stay off in there. docs/MODDING.md, "A mod's menu" and "Saves and the mods they were made with".

    scripts/mods_menu.py build/linux/verdite1 [--data data/en]
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import smoke  # noqa: E402

PORT = 27941  # not the game's 27901, smoke.py's 27911, coop_pair.py's or title_pair.py's
TAP_S = 0.35
# Notes each save and load in the log..
WATCHER = """
function on_load_game(e) mod.log("loaded slot " .. e.slot) end
function on_save(e) mod.log("saved slot " .. e.slot) end
-- The channel presses buttons, not actions: press.txt holds a count and the action to press.
local seen = 0
function on_update()
  local text = mod.fs.read("press.txt")
  if not text then return end
  local count, action = text:match("(%d+) (%S+)")
  if count and tonumber(count) ~= seen and kf.context() == "gameplay" then
    seen = tonumber(count)
    kf.action_press(action)
  end
end
"""


def check(what, good, shown=""):
    if not good:
        raise RuntimeError(f"failed: {what} {json.dumps(shown)}")
    print(f"ok: {what}")


class Game:
    def __init__(self, binary, data, home):
        self.home, self.saves = home, home / "saves"
        self.presses = int(time.monotonic())   # a new count each run, as the mod's starts at 0
        self.saves.mkdir(parents=True, exist_ok=True)
        self.log_path = home / "log.txt"
        env = dict(os.environ, KF_SHELL=str(PORT), XDG_DATA_HOME=str(home))
        self.log = open(self.log_path, "a")
        self.process = subprocess.Popen([binary, "--data", data, "--saves", str(self.saves), "--headless"],
                                        env=env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while True:
            try:
                self.channel = socket.create_connection(("127.0.0.1", PORT), timeout=10)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError("no command channel")
                time.sleep(0.2)
        self.reader = self.channel.makefile("r")
        problem = smoke.walk_setup(self.channel, self.reader)
        if problem:
            raise RuntimeError(problem)

    def run(self, line):
        return smoke.command(self.channel, self.reader, line)

    def title(self):
        return self.run("title")

    def tap(self, button):
        self.run(f"press {button} 100")
        time.sleep(TAP_S)

    def to_title(self):
        deadline = time.monotonic() + 90
        while not self.title()["open"] or not self.title()["rows"]:
            if time.monotonic() > deadline:
                raise RuntimeError("the title menu never came up")
            self.tap("start")
        return self.title()

    def rows(self):
        return self.title()["rows"]

    def row(self, text):
        return next((row for row in self.rows() if row["text"] == text), None)

    def go_to(self, text):
        """Moves the cursor to the row with this text."""
        for _ in range(60):
            rows = self.rows()
            selected = next((i for i, row in enumerate(rows) if row["selected"]), 0)
            target = next((i for i, row in enumerate(rows) if row["text"] == text), None)
            if target is None:
                raise RuntimeError(f"no row {text}: {rows}")
            if target == selected:
                return
            across = not self.title()["heading"]
            if target > selected:
                self.tap("right" if across else "down")
            else:
                self.tap("left" if across else "up")
        raise RuntimeError(f"the cursor never reached {text}")

    def choose(self, text):
        self.go_to(text)
        self.tap("confirm")

    def press_action(self, action):
        self.presses += 1
        (self.home / "Verdite/Verdite1/mods/watcher/press.txt").write_text(f"{self.presses} {action}")
        time.sleep(TAP_S)

    def wait_log(self, text, seconds=60):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if text in self.log_path.read_text(errors="replace"):
                return True
            time.sleep(0.25)
        return False

    def quit(self):
        self.run("quit")
        self.process.wait(timeout=60)
        self.log.close()
        self.channel.close()


def own_menu_and_coop(game):
    game.to_title()
    game.choose("MODS")
    check("MODS lists only the two mods off, sorted", [(r["text"], r["value"]) for r in game.rows()][:2] == [("DEBUG", "OFF"), ("THE EMBER CRYPT", "OFF")], game.rows())
    game.choose("THE EMBER CRYPT")
    check("a mod's page opens with its own heading", game.title()["heading"] == "THE EMBER CRYPT", game.title())
    check("its number row starts at the default", game.row("BRUTES")["value"] == "3", game.rows())
    check("its choice row shows the choice's label", game.row("REWARD")["value"] == "200 EXP", game.rows())
    game.go_to("BRUTES")
    game.tap("right")
    check("Right steps a number", game.row("BRUTES")["value"] == "4", game.rows())
    game.tap("left")
    game.go_to("REWARD")
    game.tap("confirm")
    check("Confirm steps a choice", game.row("REWARD")["value"] == "500 EXP", game.rows())
    game.go_to("DOUBLE EXP")
    game.tap("left")
    check("Left steps a toggle", game.row("DOUBLE EXP")["value"] == "OFF", game.rows())
    game.tap("back")
    game.choose("DEBUG")
    game.go_to("NOCLIP")
    game.tap("confirm")
    check("a disabled mod's setting can be set", game.row("NOCLIP")["value"] == "ON", game.rows())
    options = smoke.option_rows(game.channel, game.reader)
    check("the option table has the mod's rows", options["mods.debug.noclip"]["value"] is True, options.get("mods.debug.noclip"))
    game.go_to("SWITCH")
    game.tap("confirm")
    check("the page's switch turns the mod on", game.row("SWITCH")["value"] == "ON", game.rows())
    game.tap("back")
    check("Back lands on the mod", next(r for r in game.rows() if r["selected"])["text"] == "DEBUG", game.rows())
    game.tap("back")

    game.choose("COOP")
    title = game.title()
    check("COOP warns while a mod is on", title["heading"] == "WARNING" and "COOP" in title["note"], title)
    game.choose("BACK")
    check("BACK from the warning is the top page", game.title()["heading"] == "", game.title())
    game.choose("COOP")
    game.choose("ENTER")
    check("ENTER goes on to COOP", game.title()["heading"] == "COOP", game.title())
    game.tap("back")
    check("Back from COOP skips the warning", game.title()["heading"] == "", game.title())


def make_save(game, binary_saves):
    game.choose("NEW GAME")
    deadline = time.monotonic() + 120
    while game.title()["open"] or game.run("state")["state"].get("context") != "gameplay":
        if time.monotonic() > deadline:
            raise RuntimeError(f"never reached gameplay: {game.run('state')}")
        game.tap("start")
    game.press_action("quick_save")
    check("the game quick saved", game.wait_log("[watcher] saved slot 4"))
    saved = json.loads((binary_saves / "quick.modlist.json").read_text())
    check("the quick save names its mods", sorted(m["id"] for m in saved["mods"]) == ["debug", "watcher"], saved)
    # The quick card is the game's format, with the quick save as its first slot.
    shutil.copy(binary_saves / "BISLPS-00017KF.quick", binary_saves / "BISLPS-00017KF")
    shutil.copy(binary_saves / "quick.modlist.json", binary_saves / "slot1.modlist.json")
    (game.home / "Verdite/Verdite1/mods/watcher/press.txt").unlink()   # or the next run presses it again


def warned_at_continue(game):
    game.to_title()
    game.choose("MODS")
    game.choose("DEBUG")
    game.go_to("SWITCH")
    game.tap("confirm")
    check("debug is off again", game.row("SWITCH")["value"] == "OFF", game.rows())
    game.tap("back")
    game.tap("back")
    game.choose("CONTINUE")
    slot = game.rows()[0]
    check("the slot says how many mods it has", slot["value"] == "2 MODS", slot)
    game.tap("confirm")
    title = game.title()
    check("a save with other mods warns", title["heading"] == "WARNING" and "OTHER MODS" in title["note"], title)
    check("the cursor starts on BACK", next(r for r in title["rows"] if r["selected"])["text"] == "BACK", title)
    check("it names what differs", [(r["text"], r["value"]) for r in title["rows"]][2:] == [("DEBUG", "OFF NOW")], title)
    game.choose("BACK")
    check("BACK is the slots again", game.title()["heading"] == "LOAD", game.title())
    game.tap("confirm")
    game.choose("LOAD ANYWAY")
    check("LOAD ANYWAY loads it", game.wait_log("[watcher] loaded slot 1"))


def warned_at_quick_load(game):
    log_before = game.log_path.read_text(errors="replace").count("loaded slot 4")
    game.press_action("quick_load")
    time.sleep(1)
    count = game.log_path.read_text(errors="replace").count("loaded slot 4")
    check("the first F4 does not load", count == log_before, count)
    game.press_action("quick_load")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and game.log_path.read_text(errors="replace").count("loaded slot 4") == log_before:
        time.sleep(0.2)
    check("the second F4 loads", game.log_path.read_text(errors="replace").count("loaded slot 4") > log_before)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--data", default=str(ROOT / "data/en"))
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory() as temp:
        home = Path(temp)
        mods = home / "Verdite" / "Verdite1" / "mods"
        shutil.copytree(ROOT / "mods" / "debug", mods / "debug")
        shutil.copytree(ROOT / "examples" / "mods" / "crypt", mods / "crypt")
        (mods / "watcher").mkdir()
        (mods / "watcher/mod.json").write_text('{"id": "watcher", "name": "Watcher", "default_enabled": true}')
        (mods / "watcher/main.lua").write_text(WATCHER)

        first = Game(binary, args.data, home)
        try:
            own_menu_and_coop(first)
            make_save(first, first.saves)
            first.quit()
        except BaseException:
            first.process.kill()
            print(first.log_path.read_text(errors="replace")[-2000:])
            raise
        second = Game(binary, args.data, home)
        try:
            warned_at_continue(second)
            warned_at_quick_load(second)
            second.quit()
        except BaseException:
            second.process.kill()
            print(second.log_path.read_text(errors="replace")[-2000:])
            raise
    print("mods menu: a mod's own menu, the coop warning, a save's list of mods and its warnings")


if __name__ == "__main__":
    main()
