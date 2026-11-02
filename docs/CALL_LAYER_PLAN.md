# The call layer (modding API step 8): plan

Written 2026-11-01, after API 0.12. Step 8 of [MODDING.md](MODDING.md#order): a mod calls the
game's functions, through bindings generated from the same headers as the
[raw layer](RAW_LAYER_PLAN.md), each argument checked against its type and range. The host's two
bugs are fixed (step 2); the layer itself is not built yet.

## What makes this harder than the raw layer

A raw write changes one value. A call runs game code, and game code assumes things that no
argument says. The line is the raw layer's, memory safety: a call that leaves the game in an odd
state is the mod's bug, and a call that can corrupt memory is ours. Checking arguments is not
enough to hold that line, for these reasons, all found by reading the code:

- **`on_update` is not a quiet point.** It runs from `host_present_faces`
  (`port/platform/host.cpp`), so it runs inside whatever loop is presenting a frame: a menu, a
  warp's shimmer, a map script. A call from there can pull a pool out from under a loop that is
  walking it. `host_between_updates` (`src/game/game.cpp`, in the world loop) is the one place no
  menu or script holds the game, which is why `kf.warp` and equipping already queue for it.
- **Some functions run frames of their own.** A shop, dialogue, a warp or a cutscene presents
  frames, so called from `on_update` it runs `on_update` again, with no end.
- **Some functions read a "current" object.** `actor_apply_horizontal_movement` starts by reading
  `actor_state.current`, and `actor_pool_update` sets that to null when it finishes. Its signature
  is `void ()`, so nothing a mod passes can make the call safe. Effects and map events have the
  same pattern (`effect_pool_set_current`, `map_event_set_current`).
- **Events run inline.** `on_damage` fires inside the damage code, which may be inside the actor
  loop. A handler that deals damage recurses, and one that clears the pool breaks the loop.
- **A nested failure freed the state in use** (fixed, step 2). `fail` in `libverdite/src/mods.cpp`
  closed the mod's Lua state. If `kf.call.x` raised an event and that mod's handler erred, the state
  was closed while the outer `kf.call.x` was still running on it: a use after free, which ASan
  confirmed in a test before the fix.
- **A nested call refilled the budget** (fixed, step 2). `run_protected` gave each call a fresh
  instruction budget, so a loop whose calls raised events never ran out; the test hung before the
  fix. A native call also costs one instruction, however long it takes, so each `kf.call` is charged.
- **A coroutine cannot make a call that raises an event.** The host runs a handler on the mod's main
  thread, which a running coroutine has suspended beneath it. Calls that change state are refused
  inside a coroutine; `any_time` calls raise nothing, so they are not.
- **Some functions reach past the sandbox.** `save_system_write_slot` overwrites the player's memory
  card, though a mod may write only inside its own folder. `game_shutdown` and `sound_shutdown` end
  things a mod has no business ending.

## What the headers hold

Measured 2026-11-01 on `build/linux/decomp_src/include/kf` with clang's AST: **558** functions,
of which 9 are operators on `KfEffectType` and are left out. The other 549 by signature:

| signature | functions | how it crosses |
|---|---:|---|
| plain: integers, enums, bools, floats, small structs by value | 272 | as the raw layer's values |
| `VECTOR`, `SVECTOR`, `MATRIX`, `CVECTOR` by value or const | 22 | a Lua table, copied into a temporary |
| the same types by non-const pointer or reference | 20 | returned to Lua; each listed as out or in-out |
| `KfActor`, `KfEffectRecord`, `KfMapObject`, `KfMapEvent`, `KfFloorItem` by pointer | 54 | a pool slot index, checked on the way in |
| anything else: buffers, spans, arenas, callbacks, `std::vector`, structs holding pointers | 181 | held back |

And by what the function does when run. Each function counts once, in the first class it reaches:

| reaches | of the 368 not held back by signature |
|---|---:|
| a frame (`host_read_buttons`, `host_present_*`) | 64 |
| the allocator or a file load | 18 |
| the current actor | 27 |
| none of these | 259 |

The second table is an estimate. Of the 259, about 90 sit in headers that need a frame, loaded
resources or the card (render, menu drawing, cutscene, TMD, save, sound lifecycle), which leaves
about **170** callable in the first stage, before the fuzzer takes any out. It includes
`player_add_experience`, `player_adjust_hp`, `actor_pool_find_at_tile` and the `effect_spawn_*`
family.

The hazard counts come from a throwaway call graph over 100 translation units, and they are low:
it matches by name, so overloads merge; it does not follow function pointers, so the map scripts'
and actors' dispatch tables are missing; only the current actor was traced, not effects or map
events; and `src/audio/sound.cpp` failed to dump and was not looked into. Step 4 replaces it.

Every parameter in the headers has a name, so the listed file can key on it. There are 989
parameters, 384 of them plain integers with no type-level bound: that is the range work.

## Decided

The user accepted all four recommendations, 2026-11-01.

1. **Exposed by default: everything the generator can describe,** less what the listed file holds
   back, which can be a whole header. This is the raw layer's rule, and it avoids the wait for a
   binding that the raw layer dropped a listed subset to avoid. The cost is fuzzing about 170
   functions with hidden preconditions.
2. **A call that changes state runs only in a new callback, `on_between_updates`,** and is a Lua
   error anywhere else, including an event handler raised inside that callback. It runs in
   `mods_between_updates`, after the queued warp and equips. A call there returns its value at once,
   and `on_update` and event handlers can set a flag for it. Functions listed `any_time`
   (`angle_sine`, `fixed_vector3_length`) can be called from anywhere, because they change nothing.
3. **Functions that run frames ship in a second stage.** They are the ones mods will want most
   (opening a shop, showing dialogue, playing a cutscene), but they need nested callbacks proven safe
   first. Warps are already reachable through `kf.warp`.
4. **The name is `kf.call.<function>`,** with the same promise as `kf.raw`: the decomp's names,
   following its renames, nothing stable.

## The surface

```lua
function on_between_updates()
    kf.call.player_add_experience(100)
    local slot = kf.call.actor_pool_find_at_tile(10, 12)   -- the integer the game returns
end

function on_draw()
    local s = kf.call.angle_sine(1024)       -- any_time
    kf.call.player_adjust_hp(-5)             -- an error: stops the mod, as a bad raw write does
end
```

- Arguments are positional, in the game's order. Too many, too few, or a wrong type or range is a
  Lua error that stops the mod.
- A return value is a raw value, or a table for a struct, copied. Out parameters follow it as
  further return values. A pointer to a pool's element returns its slot index, or nil for null.
- A slot index passed in must be in range and in use, or it is an error.
- `kf.call` is off in coop, as every mod is.

## Where the code goes

- **libverdite**: `verdite/raw_call.h`, beside `raw.h`. A function's description (name, parameters
  with type, limits and role, the return, when it may run, a generated invoke) and the Lua binding.
  Argument checks reuse `RawType` and `RawLimits`, and step 9's hook points can use the same
  descriptions. Unit tests over hand-written functions, with no game.
