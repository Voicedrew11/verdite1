# Fuzzing the decomp

Bringing `main`'s fuzzer (`patches/Fuzz.cs`, `scripts/fuzz.py`; "Fuzzing headless playthroughs"
in `main`'s `docs/DEVELOPMENT.md`) across, and what this branch can do that RecompOne could not.
Proposed and agreed 2026-10-25. How to run it and what it found are "Fuzzing" in
[verdite-port.md](verdite-port.md); this page is the plan.

**Done** (2026-10-25): steps 1-3 of the order below, and from step 4 `--check-determinism`,
`--compare-setting` and `--minimise`. The determinism check passed: one seed twice, and its
recording replayed, gave equal traces for all 4,147 updates.

## What carries, what changes, what goes

The C# fuzzer is a seeded pad policy, macros (warp, goto), invariants every frame, a recording,
coverage, and a runner that launches many headless runs and groups findings by stack. All of
that carries. Its plumbing does not:

| `main` | here |
|---|---|
| `KF1_HEADLESS`, `KF1_TURBO` (fork `0112`) | `--headless --turbo` already exist ("Driving the game") |
| the game's `rand` seeded by reflection into `BiosA` | `random_state` is file-static in `src/lib/random.cpp`, seeded 1: one patch adds `random_seed(u32)` |
| the policy stepped on `FramePacing.Boundary` | stepped in `host_read_buttons`, which every game loop (menus included) calls; the hold is in game-clock ticks, which turbo makes exact |
| invariants on guest RAM addresses (`0x800652A8`, ...) | invariants on the named globals: `player_state`, `actor_state`, `map_object_state`, `effect_state` |
| `warp` through `Warp.cs`, then a move out of rock | the game's own `player_warp_change_floor(floor, variant)` and `player_warp_same_floor(variant, x, z)`; landing cells from `floor_entry_cells` and the cell grid |
| no load macro ("no way into a saved game but the menu") | quick save and quick load exist: a `load` macro, and runs that **start from a card file** |
| soft memory faults (fork `0114`) | no guest RAM: ASan and the hardened standard library (below) |
| ordering-table walk | no ordering table; the renderer frontend's submissions are checked instead (counts, texture pages) |
| coverage: one hit byte per recompiled function (fork `0113`) | clang's coverage on `decomp_src/`: lines and branches, not functions |
| `--verify` (C# routines beside recompiled ones) | nothing to verify; the differential oracles below take its place |
| replay "close, not exact" (threads, wall-clock vblank) | **exact**, if the determinism check passes (M1) |

To check while porting: whether the load screen's quit that never returns (OPEN `0x80024F5C`,
19 stalls on `main`) is reconstructed as it was, and answer it No the same way if so.

## New uses the decomp gives

### 1. Exact replay, minimised findings, bisection

Under turbo the clock is `host.turbo_ns`, moved only by the game's own waits; the game thread
does all the work, and `random_next` is the only randomness. If a recording therefore replays
bit for bit (M1 checks it with the state trace, §4), then:

- a finding's recording is its reproduction, not "rerun the seed and hope";
- `fuzz.py --minimise` cuts a 20,000-tick recording down (delta debugging over its button spans
  and macros) to the shortest one that still breaks the same invariant at the same site;
- `fuzz.py --bisect <recording>` runs one recording across commits, or across the hunks of a
  patch, to name the change that broke it.

### 2. The sanitizers and the hardened library on game code

The C# fuzzer could see a bad pointer only when it left guest RAM. Here the game is native C++
built from spans and `std::array`s, so a **fuzz build** (a `fuzz` preset) compiles it with:

- ASan and UBSan, as the `sanitize` preset does now, but under play rather than a smoke walk;
- `_GLIBCXX_ASSERTIONS` (libstdc++'s checked `operator[]` on `std::array`, `std::span`,
  `std::vector`): an index one past `actors` inside `KfActorState` is not an ASan finding, since
  it stays inside the global, but it is an assertion here. Most of the game's tables are
  `std::array`;
- `host_fail` and an escaped `kf::codec::Error` counted as findings with their source location
  (the codecs keep `std::source_location`): each is the reconstruction saying "this cannot
  happen".

