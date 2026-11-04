#!/usr/bin/env python3
"""Reading and rating a message on the ground, with two headless instances on a local service
(online/README.md: npm run dev, and the D1 migrations once): the prompt, the box and its choices,
holding the player, closing without rating, rating, the count and the author's notice, a rating
kept over a restart, the player's own, several in a cell, and a door taking the button from a
message before it. docs/MESSAGES_PLAN.md, "Reading and rating".

    scripts/messages_flow.py build/linux/verdite1 [--service URL] [--work DIR]
"""
import argparse
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import coop_pair  # noqa: E402
from coop_pair import Game  # noqa: E402

PORTS = (27951, 27952)  # not the game's 27901, smoke.py's 27911 or the other pairs'
SAFE_HERE, TURN_BACK = 15, 14  # templates with no blank


def messages(game):
    return game.run("messages")


def wait_for(game, what, test, seconds=30):
    deadline = time.monotonic() + seconds
    while True:
        state = messages(game)
        if test(state):
            return state
        if time.monotonic() > deadline:
            raise RuntimeError(f"{game.name}: {what} never came: {state}")
        time.sleep(0.1)


def press(game, button):
    game.run(f"press {button}")
    time.sleep(0.4)


def open_game(name, port):
    game = Game(name, port, room=None)
    game.run("option online.messages true")
    wait_for(game, "a floor", lambda s: s["floor"] > 0, 60)
    return game


def find_world_target(game):
    """A cell where something before the player takes the action button, or None."""
    for x in range(100):
        for z in range(100):
            if not game.run(f"move {x} {z}").get("ok"):
                continue
            time.sleep(0.03)
            if messages(game)["front"] == "world":
                return x, z
    return None


