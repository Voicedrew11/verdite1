# libverdite

The game-agnostic C++ library that the decomp-based Verdite ports will share,
the successor to the C# Verdite Core. It grows here, inside Verdite1, and moves
to its own subtree once a second game uses it (see `docs/verdite-port.md`, "libverdite: the C++ core"). The C# Verdite Core
(`Voicedrew11/verdite-core`) stays with the recompiled builds.

**Nothing here knows a game.** No `kf/...` include, no decomp header, no game
address, env var prefix or settings key: the game hands those in. A change
here is its own commit.

| file | what |
|---|---|
| `include/verdite/agent.h`, `src/agent.cpp` | the state beacon (a JSON line a second on stdout) and the command channel (TCP on 127.0.0.1, one request per line, one JSON line back, run on the game thread) |

Built as the `verdite` CMake target (`libverdite.a`); it needs only the C++
standard library and the platform's sockets.
