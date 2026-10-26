#!/usr/bin/env python3
"""Boot the game headless at turbo speed, walk the first-run setup, walk a little and try every setting through the command channel, quit.

Fails if the game does not start, a command fails, the player does not move, a setting does not
take each of its values, the game does not exit cleanly, or a sanitizer reports anything. The
settings, saves and mods are a fresh temporary folder's, never the user's. Usage: scripts/smoke.py BINARY [--data data/en]
[--renderer vulkan]

With --renderer vulkan it draws for real, on SDL's offscreen video driver (no window) with dummy
audio and the validation layers on, and also fails on a validation error or a renderer error.
It loads the host's Vulkan loader and drivers from --host-libraries, as scripts/run-linux.sh does.
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
PORT = 27911  # not the game's 27901, so a running game does not get in the way
TIMEOUT = 60


def command(channel, reader, line):
    channel.sendall((line + "\n").encode())
    line_back = reader.readline()
    if not line_back:
        raise RuntimeError(f"{line}: the game closed the channel")
    reply = json.loads(line_back)
    if not reply.get("ok"):
        raise RuntimeError(f"{line}: {reply}")
    return reply


def state(channel, reader):
    return command(channel, reader, "state")["state"]


def option_rows(channel, reader):
    pages = command(channel, reader, "options")["options"]["pages"]
    return {row["id"]: row for page in pages for section in page["sections"] for row in section["rows"]}


def text_of(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def values_to_try(row):
    if row["kind"] == "toggle":
        return [not row["value"], row["value"]]
    if row["kind"] == "choice":
        return [choice["value"] for choice in row["choices"]] + [row["value"]]
    if row["kind"] == "number":
        return [row["min"], row["max"], row["value"]]
    return []


def try_options(channel, reader):
    """Sets every enabled setting to each of its values and back. Actions are not pressed (one downloads)."""
    rows = option_rows(channel, reader)
    for row in rows.values():
        if row["kind"] == "choice" and row["value"] not in [choice["value"] for choice in row["choices"]]:
            return f"{row['id']} is {row['value']!r}, none of its choices"
        if row["kind"] == "number" and not row["min"] <= row["value"] <= row["max"]:
            return f"{row['id']} is {row['value']}, outside {row['min']}..{row['max']}"
    for id, row in rows.items():
        if not row["enabled"]:
            continue
        for value in values_to_try(row):
            command(channel, reader, f"option {id} {text_of(value)}")
            now = option_rows(channel, reader).get(id)
            if now and now["value"] != value:
                return f"{id} set to {value!r} reads back {now['value']!r}"
    return None


def walk_setup(channel, reader):
    """Walks the first-run setup with the Modern preset to its summary and finishes. A drawn run
    starts with it open (a new user folder); a headless one is started here."""
    if command(channel, reader, "setup")["setup"] is None:
        command(channel, reader, "setup start")
    view = command(channel, reader, "setup preset modern")["setup"]
    while view["step"] == "page":
        view = command(channel, reader, "setup next")["setup"]
    if view["step"] != "summary":
        return f"setup ended on {view['step']}, not the summary"
    if command(channel, reader, "setup finish")["setup"] is not None:
        return "setup still open after finish"
    return None


def drive():
    """Returns a description of what went wrong, or None."""
    deadline = time.monotonic() + TIMEOUT
    while True:
        try:
            channel = socket.create_connection(("127.0.0.1", PORT), timeout=10)
            break
        except OSError:
            if time.monotonic() > deadline:
                return "the command channel never opened"
            time.sleep(0.2)
    with channel, channel.makefile("r") as reader:
        if problem := walk_setup(channel, reader):
            return problem
        # The arrival is scripted for 54 updates; walking waits for the player to have control.
        while state(channel, reader).get("context") != "gameplay":
            if time.monotonic() > deadline:
                return "the game never reached gameplay"
            time.sleep(0.2)
        start = state(channel, reader)
        command(channel, reader, "press up 3000")
        while (now := state(channel, reader))["clockMs"] < start["clockMs"] + 3500:
            if time.monotonic() > deadline:
                return "the game clock stopped"
            time.sleep(0.1)
        before, after = start["pos"], now["pos"]
        if after == before:
            return f"the player did not move from {before}"
        if problem := try_options(channel, reader):
            return problem
        channel.sendall(b"quit\n")
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--data", default=str(ROOT / "data/en"))
    parser.add_argument("--renderer", choices=["headless", "vulkan"], default="headless")
    parser.add_argument("--host-libraries", default="/usr/lib64")
    args = parser.parse_args()

    env = dict(os.environ, KF_SHELL=str(PORT),
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               ASAN_OPTIONS="abort_on_error=0:detect_leaks=1")
    mode = ["--headless"]
    if args.renderer == "vulkan":
        mode = []
        # The host drivers' own allocations are reported from unloaded modules (code-style.md).
        env.update(SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy",
                   KF_VK_VALIDATION="1", KF_TEST_NO_PAUSE="1", LD_LIBRARY_PATH=args.host_libraries,
                   VK_LOADER_LAYERS_DISABLE="~implicit~", ASAN_OPTIONS="abort_on_error=0:detect_leaks=0")
    with tempfile.TemporaryDirectory() as saves:
        env["XDG_DATA_HOME"] = saves   # SDL's pref path: the user folder, with settings.json and mods
        game = subprocess.Popen(
            [args.binary, "--data", args.data, "--saves", saves, *mode, "--turbo", "--skip-intro"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            problem = drive()
        except (OSError, RuntimeError, ValueError) as error:
            problem = str(error)
        try:
            output, _ = game.communicate(timeout=TIMEOUT if problem is None else 5)
        except subprocess.TimeoutExpired:
            game.kill()
            output, _ = game.communicate()
            problem = problem or "the game did not exit after quit"

    sanitizer = [row for row in output.splitlines()
                 if "runtime error:" in row or "Sanitizer" in row]
    if problem is None and game.returncode != 0:
        problem = f"the game exited with {game.returncode}"
    if problem is None and sanitizer:
        problem = "a sanitizer reported:\n" + "\n".join(sanitizer)
    vulkan = [row for row in output.splitlines() if "[vulkan] error" in row or row.startswith("[renderer]")]
    if problem is None and vulkan:
        problem = "Vulkan reported:\n" + "\n".join(vulkan)
    if problem:
        print(output[-4000:])
        print(f"smoke: FAILED: {problem}")
        return 1
    print(f"smoke: booted, walked, quit ({args.renderer})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
