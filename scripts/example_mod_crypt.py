#!/usr/bin/env python3
"""Play mods/crypt, the example of the layers that change the game, through its whole loop, headless.

Copies the mod and a floor made from the disc to a fresh user folder, with a second mod that watches it
through mods.get and mods.on. Uses the sigil from the Use list, kills the brutes in the crypt, uses their
draught, sees experience double, goes back, and quick saves and loads. Then again without the floor's files,
where the sigil rouses brutes on the spot. Fails on any mod error, a sanitizer report or a dirty exit.
Usage: scripts/example_mod_crypt.py BINARY [--data data/en]
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
PORT = 27912  # not smoke.py's or the game's
TIMEOUT = 120
DRAUGHT, SIGIL = 160, 161  # the first ids given in a fresh folder, in the order the mod asks
FIRST_MOD_KIND = 12
FIRST_MOD_FLOOR = 6

WATCHER = """
mods.on("crypt", "brute_slain", function(e) mod.log("heard slain " .. e.total) end)
function on_load() kf.play.invincible = true end
function on_use_item(e) mod.log("used " .. e.item) end
function on_drop(e) mod.log("drop " .. e.item) end
function on_map_load(e) arrived = e.floor end

local reported, test_due, test_done

-- Back on floor 1 after the crypt, a quick save and a quick load, some updates apart.
local save_after, load_after, loaded
local function save_and_load(crypt)
    if crypt.brutes_slain() == 3 and kf.player.floor == 1 and kf.context() == "gameplay" and not save_after and test_done then
        save_after = 30
    end
    if save_after and save_after > 0 then
        save_after = save_after - 1
        if save_after == 0 then
            kf.action_press("quick_save")
            load_after = 30
        end
    elseif load_after and load_after > 0 then
        load_after = load_after - 1
        if load_after == 0 then kf.action_press("quick_load") end
    end
end
function on_load_game(e) loaded = true end

function on_update()
    local crypt = mods.get("crypt")
    if not crypt then return end
    if arrived and kf.context() == "gameplay" then
        mod.log("on floor " .. arrived .. " at cell " .. kf.player.cell_x .. " " .. kf.player.cell_z)
        arrived = nil
    end
    local line = "count " .. kf.items.count(DRAUGHT) .. " sigil " .. kf.items.count(SIGIL) ..
                 " slain " .. crypt.brutes_slain() .. " burning " .. tostring(crypt.draught_burning())
    if line ~= reported then
        reported = line
        mod.log(line)
    end
    test_due = crypt.draught_burning() and not test_done
    save_and_load(crypt)
    if loaded and kf.context() == "gameplay" then
        loaded = false
        mod.log("after the load sigil " .. kf.items.count(SIGIL) .. " slain " .. crypt.brutes_slain())
    end
end

-- kf.hook sees a call from any mod, so the crypt's doubling shows on this one.
function on_between_updates()
    if test_due then
        test_due, test_done = false, true
        local before = kf.player.experience
        kf.call.player_add_experience(10)
        mod.log("ten gave " .. (kf.player.experience - before))
    end