- **`raw.h`**: unchanged. A run-time check (a slot in use, a cell on the loaded floor), which a static
  interval cannot say, sits on the function's parameter in `raw_call.h`.
- **Mod host** (`libverdite/src/mods.cpp`): the new callback, and the phase a call is made in.
- **Generator**: the same run as `tools/raw_layer.py`, so each type is described once, writing
  `call_layer.cpp` and adding to the report. It names functions as template arguments
  (`raw_function<&player_adjust_hp>`), so the compiler checks each signature against its description
  and a generator bug is a compile error. It reads `decomp_src/include/kf` only, so `host_*` and the
  rest of `port/` are never callable. Functions private to a `.cpp` are in no header; a patch moves
  one out when a mod needs it.
- **The listed file**: `port/ext/call_layer.toml`, whose keys fail the build when they name nothing,
  as the raw layer's do. It holds parameter ranges, out or in-out roles, `any_time`, held functions
  and headers with reasons, and each pool behind a handle.
- **Run-time checks**: a small table in `port/ext/`, each named from the listed file. A new one is a
  new entry.
- **KF1's binding**: one entry in `port/ext/mods.cpp`, as `kf.raw` has.

## Order

1. ~~Answer the four questions.~~ Answered 2026-11-01 (above).
2. ~~Fix the host, in its own commit, with tests over a C function that re-enters the host.~~ Done
   2026-11-01. A mod that fails while a call of its own is running stops running Lua at once, and its
   state closes when the outermost call returns. Everything nested in a callback spends that
   callback's one budget; a first attempt gave a nested callback its own and restored the outer one,
   which still hung, since setting the hook resets Lua's count, and would have let each handler spend
   a whole budget. `mod_spend_instructions` is there for `kf.call`'s fixed charge, sized in step 8. A
   call that raises an event in *another* mod still gives that mod a whole budget per raise, so the
   charge bounds this only together with the handler's budget: at worst a stall, not memory, and
   step 8 measures it.
