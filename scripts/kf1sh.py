#!/usr/bin/env python3
"""Send one command to a running game's command channel and print the reply.

The game must run with KF1_SHELL=1 (or a port). See "Driving the game without a
person" in docs/DEVELOPMENT.md for the verbs.

    python3 scripts/kf1sh.py state
    python3 scripts/kf1sh.py warp 4 79000 71000
    python3 scripts/kf1sh.py --port 27911 poke 800A0790 e703e703
    python3 scripts/kf1sh.py spot area4-busy        # a named place, below
    python3 scripts/kf1sh.py spots                  # list the named places
"""
import json
import socket
import sys

# Places the Step 8 tour measured, by name: the warp line that reaches each.
# docs/MANUAL_PASS.md says what to look at in each.
SPOTS = {
    "area1-start": "warp 1 31000 4000",
    "area1-door": "warp 1 46000 68500 0",
    "area1-save": "warp 1 52850 73500 0",
    "area1-sign": "warp 1 27000 19700 0",
    "area1-exit": "warp 1 51000 25000 0",
    "area2": "warp 2 57000 37000",
    "area2-raised": "warp 2 61000 113000",
    "area3": "warp 3 51000 23000",
    "area4-busy": "warp 4 79000 71000",
    "area4-west": "warp 4 13000 45000",
    "area4-creature": "warp 4 17000 72000",
    "area5": "warp 5 79000 139000 - 1",
}


def send(port, line):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    return buf.decode().strip()


def main():
    args = sys.argv[1:]
    port = 27901
    if args[:1] == ["--port"]:
        port, args = int(args[1]), args[2:]
    if not args:
        sys.exit(__doc__)
    if args == ["spots"]:
        for name, line in SPOTS.items():
            print(f"{name:16} {line}")
        return
    if args[0] == "spot":
        line = SPOTS.get(args[1]) if len(args) > 1 else None
        if line is None:
            sys.exit(f"unknown spot; one of: {', '.join(SPOTS)}")
    else:
        line = " ".join(args)
    reply = send(port, line)
    try:
        print(json.dumps(json.loads(reply), indent=1))
    except ValueError:
        print(reply)


if __name__ == "__main__":
    main()
