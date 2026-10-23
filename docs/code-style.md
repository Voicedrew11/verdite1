# Code style

How Verdite's own C++ is written: `libverdite/` and `port/`. The patches to the decomp's game
code follow the decomp instead (last section). The checks that hold the mechanical part are in
`scripts/check.sh`; the rest is judgement, and this is what to judge by. The language rules (ownership, lifetimes, casts, UB) are in
[cpp-reference.md](cpp-reference.md).

## Models

Write like these. When this page and a model disagree, the model is probably right and this page
is out of date.

- In this repo: `libverdite/include/verdite/agent.h` and `src/agent.cpp`. The header is the whole
  interface and reads as documentation; the state is one struct in an anonymous namespace; the
  game hands in what it knows, so the library knows no game.
- **Lua** (5.4 source): small files, one idea each, a C core extended through a narrow API.
- **sokol_gfx.h** (floooh/sokol): one rendering interface over several backends, plain structs in,
  handles out. The model for the renderer frontend and its backends.
- **Dolphin** `Source/Core/VideoCommon` and `VideoBackends/`: the same split in an emulator's C++.
- *A Philosophy of Software Design* (Ousterhout): deep modules with simple interfaces, complexity
  pulled down into the implementation rather than pushed onto callers.

## Modules

- **The header first.** A module's header says what it does, in the reader's terms, with nothing
  the caller does not need. For anything bigger than a function or two, the header is written and
  agreed before the implementation.
- **Deep, not wide.** Few functions that each do a lot beat many that each do a little. If every
  caller has to call three functions in the right order, that order belongs inside the module.
- **State is private.** File-local state lives in an anonymous namespace in the `.cpp`, usually
  one struct; headers expose functions and plain data, not globals.
- **Dependencies point one way.** `libverdite/` knows no game. `port/` knows the game and the
  platform. Game code knows `port/` only through `kf/platform` and `kf/renderer` headers.

## Extending

- **Add by adding.** A new command is an entry in the command list; a new renderer backend is a
  new implementation of the backend interface; a new setting is a new row. If adding a case means
  editing a `switch` in a caller, the interface is missing something.
- **No backend or platform names in callers.** `if (vulkan)` belongs in exactly one place.
- **Data before code.** Tables (place names, button names, check lists) are data at the top of a
  file or in their own file, not branches.

## Functions

- **One job, one screen.** If it needs a comment to split it into parts, split it into functions.
  clang-tidy's cognitive complexity limit (30) is the hard ceiling, not the target.
- **Early returns** for the cases that are not the main path; the main path stays unindented.
- **No flag parameters.** `draw(true, false)` says nothing; use two functions, an enum, or a
  struct with named fields.
- **Return values,** not out-parameters. A failure the caller handles is `bool`, `std::optional`,
  or a small result struct; exceptions do not cross into game code.

## Names

- `snake_case` for functions, variables and constants, `CamelCase` for types, as the code has.
- Names in the game's or the domain's terms: `player_hp`, `cell_window`, `beacon_period`.
  Not `data`, `info`, `value`, `tmp`, and no `Manager`, `Helper`, `Util`, `Handler` types.
- Units in the name when there is a unit: `hold_ms`, `until_ns`, `width_px`.

## Data and memory

- RAII and value types. Containers and `std::unique_ptr` own; raw pointers and references only
  borrow. No owning `new`/`delete`, no `malloc`/`free` in C++.
- `std::span`, `std::string_view`, `const&` at interfaces. `std::array` over C arrays in new code.
- Anything from outside the program (network, files, saves, mods, the disc) is bounds-checked
  where it enters, once, so the code behind it can trust it.

## Threads

The game thread is the default and needs no comment. Anything else says so at the function
(`// On a client's thread: ...`). Data crosses threads through a queue or under a mutex, as the
agent channel hands commands to the game thread; never "it's fine, it's only a flag".

## Comments

Light: a comment says why, or what is not obvious from the code, in a line or two. Reasoning,
history and measurements go in `docs/`. A header comment above each public function is the
exception: it is the documentation.

## Game-code patches (`work/`)

The decomp's style, not this page's: its naming, its `KF_` constants, its C-shaped functions.
A patch changes as few lines as it can, keeps the original lines where it can, and marks what it
adds with a `// PC extension: ...` comment saying what and why, as the existing patches do. Only
the bug checks run on patch lines.

## The checks

`scripts/check.sh` before every commit (`--quick` skips the sanitizer build):

- build with the project's warnings;
- `libverdite/` tests (doctest, `libverdite/tests/`); new logic in `libverdite/` comes with tests;
- `scripts/lint.py`: clang-tidy on all of `libverdite/`, on the changed lines of `port/` (it is
  carried code: old lines are cleaned up on purpose, not as a side effect), and on the lines the
  patches add, with only the bug checks;
- `scripts/smoke.py`: boot headless at turbo, walk, quit, plain and under ASan and UBSan.

Checks turned off in `.clang-tidy`, and why:

| check | why off |
|---|---|
| `bugprone-easily-swappable-parameters` | flags every `(int x, int y)` |
| `bugprone-narrowing-conversions` | the game's math narrows on purpose (`s32` to `s16` and so on) |
| `performance-enum-size` | enums match the game's sizes |
| game code also: `bugprone-implicit-widening-of-multiplication-result`, `bugprone-reserved-identifier` | the decomp's arithmetic and names |
