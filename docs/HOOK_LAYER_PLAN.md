# The hook layer (modding API step 9): plan and record

Written and built 2026-11-02 (API 0.15). Step 9 of [MODDING.md](MODDING.md#order): a mod's code before,
after or in place of a game function, whoever calls it. The surface is in
[MODDING.md](MODDING.md#kfhook). It reuses the [call layer](CALL_LAYER_PLAN.md)'s descriptions, checks
and crossings, so a hook takes and returns what `kf.call` does.

## What makes this harder than the call layer

The line is the same: memory safety. What a replacement leaves odd is the mod's bug, and what can
corrupt memory is ours. The new hazards, all found by reading the code or by the fuzzer:

- **A mod's value now reaches the game's callers.** A `kf.call` argument goes into one function that
  was read. A replacement's return goes into every caller, and a caller may index with it or take it
  as never null. `map_object_effect_index` ends the program on a null effect, and fuzzing found it.
- **Skipping a function is new.** Nothing a mod could do before kept a function from running.
- **The game's own values may not pass the call layer's checks.** A listed range says what a mod may
  pass, and the game passes more. Handing the game's own value back must never stop a mod.
- **It fires anywhere.** A hook runs inside any loop, at rates far above any event, off a thread if
  the function is called from one, and below a watched `game_play` that never returns.

## Decided

The user asked for the step and left the design open, so these are this author's choices. Each can be
reversed in the listed file or one function.

1. **Reach is earned, not listed function by function.** Watching needs only a signature that crosses.
   Changing arguments needs a function `kf.call` can call, since its ranges have been read and fuzzed.
   Replacing also needs a return a caller can take: none, a bool, an enum, a pool's element, or a value
   the listed file gives a range. The alternative, replacing anything and holding what the fuzzer
   breaks, cannot be trusted for functions whose callers the fuzzer rarely runs.
2. **Null from a replacement needs a listing,** even where the game's own function can return null.
3. **A value equal to the game's own passes unchecked,** in arguments and in results.
4. **A failing mod never skips the game's function.** The next replacement runs, or the game's own,
   unless the mod already ran it, and then what it returned stands.
5. **Handlers share one budget a frame,** per mod. An event's raise gets a whole budget; a hot hook with
   the same would stall the game for 100,000,000 instructions per call.
6. **Handlers run inline and cannot make a `kf.call` that changes state,** as an event's cannot.

## How a call reaches the hook point

No patching at run time, as the plan asked, and no linker trick: `--wrap` misses calls within a file,
which is most of the calls that matter.

`tools/raw_layer.py` runs at build time and, for each function with a hook point:

1. copies the source defining it into `build/<preset>/hook_src/`, its definition's name changed to
   `kf_game_f`, with `#line` so that messages and debuggers still name the file in `decomp_src/`;
2. defines `f` itself in `raw_layer.cpp`, which every caller reaches, in any file:

```cpp
s32 f(s32 a) {
    if (!kf_hook_active[17].load(std::memory_order_relaxed))
        return kf_game_f(a);
    return verdite::raw_hook_fire<&kf_game_f>(kf_hook_layer(), 17, a);
}
```

The definition is found by a small scan of file-scope code (`hook_layer.find_definitions`). If it
misreads one, the link fails: `f` is defined twice, or `kf_game_f` not at all. `decomp_src/` stays the
patched decomp, and `tools/call_audit.py` reads that, not the copies.

A function using a current object goes through the call layer's binder, with the current one read
from the `current` its `[subjects]` entry names, so a hook on a monster's AI gets its slot.

## Where the code goes

- **`verdite/raw_hook.h`**: the layer's description, the kinds of reach, and `RawHookCall`, what answers
  a hook point sees. **`verdite/raw_hook_bind.h`**: the templates a hook point is built from.
- **`libverdite/src/raw_hook.cpp`**: checks of a handler's values, the Lua tables, the definitions.
- **`libverdite/src/mods.cpp`**: the order mods run in, the handlers, what happens when one fails.
- **`tools/hook_layer.py`**, part of the raw layer's run; **`port/ext/hook_layer.toml`**, its listed file. The
  report is `raw_layer_report.txt`.
- **`port/platform/fuzz.cpp`**: `KF_FUZZ_HOOK`, answering in a mod's place.

## What it covers

Of 558 functions in the headers: **270 hook points**, 136 to watch, 22 to change arguments, 112 to replace.
Of the 288 with none, the report gives each reason. The largest: defined inline in a header (57), math
held for speed (30), in a namespace (27), and signatures that do not cross, such as a pointer whose role
is not listed. Functions in `kf/audio` are compiled outside the game's own sources and are not routed.

To widen it: a role for a pointer in `call_layer.toml` makes a function hookable; `allowed` there lets
it change arguments and be replaced; `returns_range` here lets a function returning a plain number be
replaced. A function private to a `.cpp` needs a patch to move it to a header.

## Measured

On the plain build, headless at turbo, seed 11, 2026-11-02:

- **Nothing hooked**: 95.34 and 95.75 s of CPU before, 96.08 and 96.38 s after, so about 0.7%; seed 12,
  100.52 against 100.79 s, 0.3%. Later pairs ran beside other work and were dropped. Final state and
  update count match the build before, so the renaming did not change the game.
- **A hook on `actor_update_awareness`**, 1,638,660 calls in 60,000 ticks: 0.26 µs a call for a `before`
  handler, 0.64 µs for a replacement running `original`, over 32.74 s with none. Same updates.

## Checks

- Unit tests over a small game with no real one (`libverdite/tests/raw_hook.cpp`), under ASan and UBSan.
- `scripts/check.sh` in full.
- Probe mods in the game under ASan and UBSan, headless with leak checks: a hot replacement on a monster's AI
  (822,409 calls in one run), arguments changed, watching what `kf.call` holds back, `kf.call` refused inside a handler,
  a mod failing in a chain, nested with another mod. Exits cleanly, with `on_unload` run.
- The fuzzer answering every hook point in a mod's place (`--hook`), under ASan and UBSan, with `--preset fuzz --hook 10
  --mix --vary --ticks 20000`, on the final build. Each function's first calls are acted on far more often, so hot ones
  do not take every action. 96 runs each, no crash, sanitizer report or `host_fail`:

  | with | seeds | replacements | arguments changed | functions acted on, of 134 |
  |---|---|---:|---:|---:|
  | nothing more | 10000-10095 | 58,371 | 166,554 | 114 |
  | `--call 6000` | 11000-11095 | 223,617 | 274,831 | 134 |
  | `--raw 4000` | 12000-12095 | 60,064 | 168,729 | 116 |

  The runs with no `--call` miss what the game seldom calls itself; `--call` reaches those through their hook points. 33,
  38 and 35 runs stalled, all in the `opening` context: a replacement or a change sent the game back to its title, as
  raw writes can. Why each left play was not traced. These count as the mod's own bug, not a finding, since frames go on
  and memory is safe.

  Found on the way: `map_saved_link_valid` ends the program when a load finds a bad link, so it is watch only. A
  replaced `effect_spawn_*` returning null ended the program in `map_object_effect_index`, so a replacement's null
  needs a listing (none has one yet). An action a monster's definition has no clip for drew clip `NONE`, which
  `render_bind_instance_vertices` took as bad disc data and ended the program: a game patch draws it unanimated.
  `kf.call.actor_set_action` could reach this too, though its campaigns did not.
- These fuzzer runs answer in a mod's place in C++, so the Lua path below them was exercised by the unit tests and
  probe mods, not at this volume.

Not checked: how any of it looks in play, a build for Android or Windows, or a drawn run with hooks.
The Lua path's frames were tested by unit tests only.

## Left for later

- A function returning a plain number can only change its arguments until someone reads its callers and gives
  it a `returns_range`. 22 wait on this, among them the distance functions and `actor_pool_find_at_tile`.
- `after` cannot change the result; a replacement does that, and so is held to the same reach.
- `kf/audio`: routing it means compiling it in the game's sources.
- Stalls under `--hook` are not findings: see the campaigns above.
- `returns_nil` has no entries: reading each pool-returning function's callers would let replacements return nil.
