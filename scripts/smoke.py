#!/usr/bin/env python3
"""Boot the game headless at turbo speed, walk a little through the command channel, quit.

Fails if the game does not start, a command fails, the player does not move, the game does not
exit cleanly, or a sanitizer reports anything. Usage: scripts/smoke.py BINARY [--data data/en]
[--renderer vulkan]

With --renderer vulkan it draws for real, on SDL's offscreen video driver (no window) with dummy
audio and the validation layers on, and also fails on a validation error or a fall back to GL.
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
        while not state(channel, reader).get("inGame"):
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
        env.update(SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy", KF_RENDERER="vulkan",
                   KF_VK_VALIDATION="1", KF_TEST_NO_PAUSE="1", LD_LIBRARY_PATH=args.host_libraries,
                   VK_LOADER_LAYERS_DISABLE="~implicit~", ASAN_OPTIONS="abort_on_error=0:detect_leaks=0")
    with tempfile.TemporaryDirectory() as saves:
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