3. Write `verdite/raw_call.h` and the definitions it adds, and get it agreed before the implementation.
   Any change found while implementing, including by a subagent, goes back to the user, each named.
4. **Stage one (API 0.13).** libverdite implementation and tests, its own commit.
5. An audit in the generator's tool, run by `scripts/check.sh` and not by the build, since it reads
   every source: it fails when a callable function reaches a frame, the allocator, a file write, or a
   current object without the listed file saying so. It replaces the throwaway graph and must follow
   the dispatch tables or list them, and cover `sound.cpp`.
6. The generator, the listed file and the CMake step. Report what was left out and why.
7. The fuzzer: a `KF_FUZZ_CALL` mode (`fuzz.py --call`) that calls a random callable function with
   in-range arguments from `fuzz_between_updates`, half at the edges, as the raw mode does, and a
   finding when a stage-one call reaches a frame at run time, which catches what the audit cannot see.
   Each crash, stall or `host_fail` becomes a range, a run-time check, a hold or a game patch; the
   source is scanned for each parameter's use as an index, divisor or bound, since reading found what
   the fuzzer missed last time. Then `--call --raw` together, since a raw write can break what a call
   assumes. It does not ship before 96 clean runs under ASan and UBSan, the raw layer's bar.
8. Binding, definitions file, MODDING.md's reference, API 0.13. Measure a call's cost in a probe mod
   (a loop of 100 000) and set the charge from it.
9. **Stage two (API 0.14).** Handle the three remaining classes, each with its own fuzz campaign:
   current-object functions take the slot as their first argument, and the binding binds it, calls,
   and unbinds; allocator and load functions get a hold or a reason to allow each one; frame-running
   functions run with nested `on_update` and `on_draw` allowed, a `game_play`-style function that
   never returns held, and a stall the fuzzer can see. A nested frame also reaches the Mods panel and
   `ModHost::scan`, which unload mods, so those must wait for the outermost call to end. MODDING.md's
   step 8 is struck when this lands.

To check during step 2: whether `host_between_updates` runs while `kf.play.paused` holds the game,
since a paused mod may then get no `on_between_updates`. It does not run at the title or in a menu.

## Held back from the start

The fuzzer would not necessarily find these, so they are listed by reading:

- **Reaches past the sandbox**: all of `kf/game/save.h` (it writes the card), and whatever the
  audit's file-write check adds.
- **Ends or stalls the program**: `game_shutdown`, `sound_start`, `sound_shutdown`,
  `frame_pacer_wait`, `game_main_loop`, `game_play`.
- **Needs an open frame or loaded scene**: `kf/game/render.h`, `kf/lib/tmd.h`, `kf/lib/render_face.h`,
  `menu_draw_*` and all of `kf/cutscene/`. Some may come back later; `render_publish_light` is the
  first candidate.
- **By signature**: any raw buffer, span, arena or callback. No Lua value becomes an address.

## Checks

From AGENTS.md and the global rules; a subagent's brief names these:

- Write C++ by `docs/cpp-reference.md` (its "Before finishing" list) and `docs/code-style.md`.
  libverdite has no `kf/` or decomp include, and its changes are in their own commits.
- Never edit `decomp/` or `decomp_src/`; a game patch goes through `work/` and `tools/patches.py`.
- Unit tests in the same commit: argument count, type, range, a run-time check, out parameters, a
  phase refusal, a failure and a budget in a nested call.
- `scripts/check.sh` with the sanitizer build.
- The fuzzer's call mode clean under ASan and UBSan (step 7).
- An offscreen run with a probe mod: a call the game then shows (`player_adjust_hp` against
  `kf.player.hp`), an error for each bad argument, a refusal from `on_update` and from an event, and a
  handler that errs inside a call.
- Commit messages state the finding, in the imperative, with the consequence and what was and was
  not checked.