**Uninitialised reads without MSan.** Two of the four deliberate corrections in
`decomp/docs/port-notes.md` are the original reading registers or stack it never wrote (return
home, player-homing pitch). MSan needs SDL and everything else instrumented; instead, run the
same recording on a build with `-ftrivial-auto-var-init=zero` and one with `=pattern` and compare
state traces (§4). Where they part, game state depends on a local nobody wrote.

### 3. Invariants that come from the decomp's names and types

`main` could not check the creature table ("its type field's range is not known"). The decomp
names every field and types most of them as `enum class ... : u8`:

- **Enum fields hold a declared value.** `tools/enum_table.py` reads the decomp's headers
  (`clang -Xclang -ast-dump=json`) and writes the enumerators of each enum into a generated
  table; the KF1 invariants check every enum field of `player_state`, each live `KfActor`, each
  map object and event. `kf_enum_decode` is an unchecked cast at 68 sites, so a bad byte on disc
  or in a save shows up there first.
- **Record pointers point into their tables.** `player_state.equipped_weapon_record` into the
  weapon records, the five armour pointers into the armour records, `selected_magic_record` into
  the magic records, `actor_state.current` and `player_target` into `actors`, each actor's
  `animation_cache` into the cache.
- **`main`'s invariants**, by name: HP ≤ max HP, MP ≤ max MP, the level-up's caps, floor 1-5,
  position inside the grid, inventory counts ≤ 99, the unused item ids at 0, no stall.

### 4. State traces and differential oracles