end
""".replace("DRAUGHT", str(DRAUGHT)).replace("SIGIL", str(SIGIL))


class Game:
    def __init__(self, binary, data, home):
        self.log_path = home / "game.log"
        env = dict(os.environ, KF_SHELL=str(PORT), XDG_DATA_HOME=str(home),
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", ASAN_OPTIONS="abort_on_error=0:detect_leaks=1")
        saves = home / "saves"
        saves.mkdir()
        self.process = subprocess.Popen([binary, "--data", data, "--saves", str(saves), "--headless", "--turbo", "--skip-intro"],
                                        env=env, stdout=self.log_path.open("w"), stderr=subprocess.STDOUT)
        self.deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                self.channel = socket.create_connection(("127.0.0.1", PORT), timeout=10)
                break
            except OSError:
                self.check_time("the command channel never opened")
                time.sleep(0.2)
        self.reader = self.channel.makefile("r")

    def check_time(self, what):
        if time.monotonic() > self.deadline:
            raise RuntimeError(f"timed out: {what}")
        if self.process.poll() is not None:
            raise RuntimeError(f"the game exited early with {self.process.returncode}: {what}")

    def command(self, line):
        self.channel.sendall((line + "\n").encode())
        reply = json.loads(self.reader.readline())
        if not reply.get("ok"):
            raise RuntimeError(f"{line}: {reply}")
        return reply

    def state(self):
        return self.command("state")["state"]

    def log(self):
        return self.log_path.read_text(errors="replace")

    def wait_for(self, what, test):
        while not test():
            self.check_time(what)
            time.sleep(0.02)

    def wait_log(self, text):
        self.wait_for(text, lambda: text in self.log())

    def wait_game_ms(self, ms):
        end = self.state()["clockMs"] + ms
        self.wait_for("the game clock", lambda: self.state()["clockMs"] >= end)

    def press(self, button):
        self.command(f"press {button} 150")
        self.wait_game_ms(550)

    def use_item(self, rows_down):
        """From play, the Use list's row rows_down, whose first row is the disc's starting item."""
        self.wait_for("play", lambda: self.state().get("context") == "gameplay")
        self.press("start")
        self.press("confirm")
        for _ in range(rows_down):
            self.press("down")
        self.press("confirm")

    def brutes(self):
        return [actor for actor in self.command("actors")["actors"] if actor["definition"] >= FIRST_MOD_KIND]


def expect(condition, what):
    if not condition:
        raise RuntimeError(what)


def play(game):
    game.wait_for("gameplay", lambda: game.state().get("context") == "gameplay")
    game.wait_log("count 0 sigil 1 slain 0 burning false")

    game.use_item(1)
    game.wait_log(f"used {SIGIL}")
    game.wait_for("the crypt", lambda: game.state().get("area") == FIRST_MOD_FLOOR)
    game.wait_log("roused 3 brutes")
    game.wait_log("on floor 6 at cell 14 1")
    game.wait_for("gameplay in the crypt", lambda: game.state().get("context") == "gameplay")
    brutes = game.brutes()
    expect(len(brutes) == 3, f"expected 3 brutes, not {brutes}")

    before = game.state()["experience"]
    for brute in brutes:
        for _ in range(40):
            if f"heard slain {brutes.index(brute) + 1}" in game.log():
                break
            game.command(f"hit {brute['slot']}")
            game.wait_game_ms(300)
    game.wait_log("heard slain 3")
    game.wait_log("the crypt is quiet: 200 experience")
    game.wait_for("the reward", lambda: game.state()["experience"] >= before + 200)
    game.wait_log(f"drop {DRAUGHT}")

    # The drops lie where the brutes fell; walking to one is not what this tests.
    game.command(f"give {DRAUGHT} 1")
    game.wait_log("count 1 sigil 1 slain 3 burning false")
    game.command("hurt 5")
    game.wait_for("the hurt", lambda: game.state()["hp"] < game.state()["maxHp"])
    game.use_item(1)
    game.wait_log(f"used {DRAUGHT}")
    game.wait_log("count 0 sigil 1 slain 3 burning true")
    game.wait_for("the healing", lambda: game.state()["hp"] == game.state()["maxHp"])
    game.wait_log("ten gave 20")

    game.use_item(1)
    game.wait_for("the way back", lambda: game.state().get("area") == 1)
    game.wait_for("gameplay on floor 1", lambda: game.state().get("context") == "gameplay")
    game.wait_log("on floor 1 at cell 15 2")
    expect(not game.brutes(), "brutes followed the player to floor 1")
    game.wait_log("after the load sigil 1 slain 3")
    game.channel.sendall(b"quit\n")


def play_without_floor(game):
    game.wait_log("no crypt: run tools/mod_floor_from_disc.py")
    game.wait_log("count 0 sigil 1 slain 0 burning false")
    game.use_item(1)
    game.wait_log("roused 3 brutes")
    game.wait_for("gameplay", lambda: game.state().get("context") == "gameplay")
    expect(game.state().get("area") == 1, "the sigil left floor 1 with no crypt to go to")
    expect(len(game.brutes()) == 3, "expected 3 brutes on floor 1")
    game.channel.sendall(b"quit\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--data", default=str(ROOT / "data/en"))
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())

    run(binary, args.data, play, with_floor=True)
    run(binary, args.data, play_without_floor, with_floor=False)
    print("crypt mod: sigil, crypt, brutes, reward, draught, doubled experience, way back, save and load; no floor")


def run(binary, data, playing, with_floor):
    with tempfile.TemporaryDirectory() as temp:
        home = Path(temp)
        mods = home / "Verdite" / "Verdite1" / "mods"
        shutil.copytree(ROOT / "mods" / "crypt", mods / "crypt")
        if with_floor:
            subprocess.check_call([sys.executable, str(ROOT / "tools/mod_floor_from_disc.py"),
                                   str(Path(data) / "KF" / "B1"), str(mods / "crypt/floors/crypt")])
        manifest = json.loads((mods / "crypt/mod.json").read_text())
        manifest["default_enabled"] = True
        (mods / "crypt/mod.json").write_text(json.dumps(manifest))
        (mods / "watcher").mkdir()
        (mods / "watcher/mod.json").write_text('{"id": "watcher", "default_enabled": true}')
        (mods / "watcher/main.lua").write_text(WATCHER)

        game = Game(binary, data, home)
        problem = None
        try:
            playing(game)
        except (OSError, RuntimeError, ValueError) as error:
            problem = str(error)
        try:
            game.process.wait(timeout=TIMEOUT if problem is None else 5)
        except subprocess.TimeoutExpired:
            game.process.kill()
            game.process.wait()
            problem = problem or "the game did not exit after quit"
        log = game.log()

    bad = [row for row in log.splitlines()
           if "runtime error:" in row or "Sanitizer" in row or "] error:" in row]
    if not problem and game.process.returncode != 0:
        problem = f"the game exited with {game.process.returncode}"
    if problem or bad:
        print(log[-4000:])
        print("\n".join(bad))
        sys.exit(f"crypt mod: {problem or 'errors in the log'}")


if __name__ == "__main__":
    main()