def main():
    checks = []

    def check(what, ok, detail=None):
        checks.append((what, ok))
        print(("PASS " if ok else "FAIL ") + what + (f": {detail}" if detail is not None and not ok else ""))

    # Assumes a service with no earlier run's messages on the floor's first cell.
    author = open_game("author", PORTS[0])
    author.run(f"leave {SAFE_HERE}")
    wait_for(author, "the author's message", lambda s: any(m["kind"] == "mine" for m in s["marks"]))
    check("the author's message is marked as its own", True)
    box = wait_for(author, "the author's prompt", lambda s: s["prompt"] == "READ MESSAGE")
    check("the prompt shows on the author's own message", box["prompt"] == "READ MESSAGE")
    press(author, "confirm")
    box = messages(author)["box"]
    check("the author's box says it is theirs and offers no Good",
          box is not None and "YOUR MESSAGE" in box["status"] and "GOOD" not in box["choices"], box)
    press(author, "confirm")
    check("Leave closes the author's box", messages(author)["box"] is None)

    reader = open_game("reader", PORTS[1])
    wait_for(reader, "the reader's mark", lambda s: any(m["kind"] == "unread" for m in s["marks"]), 40)
    check("the reader sees an unread mark", True)
    wait_for(reader, "the prompt", lambda s: s["prompt"] == "READ MESSAGE")
    check("the reader gets the prompt", True)

    press(reader, "confirm")
    state = messages(reader)
    box = state["box"]
    check("the first press opens the box on Leave, with no count yet",
          box is not None and box["cursor"] == 0 and box["choices"][:2] == ["LEAVE", "GOOD"] and box["status"] == "", box)
    check("the prompt is gone while the box is open", state["prompt"] == "", state["prompt"])

    before = reader.run("state")["state"]
    reader.run("press up 400")
    time.sleep(0.6)
    after = reader.run("state")["state"]
    check("the player stands still while the box is open", before["pos"] == after["pos"], (before, after))
    check("Up moved the cursor instead", messages(reader)["box"]["cursor"] == 1)
    press(reader, "left")
    check("and Left moved it back", messages(reader)["box"]["cursor"] == 0)
    before = reader.run("state")["state"]
    press(reader, "right")
    check("Right moved it on, without turning the player",
          messages(reader)["box"]["cursor"] == 1 and before["yaw"] == reader.run("state")["state"]["yaw"])
    press(reader, "left")

    press(reader, "back")
    state = messages(reader)
    check("Back closes without rating", state["box"] is None and not any(m["rated"] for m in state["marks"]), state)
    check("and no menu opened", reader.run("state")["state"]["context"] != "menu")
    check("the opened mark is read", any(m["kind"] == "read" for m in state["marks"]), state["marks"])

    press(reader, "confirm")
    press(reader, "confirm")
    state = messages(reader)
    check("two presses of the button rate nothing", state["box"] is None and not any(m["rated"] for m in state["marks"]), state)

    press(reader, "confirm")
    press(reader, "down")
    press(reader, "confirm")
    box = messages(reader)["box"]
    check("Good stays open, says the reader found it good and has only Leave",
          box is not None and box["status"] == "YOU FOUND IT GOOD" and box["choices"][0] == "LEAVE" and "GOOD" not in box["choices"], box)
    author_state = wait_for(author, "the author's notice", lambda s: s["notices"], 20)
    check("the author is told", author_state["notices"][0].startswith("YOUR MESSAGE RATED GOOD"), author_state["notices"])
    check("the author's count rose", any(good >= 1 for good in author_state["mine"]), author_state["mine"])
    wait_for(author, "the count on the author's own mark", lambda s: any(m["good"] >= 1 for m in s["marks"]), 20)
    check("and so did the one its box shows", True)
    press(reader, "confirm")
    check("Leave closes it", messages(reader)["box"] is None)

    reader.quit()
    reader = open_game("reader", PORTS[1])
    wait_for(reader, "the rating after a restart", lambda s: any(m["rated"] for m in s["marks"]), 40)
    check("a rating is remembered over a restart", True)
    wait_for(reader, "the prompt again", lambda s: s["prompt"] == "READ MESSAGE")
    press(reader, "confirm")
    box = messages(reader)["box"]
    check("and Good is not offered again", box is not None and "GOOD" not in box["choices"], box)
    press(reader, "back")

    author.run(f"leave {TURN_BACK}")
    wait_for(author, "the second message", lambda s: sum(m["kind"] == "mine" for m in s["marks"]) >= 2)
    press(author, "confirm")
    box = messages(author)["box"]
    check("two in a cell show which of them this is and offer Next",
          box is not None and " OF " in box["status"] and "NEXT" in box["choices"], box)
    first = box["status"]
    press(author, "down")
    press(author, "confirm")
    box = messages(author)["box"]
    check("Next turns to the other", box is not None and box["status"] != first, box)
    press(author, "back")

    target = find_world_target(author)
    check("a cell with something to act on was found", target is not None)
    if target:
        author.run(f"leave {SAFE_HERE}")
        wait_for(author, "the message before it", lambda s: sum(m["kind"] == "mine" for m in s["marks"]) >= 3)
        check("no prompt before it", messages(author)["prompt"] == "")
        press(author, "confirm")
        check("the button stays the game's: no box opens", messages(author)["box"] is None)

    if messages(reader)["box"] is None:
        wait_for(reader, "the prompt once more", lambda s: s["prompt"] == "READ MESSAGE")
        press(reader, "confirm")
        reader.run("hurt 9999")
        wait_for(reader, "the fall closing the box", lambda s: s["box"] is None, 10)
        check("falling closes the box", True)

    author.quit()
    reader.quit()
    failed = [what for what, ok in checks if not ok]
    print(f"{len(checks) - len(failed)} of {len(checks)}")
    return 1 if failed else 0


def parse():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--service", default="http://127.0.0.1:8787")
    parser.add_argument("--work", help="a new folder for the instances' files and logs, kept")
    args = parser.parse_args()
    coop_pair.BINARY, coop_pair.SERVICE = args.binary, args.service
    return args.work


if __name__ == "__main__":
    kept = parse()
    with tempfile.TemporaryDirectory() as work:
        coop_pair.WORK = Path(kept or work)
        try:
            sys.exit(main())
        finally:
            coop_pair.kill_all()