A **state trace** is a hash of each named global per game tick, written to a file, with pointer
fields replaced by their index into the table they point at (so two builds' traces compare).
Comparing two traces gives the first tick and the first field where they part. On one machine,
with exact replay, that is a cheap oracle for questions the recompilation could only eyeball:

- **Classic mode against the unpatched decomp.** The same recording on our build in Classic mode
  and on `decomp/` built with no `patches/`. Every patch claims Classic is the original; this
  proves it, field by field, and names the patch that is not.
- **Visual settings change no game state.** The same recording with the null and Vulkan
  renderers, 4:3 and 16:9, HUD at the edges on and off, smoothing on and off, Japanese and
  English: the traces must be equal. This is `main`'s `--vary` with an answer, not a sample of
  crashes; widescreen's widened cull would be caught if it ever changed which actors update.
- **Moving the pin.** `patches.py upgrade` runs the regression recordings (§6) before and after;
  sushi-shi's regenerations that should be renames are then shown to be, and the rest name the
  tick where behaviour moved.
- **Against the recompilation** (the oracle in `verdite-port.md`, "Things to plan for"): one
  input recording, in game ticks, on both, comparing the named fields with the PS1 addresses
  `main` reads. It needs a name-to-address table (sushi-shi's matching branch has the
  addresses) and tick alignment between the two clocks, and the port-notes' deliberate
  corrections are expected differences. The largest piece of this plan, so last.

### 5. Coverage that can guide

Clang's source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`) on `decomp_src/`
only gives each run's lines and branches; `fuzz.py` merges them into an `llvm-cov` HTML report of
the reconstruction, which says which branches of which function no run has reached. Two uses:

- **The corrections the port-notes say are unverified** ("The affected combat paths still need
  direct runtime verification"): the jump landing, return home (action 33), the player-homing
  pitch (effect kind 20), floor 5's spatial panning (actor definitions 5-7). The report shows
  whether play reached them; runs started where they happen (`--area 5`, a card file beside the
  enemy) do the rest.
- **Coverage-guided play.** With `-fsanitize-coverage=inline-8bit-counters` on `decomp_src/`,
  the runner keeps a corpus of (start card, recording) pairs that reached new edges and makes new
  runs by extending, splicing and mutating them, AFL's loop over whole playthroughs. It needs
  exact replay (§1) and start states (card files, which the port now writes in the PS1's
  format); `fork()` after boot and load would save the boot per run, if the language and console
  threads are not running at the fork.

### 6. libFuzzer on what comes from outside

Small, independent of the rest, and the rule is already that saves, mods and files are
bounds-checked where they enter:

- the decomp's own `tests/fuzz_codecs.cpp` (TIM, assets, placements, audio banks, music) is not
  built here; build it against `decomp_src/` so it covers our patches too;
- ours: the card reader and writer (`verdite/memory_card.h`), the save entry loader (an entry
  holds PS1 addresses for magic, weapons and armour; a crafted save must not become a pointer),
  the PPF patcher, `DiscImage` (CHD and plain), `settings.json`, the translation tables;
- later, the Lua mod API, once 1.0 sandboxes it.

Each finding's input goes into a corpus next to its target, and recordings that found a game
bug go into `tests/fuzz/recordings/` as regressions (inputs only, no disc data).

## The interface

As proposed. The built headers, `libverdite/include/verdite/fuzz.h`,
`libverdite/include/verdite/state_trace.h` and `port/include/kf/platform/fuzz.h`, are the
reference; they moved from this in four ways. The clock is `fuzz_poll`'s (ending the run, status,
stalls), so a modal loop that never reads the pad still ends. Checks run in
`fuzz_between_updates`, where macros also run. A macro has `choose` and `run`, so its arguments
are recorded before it runs (a macro that crashes the game is in the recording). And there is a
menu table, escape presses, `fuzz_trigger` and `fuzz_note`. A trace line is per update, not per
tick.

Two headers, as the agent channel splits: the library's half knows no game, KF1's half hands it
buttons, macros, invariants and state.

```cpp
// libverdite/include/verdite/fuzz.h
#ifndef VERDITE_FUZZ_H
#define VERDITE_FUZZ_H
// A monkey at the pad: seeded random play stepped on game ticks, macros now and then,
// invariants checked as the game runs, and a recording that replays the run. Findings are
// "[<tag>-FUZZ] {...}" lines on stdout for scripts/fuzz.py. Game-agnostic: the game names its
// buttons, macros and invariants.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// The policy's random numbers, separate from the game's so a policy change does not move it.
class FuzzRandom {
  public:
    explicit FuzzRandom(std::uint64_t seed);
    std::uint32_t below(std::uint32_t bound); // 0..bound-1
    bool chance(std::uint32_t per_10000);

  private:
    std::uint64_t state_;
};

// One thing the policy presses: a mask of the game's buttons, how often, how long.
struct FuzzPress {
    std::string name;            // "walk", "attack", "menu"
    std::uint32_t buttons;
    int weight;
    int min_hold_ticks, max_hold_ticks;
};

// A jump the pad would take too long to make: a warp, a load. run does it on the game
// thread and returns its arguments, which go in the recording so a replay does the same.
struct FuzzMacro {
    std::string name;            // "warp"
    int per_10000;               // chance per policy decision
    std::function<std::string(FuzzRandom &)> run;
    // Replays a recorded macro from its arguments; false when they do not parse.
    std::function<bool(std::string_view args)> replay;
};

// Something that must always hold. check returns what is wrong, or nothing.
struct FuzzInvariant {
    std::string name;            // "hp-over-max"
    std::function<std::optional<std::string>()> check;
};

struct FuzzConfig {
    std::string tag;                     // "KF1"
    std::uint64_t seed = 0;
    std::uint64_t ticks = 20000;         // then a clean exit
    std::optional<std::string> replay;   // a recording to press back instead of the policy
    std::string recording = "fuzz-input.log";
    bool stop_on_violation = false;      // exit 3 at the first
    std::vector<FuzzPress> presses;
    std::vector<FuzzMacro> macros;
    std::vector<FuzzInvariant> invariants;
};

// On the game thread, before play.
bool fuzz_start(FuzzConfig config);
bool fuzz_active();
// The buttons held at this game tick, from the policy or the recording; also counts the tick
// and ends the run at config.ticks. Called once per game input read.
std::uint32_t fuzz_buttons(std::uint64_t tick);
// Checks the invariants, unless settling. Called where the game state is whole (each update).
void fuzz_check();
// After a load, a death or a macro: no checks for this many ticks.
void fuzz_settle(int ticks);
// A finding the game reports itself (a host_fail, a codec error): logged, then the run goes on.
void fuzz_report(std::string_view kind, std::string_view detail);

} // namespace verdite

#endif // VERDITE_FUZZ_H
```

```cpp
// libverdite/include/verdite/state_trace.h
#ifndef VERDITE_STATE_TRACE_H
#define VERDITE_STATE_TRACE_H
// A hash of the game's state per tick, written to a file, so two runs of one recording can be
// compared: the first tick and region where they part. Game-agnostic: the game names its
// regions and how each pointer field is written (as an index, so builds compare).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace verdite {

struct TraceRegion {
    std::string name;                                   // "player_state", "actor[3]"
    // Writes the region's bytes as they should compare: pointers as table indices,
    // padding and host-only fields left out.
    std::function<void(std::vector<std::byte> &out)> write;
};

class StateTrace {
  public:
    // Opens path for writing; false when it cannot.
    static std::optional<StateTrace> create(const std::string &path, std::vector<TraceRegion> regions);
    void record(std::uint64_t tick);

  private:
    // the file, the regions, a reusable buffer
};

struct TraceDivergence {
    std::uint64_t tick;
    std::string region;          // the first region that differs at that tick
};
// Nothing when the traces agree for as long as both run.
std::optional<TraceDivergence> trace_compare(const std::string &a, const std::string &b);

} // namespace verdite

#endif // VERDITE_STATE_TRACE_H
```

KF1's half is `port/include/kf/platform/fuzz.h`, shaped like `agent.h`: `fuzz_start()` reads
`KF_FUZZ`, `KF_FUZZ_TICKS`, `KF_FUZZ_REPLAY`, `KF_FUZZ_AREA`, `KF_FUZZ_KIT`, `KF_FUZZ_STRONG`,
`KF_FUZZ_TRACE` (diagnostic switches, so environment variables, as `KF_AGENT` is); `fuzz_buttons()`
is or'd into the pad beside `agent_buttons()` in `host_read_buttons`; `fuzz_check()` runs there
in the gameplay context. The tables (presses, macros, invariants, trace regions) are data at the
top of `port/platform/fuzz.cpp`. Game-code patches: `random_seed` only; `host_fail` reaching
`fuzz_report` is in `port/`.

`scripts/fuzz.py` is `main`'s, retargeted: `build/<preset>/verdite1 --data data/en --headless
--turbo --skip-intro --saves <run dir>`, the same outcomes (crash, hang, violation, ok; "fault"
becomes sanitizer and assertion reports), signatures from the crash report's named stack,
`--mix`, and `--vary` turned into §4's comparisons.

## Order

1. ~~**The fuzzer as `main` had it**, on this build: the two headers, `random_seed`, the KF1
   adapter, `fuzz.py`. Then **the determinism check**: one seed, two runs, equal traces.~~ Done
   2026-10-25; the check passed.
2. **The `fuzz` preset**: ~~ASan, UBSan, `_GLIBCXX_ASSERTIONS`~~ (done), `host_fail` (a `fail`
   outcome, by its message) and codec errors as findings (done); ~~the pointer invariants~~
   (done: `equipment`, `actors`, `effects`). Still to do: the generated enum table.
3. ~~**libFuzzer targets** (§6)~~: `memory_card`, `save_card`, `disc_image` and the decomp's
   `codecs` (done). The PPF patcher is not one: it checks the Japanese files' hashes before
   it parses, so it needs the disc. Settings and translations are still to do.
4. **Differential**: ~~visual settings~~ (`--compare-setting`, done), ~~`--minimise`~~ (done);
   still to do: Classic against the unpatched decomp, the zero/pattern initialisation pair,
   recordings checked on pin moves, `--bisect`.
5. **Coverage**: the report, the port-notes' unverified paths; then coverage-guided play from
   card files, then `fork()` snapshots if the boot is the cost.
6. **Against the recompilation**, once there is a name-to-address table.

## Questions

Taken as the defaults when the plan was agreed; open to change.

- `state_trace.h` is in libverdite, beside `fuzz.h`: it knows no game.
- Regression recordings are not committed yet. A recording replays the build that made it, so
  committed ones would need re-recording when the game changes.
- `_GLIBCXX_ASSERTIONS` is in the `fuzz` preset only (`KF_HARDEN`), so `sanitize` stays the
  check that `scripts/check.sh` has always run.
