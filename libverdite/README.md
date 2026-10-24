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
| `include/verdite/crash.h`, `src/crash.cpp`, `src/crash_helper.cpp` | crash reports: on a fatal signal, an unhandled Windows exception or `std::terminate`, a report with the stack named to functions and lines (captured raw in the handler, named by the same binary started again as a helper, with cpptrace), the game's sections, the last events and copies of its files; soft faults the game survives (`docs/CRASH_REPORTS.md`) |
| `include/verdite/disc_image.h`, `src/disc_image.cpp` | a CD image's data track as flat bytes, stored plain (`.bin`, `.iso`) or as a CHD (MAME's compressed format, through libchdr in `third_party/libchdr`); a CHD of a raw track reads back byte for byte as its `.bin`. Test images from `tools/make_disc_fixtures.py` in `tests/data/disc/` |
| `include/verdite/discord.h`, `src/discord.cpp` | Discord Rich Presence over the Discord app's local IPC (Unix socket or named pipe), no SDK: a thread of its own connects, retries and sends the latest activity, paced so a counting value does not flood Discord |
| `include/verdite/memory_card.h`, `src/memory_card.cpp` | PlayStation memory card images (the raw 128 KB `.mcd`/`.mcr` dump, the C# builds' `carda.sav`): the files on a card by name, read along their block chains, and a formatted card written from files |
| `include/verdite/places.h`, `src/places.cpp` | place names over a floor's grid of cells, spread from named seed cells through walkable cells |

Tests are in `tests/` (doctest), built as `verdite_tests` when doctest is installed (the nix
shell has it) and run with `ctest --preset linux`. New logic here comes with tests.

Built as the `verdite` CMake target (`libverdite.a`); it needs the C++ standard library, the
platform's sockets (and, for Discord, its named pipes), cpptrace (crash reports) and libchdr (CHD
images).
