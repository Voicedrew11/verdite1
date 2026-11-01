# The raw layer (modding API step 5): handoff

Written 2026-11-01, after API 0.8. Step 5 of [MODDING.md](MODDING.md#order):
every named global and struct field in the decomp, reachable from Lua, typed
and bounds-checked, generated from the decomp's headers so it follows the pin.
Built 2026-11-01 (API 0.9): steps 2 to 7 below are done; the fuzzer's status
is under step 5. This page was the handoff and is now the record.
Steps 8 to 10 (calling the game's functions, hooking them, growing the tables)
come after it and reuse its type descriptions and range checks.

## Decided

- **Everything, read and write; the line is memory safety** (2026-11-01). The
  user wants a mod able to change basically anything, not only adjust numbers.
  So nothing is held back for consistency: a write the game overwrites, or one
  that leaves the automap out of step with a changed floor, is the mod's bug,
  documented in the definitions file, not blocked. What is held back is any
  way for a Lua value to reach the game's memory unchecked, since mods travel
  as zips from strangers and corrupted memory is how one escapes the sandbox.
- **Generated with clang, with a short listed file beside it** (2026-11-01).
  clang's JSON AST (`clang++ -std=c++20 -Xclang -ast-dump=json -fsyntax-only`)
  gives the declarations; no Python libclang package is needed (it is not in
  the shell; `clang` is). The listed file holds value ranges and aliases, not a
  list of what is exposed. A fully listed subset was weighed and dropped: it
  brings back the curated layer's wait for a binding, which step 5 removes.
- **No pointers.** Values are copied in and out of a named field, checked
  against its type, its range and an array's bounds
  ([MODDING.md](MODDING.md#what-a-mod-cannot-do)).
- **Unstable by promise.** The raw names are the decomp's and follow its
  renames; semver covers the curated layer only ([MODDING.md](MODDING.md#stability)).
- **Off in coop**, as every mod is (decided 2026-10-31).
- **It hangs at `kf.raw.<global>`** (2026-11-01), not `kf.game`, which is the
  curated table of the calling mod's `speed`, `paused`, `music_while_paused`
  and `mouse` (`game_fields()` in `port/ext/mods.cpp`); a global named like one
  of those would collide, and `kf.raw` says at the call site that the name is
  unstable.
- **Proxies** (2026-11-01): `kf.raw.player_state.camera_position.vx` reads and
  writes one field. Each step is a userdata holding a path, which it resolves
  again from the global on every access; it never holds an address, so it can
  never dangle. `kf.raw.copy(proxy)` gives a whole struct or array as a Lua table.
- **Raw writes and `kf.rules` are left as they fall** (2026-11-01). Both write
  the monster definitions, level growth, prices and equipment records;
  `kf.rules` lays its values again at the next map load, over a raw write.
  The definitions file says so.

## What is held back, and why

Only what would let Lua reach memory:

- **Raw pointers**: never a value in Lua. About 30 pointer members (such as
  `KfArmorRecord *equipped_head_armor_record`) and two pointer globals
  (`render_precise_map`, `map_event_in_conversation`). A mod reaches what a
  pointer points to through the global that owns it (`armor_records`), not
  through the pointer. Later, a pointer whose target is a known array could
  read as that array's index; that is an addition, not a hole.
- **The allocators' internals**: `memory_arena`, `cutscene_memory_arena`,
  `menu_item_model_allocation`. Their fields are offsets and sizes into the
  game's own memory; a write is a buffer overrun.
- **Unions** (2): a field read through the wrong member reinterprets memory.
  Each can come back as one named member when someone checks which one the
  game uses.

Everything else is readable and writable: the floor's grids, lighting and
colour matrices, the HUD and sprite tables, sound state, file paths, the
snapshots, the opening and cutscenes, the coop bookkeeping, the rules' tables,
`unknown_*` fields (58, under their decomp names until the listed file aliases
them) and `DAT_80057d24`.

## Ranges: the real work

"Writable" means writable within what the game survives. Many plain integers
are used by the game as array indices or sizes without a check: an equipped
armour id of 200 has the game read `armor_records[200]`. So every field gets a
range, from three places:

1. **The type.** An integer's width and sign, an enum's values (the generator
   reads the enum's declaration; `kf_enum_encode` converts), an array's length.
   A fixed `std::array<char, N>` path takes a string of fewer than N bytes.
2. **The listed file.** A plain integer the game uses as an index or a count
   gets the range the game survives (`player_state.equipped_head_armor_id`:
   the armour ids). Found by reading the decomp where the field is used, and
   by the fuzzer, below.
3. **The fuzzer.** A new mode of `verdite/fuzz.h` (see FUZZING_PLAN.md) that
   writes random in-range values into random raw fields while it plays, under
   ASan and UBSan. A crash names a field whose range is too wide; its range
   goes in the listed file, and the run repeats until clean. The raw layer
   does not ship before a clean run.

A field that cannot be given a safe range (one the game indexes with no single
array behind it) is read-only, with a line in the listed file saying why.
That is the only way a field becomes read-only. The fuzzer found three kinds
(2026-11-01): a bound on a product or a sentinel ("not the last row", a cell
window's width times height), a value the game keeps beside a pointer it sets
(an equipped id and its record, the animation cache's owner), and a sub-field of
a shared type (a position's x and z, a `VECTOR` like every other). One interval
per field says none of these; a richer range would give some of them back.

## What the headers hold

Measured on the patched headers, `build/linux/decomp_src/include/kf/`
(`audio`, `cutscene`, `game`, `lib`, `platform`, `renderer`):

- 85 headers; about 83 single-line data globals (`extern KfPlayerState
  player_state;`, `extern std::array<KfPlayerLevelGrowth, KF_PLAYER_LEVEL_GROWTH_COUNT>
  player_level_growth_table;`, `item_stock`, `map_object_state`,
  `map_runtime_state`). Multi-line declarations may add a few.
- About 170 structs, C++ (`std::array` members, `typedef struct` style).
- Field kinds: integers of each width, enums (`KfObjectId`, `KfPlayerOption`),
  nested structs, `std::array` of integers or structs, the PS1 SDK's
  `SVECTOR`, `VECTOR`, `MATRIX`, `CVECTOR`, and `SoundRef`.
- The generator reports every declaration it does not expose, and why.
  Globals defined only in a `.cpp` (static or in an anonymous namespace) are
  in no header, so it never sees them; it counts them from the sources, so
  that gap is known, and a patch can move one into a header when a mod needs it.

Read the **patched** copy, not `decomp/`: the patches in `patches/` change
headers, and the build compiles `decomp_src/`. The flags clang needs are in
`build/linux/compile_commands.json`.

## Where the code goes

Following [MODDING.md](MODDING.md#where-the-code-goes):

- **libverdite**: the game-agnostic part, so KF2 and KF3 get a raw layer by
  generating their own tables. A type description (integer of a width and
  signedness with a range, enum, struct of fields, fixed array of a type,
  fixed string), a field's get and set through generated functions, the proxy
  machinery and `kf.raw.copy`, the checks, and the raw part of the definitions file. Its
  own commits, with unit tests over a hand-written description (no game).
  Steps 8 and 9 check function arguments with the same descriptions.
  Today's `ModTable`/`ModField` (`libverdite/include/verdite/mods.h`) is flat,
  one path per table with get/set per field; arrays of structs indexed at run
  time do not fit it, so this is a new shape beside it, not more `ModTable`s.
- **Generator**: `tools/raw_layer.py` (beside `tools/patches.py`). Runs clang
  on the patched headers, reads the listed file, writes a C++ source of
  tables into the build folder. A CMake custom command that depends on the
  patched headers and the listed file, so a pin move or a patch regenerates it.
- **Generated C++**: names fields by member pointer or by a lambda over the
  global (`&KfPlayerState::foot_height`, as `read_only_integer` in
  `port/ext/mods.cpp` does), never by offset, so the compiler checks every
  name and type and a generator bug is a compile error, not a wrong read.
- **The listed file**: `port/ext/raw_layer.toml` or similar: ranges for index
  and count fields, the few read-only fields with their reasons, aliases for
  `unknown_*` names, and which member of each union is exposed.
- **KF1's binding**: one entry in `port/ext/mods.cpp`'s tables that hangs the
  generated tables at the chosen path.

## Order

1. ~~Ask the three questions.~~ Answered 2026-11-01 (above).
2. Write the libverdite header (the type description and the Lua surface) and
   get it agreed before the implementation; that is the global rule for a
   module bigger than a function or two. Any change found while implementing,
   including by a subagent, goes back to the user, each change named.
3. libverdite implementation and its tests, in its own commit.
4. The generator, its listed file, the CMake step; the generated source
   compiles with no hand edits. Report what it left out and why.
5. The fuzzer's raw-write mode, and runs under ASan and UBSan until clean,
   each found range in the listed file. Mode built 2026-11-01 (`KF_FUZZ_RAW`,
   `fuzz.py --raw`); **not clean**. 25 entries came from 10 rounds of 16
   runs, but the first rounds were shallow: a `game_result` write ends the
   loop, so most runs stalled within a few hundred updates. With the chooser
   skipping it (seeds 201-216, about 1500 writes a run), 11 of 16 runs crashed
   or failed: the notification queue's tail, a map object's action, the copy
   regions, the collision grid, a fog value used as a divisor, and others.
   The game indexes much of its own working state with no check, so a field
   at a time will not converge soon.
6. KF1's binding, the definitions file, MODDING.md's reference section, the
   API version, step 5 struck in the order.
7. Measure: a raw read and write cost per access (a loop of 100 000 in a
   probe mod, as the map mod's cost was measured), against the 100 million
   instruction budget.

## Checks

From AGENTS.md and the global rules; a subagent's brief names these:

- Write C++ by `docs/cpp-reference.md` (its "Before finishing" list) and
  `docs/code-style.md`. libverdite has no `kf/` or decomp include.
- Never edit `decomp/` or `decomp_src/`; the generator only reads.
- Unit tests for the libverdite part (bounds, ranges, enum encoding, string
  lengths, a path that stops resolving) in the same commit.
- `scripts/check.sh`, with the sanitizer build: raw access is memory access,
  so ASan and UBSan must run on a probe mod that reads and writes every kind
  of field, an array's last element, and one past it (an error, not a read).
- The fuzzer's raw-write mode clean under ASan and UBSan (above).
- Offscreen run with a probe mod, as the 0.7 and 0.8 commits did: a raw write
  the game then shows (HP through `player_state` against `kf.player.hp`), and
  an error for each bad path, index, type and range.
- Commit messages state the finding, in the imperative, with the consequence
  and what was and was not checked.
