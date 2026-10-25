# Verdite1 on the decomp

Branch `decomp` of `Voicedrew11/verdite1`: Verdite1's features carried onto
[sushi-shi/kings-field-decomp](https://github.com/sushi-shi/kings-field-decomp)
(`port`), starting from [acranon/kf1-enhanced](https://github.com/acranon/kf1-enhanced)
(`enhanced`). Both are CC0. Started 2026-10-22.

`main` is the RecompOne recompilation and shares no files with this branch.
Nothing from `main` is merged; features are rewritten here in C/C++.

The rendering plan (API, platforms, tiers, picture quality) is
[RENDERER_PLAN.md](RENDERER_PLAN.md). What the port does in place of the
console's hardware, and why it draws fewer frames than the recompilations, is
[RUNTIME.md](RUNTIME.md). Online play (messages, bloodstains, coop) is
[NETWORKING_PLAN.md](NETWORKING_PLAN.md), a draft.

## Direction: every Verdite port becomes a decomp

Decided 2026-10-22: in the long run every Verdite port is built on a
decompilation, not on RecompOne, because a source port has the least overhead.
There is nothing to emulate and nothing to recover from a packet stream, and
each new platform costs only a renderer backend, an audio output and a
platform layer.

A matching decomp is half of what a port needs. The other half is the source
port, which replaces the GTE, GPU packets, ordering tables and libgpu with a
real renderer and platform layer. sushi-shi did that for KF1 (`port`).

| port | game | decomp (2026-10-22) | port layer | state |
|---|---|---|---|---|
| Verdite1 | KF1 JP `SLPS-00017` | sushi-shi, 98.7% matching | yes (`port`, kf1-enhanced) | **move now** (this branch) |
| Verdite2 | KF2 US `SLUS-00158` | sushi-shi's JP `SLPS-00069`; our US fork (`../kings-field-2-decomp`, `us-port`) in progress | none: matching branches and `docs/port-plan` only | finish the US decomp, then write a port layer |
| Verdite3 | KF3 US `SLUS-00255` | **none exists** | none | start a decomp (decomp.wtf can import the disc and run helper agents) |
| kfiv | KF4 PS2 `SLUS-20318` | ours (`../kfiv-decomp`): the image matches byte for byte; the game code is still assembly | none | long way off |

Until a game's decomp and port layer exist, its recompiled Verdite stays the
shipping build. RecompOne goes into maintenance, not retirement.

### libverdite: the C++ core

The C# Verdite Core and the RecompOne fork are replaced by a **shared,
game-agnostic C++ library** kept as a subtree, under the same rules as now:
nothing game-specific in it, its edits in their own commits, and no change to
it may move another game. It holds:

- platform: SDL3 window and input, analog tuning, rumble, gyro, focus pause,
  crash reports, hang watchdog;
- settings and the F1 overlay, Discord IPC, state beacon and command channel,
  the fuzz harness, the profiler;
- the renderer frontend and its backends ([RENDERER_PLAN.md](RENDERER_PLAN.md)).

Each game supplies an adapter: its decomp, plus "here is the camera, the map
cells, the models." KF1-KF3 share one evolving engine (TMD models, map-cell
grids, the same lighting), so the renderer interface should hold for all three.
It is first built inside Verdite1, then extracted into the subtree.

It is named **libverdite**, so that it is not confused with the C# Verdite
Core (`Voicedrew11/verdite-core`), which stays with the recompiled builds for
as long as they ship. It lives in this repo, in `libverdite/` (started
2026-10-23), until a second game uses it; then it becomes
`Voicedrew11/libverdite`. Nothing in `libverdite/` includes a `kf/` or decomp
header: the game hands in its tag, port, state and commands. The first tenant
is the agent channel; its KF1 half is `port/platform/agent.cpp`.

### Things to plan for

- **Faithfulness risk changes shape.** A recompilation runs the original
  instructions; a source port is rewritten C++ and can diverge, and
  `docs/port-notes.md` already lists deliberate deviations. Keep the recompiled
  build as an **oracle**: replay one input recording on both and compare game
  RAM (Verdite Core's `Differential.cs` is the idea). The fuzzer then also
  checks the two engines against each other.
- **Region.** The decomps are JP; Verdite2 and Verdite3 are US releases. Each
  needs its US relink adapted (as the KF2 fork does), with its text encoding
  and fonts.
- **Saves.** Players coming from a Verdite build want their saves. KF1 now
  writes the PlayStation's own save file (`BISLPS-00017KF`, the card file's
  40960 bytes, done 2026-10-24): a save taken out of a C# Verdite1 card
  (`carda.sav`) loads, and one written here loads in the C# build. The earlier
  per-slot `.kfs` files are brought in once on start. Still to do: importing
  and exporting whole cards (`verdite/memory_card.h` reads and writes them) from
  the game, for the C# builds and emulators (`.mcd`, `.mcr`).
- **Following upstream.** sushi-shi moves fast and regenerates source. Keep
  Verdite features behind the core and small hooks rather than edits spread
  through the decomp's files, the same discipline as the fork patches.
- **Agent tooling first.** The beacon, command channel, headless/turbo mode and
  fuzzer are what let an agent measure without the user, so they come before
  the visual features.
- **Licensing is simpler.** The decomps are CC0; nothing from the disc ships.

## Layout

Holyland's (`../holyland`, `docs/port/README.md` there), set up 2026-10-22:

```
decomp/        submodule: Voicedrew11/kings-field-decomp (our fork of sushi-shi), pinned on `port`
patches/       Verdite's edits to the decomp's game code, one diff per copied directory
port/          platform layer, renderer, overlay, settings, mods: ours, edited in place
libverdite/    the game-agnostic C++ library the ports share (agent channel so far): ours, knows no game
third_party/   Dear ImGui, Lua, cJSON, the GLES headers and loader (kf1-enhanced's vendor/)
tools/         patches.py, from holyland
work/          (gitignored) the editable copy patches.py makes
```

- **The decomp is read-only.** Configure copies its game code (`codecs`,
  `src/{audio,cutscene,game,lib}`, `include/kf/{audio,cutscene,game,lib}`; the
  list is `KF_COPIED` in `CMakeLists.txt` and `COPIED` in `tools/patches.py`),
  applies `patches/`, and compiles the read-only copy in
  `build/<preset>/decomp_src/`. It fails if `decomp/` has local changes.
- **Game-code edits**: `python3 tools/patches.py apply`, edit `work/`,
  `python3 tools/patches.py save`, commit `patches/`.
- **The decomp's platform layer and renderer are not used.** `src/platform`,
  `src/renderer`, `include/kf/platform` and `include/kf/renderer` are replaced
  by `port/`, which is where the Vulkan renderer and the C++ core will grow.
  `port/include` comes first on the include path.
- **The pin is `7bd4567c`**, `sushi-shi/port`'s head on 2026-10-23. The first
  layout pinned `150ae354`, the commit kf1-enhanced is based on, so that it
  reproduced kf1-enhanced exactly. Upstream's changes to the platform layer
  and renderer are carried into `port/` by hand, as a 3-way merge with the old
  pin as the base (`git merge-file`). The submodule's
  `upstream` remote is sushi-shi; `python3 tools/patches.py upgrade <commit>`
  moves the pin. The decomp has no `config/symbol_addrs.txt`, so unlike
  holyland's, renames are not followed: they come up as conflicts or compile
  errors. Move the pin often and in small steps.
- **The fork is a fork** so that pinned commits survive sushi-shi's
  regenerations. It carries only upstream commits.
- kf1-enhanced's tests (`tests/`) were not carried: its CMake never built them.
  Its WebAssembly build (`web/`) was dropped.

How kf1-enhanced's tree was split (from `ac147450` against the pin): its
`src/{platform,renderer,ext}` and `include/kf/{platform,renderer,ext}` went to
`port/`, `vendor/` to `third_party/`, `mods/` and its docs as they were; its
edits to the game code became `patches/` (36 files, seven patch files, 2,081
lines). The scripts the build uses (`english_patch.py`, the layout table) were
unchanged from sushi-shi's, so the build reads them from `decomp/scripts/`.

**Checked**: the new layout's binary has the same 3,939 symbols, with the same
names, sizes and types, as kf1-enhanced's built from its own tree. Its
read-only strings differ only in the source paths and in the order two strings
were laid out. It boots as before. `patches.py apply` then `save` changes
nothing, and configure refuses an edited `decomp/`.

## Remotes

| where | remote | repo | role |
|---|---|---|---|
| this repo | `origin` | Voicedrew11/verdite1 | `decomp` (this branch) |
| this repo | `kf1-enhanced` | acranon/kf1-enhanced | where `port/` came from; read for later changes |
| `decomp/` | `origin` | Voicedrew11/kings-field-decomp | the fork the submodule pins |
| `decomp/` | `upstream` | sushi-shi/kings-field-decomp | what the fork follows (`port`) |

At the branch point `enhanced` was 23 commits ahead of `sushi-shi/port` and 37
behind it. Nothing goes upstream to either: no pull requests, no issues.

## Build and run (Linux)

SDL3 is not a system package here; the flake's dev shell supplies the
toolchain and the English delta. A flake does not see submodule files, so it
fetches the decomp's scripts from the fork at the pin; **moving the pin means
updating the `rev` and `hash` in `flake.nix` too**
(`nix flake prefetch github:Voicedrew11/kings-field-decomp/<rev>`).

```bash
git submodule update --init decomp
nix develop -c cmake --preset linux
nix develop -c cmake --build --preset linux
```

The binary takes the disc and keeps extracted files under `--data`:
`--disc <cue|bin|iso|chd>`, `--english-patch <ppf|rar>`, `--language ja|en`,
`--saves DIR`, `--skip-intro`, `--extract-only`. The disc is the one
`main` uses, `../verdite1/disc/KingsField1.cue`; it is already the English
v1.0 image (extraction verified 428 English files), so it went to `data/en`:

```bash
mkdir -p data && nix develop -c build/linux/verdite1 \
  --disc ../verdite1/disc/KingsField1.cue --extract-to data/en --extract-only
```

Run with `scripts/run-linux.sh`, not inside `nix develop`: nix's libglvnd
does not see Fedora's Mesa ("Could not get EGL display"), so the script points
`__EGL_VENDOR_LIBRARY_DIRS` at `/usr/share/glvnd/egl_vendor.d` and puts
`/usr/lib64` on the library path. With that it booted (2026-10-22): 960x720
target, overlay ready, "Starting King's Field (English v1.0)". User files go
to `~/.local/share/Verdite/Verdite1/`. The `offscreen` SDL driver finds no
EGL device even so, so there is no headless run yet.

Without `--data` or `--disc` the game takes the release path, as on Windows:
game files in `data/ja` and `data/en` beside the game (beside the `.AppImage`
file, from `$APPIMAGE`, not the mount), or under the user folder when that
folder cannot be written; otherwise a `.cue`, `.bin`, `.iso` or `.chd` beside the game
is copied there once, otherwise a file dialog asks for one.

A `.chd` is read through libverdite's `verdite/disc_image.h` (libchdr, vendored in
`third_party/libchdr`), which hands the importer the same bytes as the `.bin`, so the checks, the
English patch and the extracted folders are unchanged. Checked 2026-10-24: `chdman createcd` of
`KingsField1.cue` (one `MODE2_RAW` track, 12,916 frames, 7.4 MB, 98.6% of hunks LZMA) extracts
the same 428 files as the `.bin`, 0.3 s slower. chdman 0.289 writes, for some tiny images, a map
that fails its own CRC in chdman and libchdr alike (`tools/make_disc_fixtures.py` avoids those
sizes); such a CHD fails `chdman verify` too. For the 3DS, libchdr's `CHDR_LOWRAM_TARGET` is to be
measured once a build runs there.

## Linux release (AppImage)

`scripts/package-linux.sh VERSION` builds `dist/Verdite1-vVERSION-x86_64.AppImage`
in podman (or docker) from `packaging/appimage/`. The nix build cannot ship: it
links against the store's glibc. The image is Ubuntu 22.04 (glibc 2.35, the
oldest the AppImage runs on) with clang 21 from apt.llvm.org and, built from
pinned releases, SDL3 (the Windows build's version), the Vulkan libraries and
cpptrace at the nix shell's versions, and a static bsdtar. The AppImage carries
`libSDL3.so.0`, the C++ runtime linked in, and bsdtar on `PATH` for the English
patch's `.rar` (GNU tar cannot read it); the Vulkan loader and drivers are the
host's. No English delta is embedded. The repository is mounted at its own path
with git's common directory beside it, because a worktree's `.git` files name
them absolutely. `scripts/smoke.py dist/<AppImage>` boots it like the nix build.

## Driving the game

For an agent or a script, without a person or a screen:

- **`--headless`**: SDL's dummy video and audio drivers and the null backend;
  the renderer keeps its state (texture store, target size) but draws nothing, the
  overlay loads settings and mods without Dear ImGui, frame smoothing is off.
  Runs inside `nix develop`, since nothing needs the host's EGL.
- **`--turbo`**: the game clock jumps to each wait's deadline instead of
  sleeping, so the game runs as fast as the CPU allows: about 100 game seconds
  per second headless (1,400 frames a second). Sound follows the game clock.
  With a window it also works, and draws every game frame.
- **`KF_AGENT=1`**: the beacon, `[KF1-AGENT] {...}` on stdout about once a
  real second: `context` (opening, gameplay, menu, scripted), `inGame`,
  `clockMs` (game clock), `frames`, and in game `hp`, `maxHp`, `mp`, `maxMp`,
  `area` (floor 1-5), `level`, `gold`, `pos` (camera position), `pitch`, `yaw`.
  The names follow `main`'s beacon.
- **`KF_SHELL=1`** (or a port): the command channel on `127.0.0.1:27901`, one
  request a line, one JSON line back, run on the game thread at the next host
  poll (a reply times out after 5 s if the game is not polling). `help`,
  `state`, `press <button>[+<button>...] [holdMs]` (the game's buttons: up,
  down, left, right, strafe_left, strafe_right, look_up, look_down, attack,
  magic, confirm, back, start, select; held for game-clock milliseconds, so a
  hold means the same under turbo), `release`, `quit`.

```bash
KF_AGENT=1 KF_SHELL=1 nix develop -c build/linux/verdite1 --data data/en \
  --saves /tmp/kf-saves --headless --turbo --skip-intro
```

`KF_TEST_STICK=<lx>,<ly>,<rx>,<ry>` holds the gamepad's sticks (-1..1, right
and down positive) with no pad connected. Checked 2026-10-23 from `--skip-intro`,
at 15 updates a second: the right stick full right turns 28 an update (the
D-pad's rate), half right 8.5 (the curve predicts 8.45), full down pitches 10 an
update (L2/R2's limit); the left stick forward walks about 181 an update, and
centred sticks move nothing.

`KF_TEST_CRASH=fault|abort|soft@<gameplay polls>` crashes the game once on purpose,
to check crash reports ([CRASH_REPORTS.md](CRASH_REPORTS.md)).

`KF_TEST_BACKGROUND=<polls>` sends Android's "going to the background" event once,
on any platform: the renderer drops its surface and makes it again at the next
present ([ANDROID_PLAN.md](ANDROID_PLAN.md) B2).

`--skip-intro` starts a new game straight away (area 1, HP 30/30, at
(31000, -11500, 4000)). Checked 2026-10-23: `press up 5000` walks to z 22302,
`press right 1000` turns, `quit` exits. Use a separate `--saves` folder; the
settings file is the player's, and is saved on exit.

Not yet: `main`'s `peek`/`poke`/`dump` (there is no guest RAM; the decomp's
named globals take their place, command by command), `warp`, `stick`, `view`.

## Fuzzing

`scripts/fuzz.py` plays the game many times at once, headless and seeded, and sorts what
went wrong by where it went wrong: `main`'s fuzzer (2026-10-21), carried 2026-10-25. The
plan and what comes next are [FUZZING_PLAN.md](FUZZING_PLAN.md).

```bash
nix develop -c cmake --build --preset linux        # fuzz.py refuses a stale build
python3 scripts/fuzz.py --runs 36                  # half the CPUs at a time, 20 game minutes each
python3 scripts/fuzz.py --budget 3600 --mix --vary # an hour of seeds, each started and set up differently
python3 scripts/fuzz.py --replay fuzz/runs/000117 [--windowed]
python3 scripts/fuzz.py --check-determinism --runs 6
python3 scripts/fuzz.py --compare-setting          # every visual setting must leave play unchanged
python3 scripts/fuzz.py --minimise fuzz/runs/000117
python3 scripts/fuzz.py --preset fuzz --runs 12    # under ASan, UBSan and checked containers
python3 scripts/fuzz.py --targets --seconds 600    # the libFuzzer targets (fuzz preset)
```

Every flag is in `python3 scripts/fuzz.py --help`; the switches they set, `KF_FUZZ_*` and
`KF_TRACE`, are in `port/include/kf/platform/fuzz.h`. It writes `fuzz/report.md` and
`report.json` (gitignored): outcome counts, a heading per signature with its seeds, the
command that reruns it and the one that replays it, and the floors, cells and items the runs
reached.

- **The pieces.** libverdite's `verdite/fuzz.h` is the policy (weighted presses held for a
  while, a release between them so each is an edge, a menu table, escape presses when no update
  comes), the macros, the invariants, the recording and the `[KF1-FUZZ] {...}` lines;
  `verdite/state_trace.h` hashes named state per update and compares two traces. KF1's half,
  `port/platform/fuzz.cpp`, is tables: the presses (main's weights, in 60 Hz ticks), the macros,
  the invariants and the trace regions. Game-code patches: `random_seed` and `random_peek`
  (`src/lib/random.cpp`), `host_between_updates()` in the main loop, and
  `host_allows_power_off()` in the system menu's Quit.
- **A run** is `verdite1 --headless --turbo --skip-intro` with `KF_FUZZ=<seed>` in its own
  directory under `fuzz/runs/`, with its own saves folder and its own user folder
  (`XDG_DATA_HOME`), so settings, crash reports and mods are the run's, not the player's. The
  seed seeds the policy and the game's `rand`. It is **crash** (a signal, an assertion or a
  sanitizer report; the signature is the crash report's top frames), **fail** (`host_fail`),
  **violation**, **timeout** or **ok**.
- **Macros**, a few per ten thousand updates, run between updates: `warp <floor>`
  (`player_warp_change_floor`, then a move to an open cell two updates later, since the warp
  keeps the cell), `goto <x> <z>` (an open cell from the automap's snapshot: floor with four
  floor neighbours; floor 5 by its own teleport, the others by moving the player as it does,
  since only floor 5 has `CHR<variant>.MIM`), `save` and `load` (the quick save, through
  `host_take_quick_request`), and, at the start, `kit` and `strong`. A run whose position has
  not moved for 600 updates is moved to an open cell.
- **Invariants**, between updates, resting 60 updates after a death, a floor change or a warp:
  `vitals` (HP and MP within their maxima, the maxima within 9999 and 999), `progress` (floor 1-5,
  highest floor at least the current, level non-zero), `position` (inside the grid), `cell`
  (`motion_state.map_cell` is the cell of the position), `in-rock` (the player's cell is not
  `KF_MAP_CELL_BLOCKED`), `items` (counts at most 99, the unused ids 0), `equipment`, `actors`
  and `effects` (every record pointer null or inside its table; each live actor's definition and
  tile in range), and `stall` (an update within 6000 ticks).
- **Exact replay.** Under turbo the game clock moves only with the game's waits and the game
  thread does all the work, so a run is a function of its seed. Checked 2026-10-25: seed 7 run
  twice, and its recording replayed, gave equal traces in every region for all 4,147 updates. So
  `--replay` reproduces a finding exactly, and `--minimise` cuts its recording down by delta
  debugging. A recording replays the build that made it; one made before a change to the game
  or the fuzzer may not.
- **Speed**: 48 runs of 72,000 ticks (16 game hours) took 307 s eight at a time
  (2026-10-25), about 1,400 ticks (300 updates) a second each.

**The system menu's Quit is answered No** under the fuzzer: Yes shows the power-off screen,
`menu_system_panel`'s loop that draws until the console is switched off, with no pad read and no
way back (the original's design). Seed 104 sat there for the rest of its run (a `stall`, its
stack taken with gdb from the exact replay, 2026-10-25). `main` found the same on its load screen.
The decline is a `note`, `quit-declined`.

**What it found** (2026-10-25):

- Only the fuzzer's own mistakes so far, each fixed: a `goto` by the same-floor teleport on floor
  1 failed on `B1/CHR0.MIM` (that teleport is floor 5's); a quick save between a floor warp and
  the move to an open cell saved the player in rock (`in-rock` after the load, seed 100 of
  `--mix --vary`).
- 48 runs of `--mix --vary` reached all five floors, 7,844 cells (floor 1: 2,016, 2: 1,550,
  3: 1,299, 4: 1,086, 5: 1,893) and 69 kinds of item; `main`'s first 12 barely left floor 1.
- The libFuzzer targets ran three minutes each under ASan, UBSan and `_GLIBCXX_ASSERTIONS`
  without a finding: `memory_card` (seeded with the C# build's `carda.sav`), `save_card` (with
  a quick save the fuzzer wrote), `disc_image` (the made-up test discs) and the decomp's own
  `codecs`.

## What changes in kind

- **No recompiler.** The game is native C/C++ from the decomp. Everything on
  `main` that exists to watch or replace recompiled MIPS (the `Literal` draw
  routines, the retained scene, the GPU census, VBlank and loop pacing, the
  dozens of probes, `PictureDepth`, `RetainedMap`, the fork patches) has no
  counterpart and is not carried.
- **No C#.** Verdite Core and the C# patches cannot be linked in. A feature
  is ported by re-implementing it against the decomp's named functions and
  globals. Hooks become plain calls at the right place in the source.
- **Mods are Lua** (`docs/MODDING.md`), not C# runtime mods. A Verdite debug
  tool that only reads and writes game state can be a Lua mod instead of code.
- **Settings are kf1-enhanced's** `settings.json` and F1 overlay, not `KF1_*`
  variables. Ported features add settings there; env switches only for
  diagnostics.

## Feature inventory

From `main`'s `docs/ENV_VARS.md` and `patches/`, against `enhanced` at the
branch point.

### Already in kf1-enhanced (compare, do not port)

| Verdite1 | kf1-enhanced | to check |
|---|---|---|
| widescreen, HUD/menu/tints widened, cull cone | aspect setting | the widened cull, tints at the edge. HUD at the screen edges done (2026-10-24): Display > *HUD at the screen edges* (`display.hud_at_edges`, on; off in Classic mode, dimmed at 4:3). `render_hud_sprites` and `render_hud_models` draw each record moved out by the widescreen columns, left of X 160 to the left and right of it to the right, by X and not the middle because the gauges change width; the records are not touched, so nothing needs putting back. At 16:9 (54 columns) the gauges land at -23, the attack/magic gauges at 75, the panels at -44 and 40, the compass dial at 323 and its needle at 340, as main's RAM dump had them. Not judged by eye. The menu's background tiled into the margin done (2026-10-24), as main's `Widescreen.BeforeMenuPresent`: `menu_enqueue_background` also submits each of its four 160 x 120 quads 320 columns left and right (and further while a copy still reaches the margin), skipping copies wholly outside the picture; at 4:3 and in Classic mode there are no columns and nothing changes. Every page's background goes through it, where main had to find four routines. Probed at 16:9 (54 columns): two copies a side, at X -160..0 and 320..480. Not judged by eye |
| frame pacing, view/model/HUD smoothing | frame smoothing | carried on Vulkan: models, poses, the arm, HUD bars, item display, compass (`RENDERER_PLAN.md`) |
| no dither, true colour, perspective, sub-pixel | `true_color`, `perspective`, `subpixel` | |
| Z-buffer | `depth_buffer` | |
| AO, per-pixel lighting | dynamic lights, AO, shadows, flame glow | different designs: judge, do not merge blindly. AO and the game's lighting per pixel are done on Vulkan (`RENDERER_PLAN.md`); dynamic lights, shadows and glow were drawn only by GL and are not drawn yet |
| mouse look, free look | mouse look, `look_limit` | |
| English translation | language switch, ppf/rar import | |
| keyboard layout | controls | |
| settings UI (Verdite Core) | F1 overlay | |

### Missing: port

| Verdite1 | where on `main` | notes |
|---|---|---|
| Discord Rich Presence, 41 place names | `DiscordIpc.cs`, `DiscordPresence.cs`, `DiscordPlaces.cs` | done (2026-10-23): `verdite/discord.h` and `verdite/places.h` in libverdite, the text and place table in `port/ext/discord.cpp`; the walkable cells come from the automap snapshot, the menu, shop and save screens from `kf/platform/screen.h`; no "Loading", which was guessed from a stalled tick |
| render distance + fade, models past the game's reach | `RenderDistance.cs`, `CullCone.cs` | the decomp's stencil/cell loop can be widened directly |
| distance fog, anisotropic, mipmaps, enhance distance | `KF1_FOG`, `KF1_ANISO`, `KF1_MIPMAPS`, `KF1_ENHANCEDIST` | done on Vulkan (2026-10-23, `RENDERER_PLAN.md`); enhanced distance leaves dynamic lights alone |
| analog tuning: curves, deadzones, ease, accel, tank, inverts | `Analog.cs` | done (2026-10-23): the shaping in libverdite (`verdite/stick.h`), the options and per-axis state in `port/platform/analog.cpp`, and a line in each of the player update's four velocity branches asking it for the velocity (`host_analog_velocity`), so nothing is pre-loaded to steer the game's own branch; centred, the D-pad's branches run. Twin stick by default; off in Classic mode. Not carried: `AnalogProbe` |
| pad layout `jp`/`west` | `PadLayout.cs` | done (2026-10-23): two face-button tables in `port/platform/host.cpp`, chosen by `controls.pad_layout` (West by default); only the gamepad's bindings change, so the keyboard, mouse and agent need no subtraction |
| enemy turn speed | Verdite2's `EnemyTurn.cs` (KF2, `81c02db` there) | done (2026-10-23), reworked for KF1: its enemies already turn 60-80 a tick (32, 48 and 10 for a few) against the D-pad's 28, so the turn rate alone was not what let a player circle them; the pursuit re-aims at the player on only one update in eight, and a retreat only as it starts. Gameplay > *Enemy turn speed* (x1-x4, x1.5 by default, x1 in Classic mode, `gameplay.enemy_turn`) multiplies the turn of a pursuit or a retreat, not a wander or the way home; above x1, *Enemies keep you in sight* (`gameplay.enemy_track`, on) re-aims both every update, the random draw kept so the sequence is the game's. Staged on floor 1 (an enemy of turn 80 put 2,500 ahead, the player strafing round it at 180 a tick, three runs each): at x1 it lost the player in every run and wandered off, with 4-5 melee attacks; at x1.5 with tracking it stayed at about 2,550 and started 8-10, at x2 7-15; x1.5 without tracking lost the player too. Not carried: V2's tracking while standing, since a KF1 enemy near the player is pursuing, not standing |
| rumble, gyro aim | Verdite Core `Rumble.cs`, `GyroAim.cs` | SDL3 has both |
| debug: noclip, god mode, infinite MP, peaceful, speed, warp | `Menu/`, `Warp.cs` | candidates for a Lua mod |
| message text (dialogue pictures as glyph text) | `Text/` | may be moot if the decomp's English resources are text already |
| crash reports, hang watchdog, focus pause | Verdite Core | crash reports done (2026-10-24, [CRASH_REPORTS.md](CRASH_REPORTS.md)): `verdite/crash.h` in libverdite, the stack named by a helper process with cpptrace, KF1's sections (player, the actor being updated, the last effect, settings), the last 64 events and copies of the saves from `port/platform/crash.cpp`. Not yet: hang watchdog, focus pause |
| modding API (Verdite's C# runtime mods) | Verdite Core | kf1-enhanced has Lua API 0.1 (player stats, keys, ImGui); 1.0 is planned in [MODDING.md](MODDING.md#the-plan-api-10): whole game reachable, sandboxed, host in libverdite |
| state beacon + command channel (port 27901) | `AgentBeacon.cs`, `AgentServer.cs` | what lets an agent drive and measure the game |
| fuzzer: seeded headless turbo play, invariants, report | `Fuzz.cs`, `scripts/fuzz.py` | done (2026-10-25, "Fuzzing" above): `verdite/fuzz.h` and `verdite/state_trace.h` in libverdite, KF1's tables in `port/platform/fuzz.cpp`, exact replay, a state trace, the `fuzz` preset and libFuzzer targets ([FUZZING_PLAN.md](FUZZING_PLAN.md)) |
| frame profiler | Verdite Core | |

### Not carried

Everything that measures or replaces recompiled code: `Literal`, `GpuWorld`,
`Retained*`, `*Probe`, `*Census`, `VBlankPacing`, `OpenPacing`, `LoopPacing`,
`FramelessWaits`, `PictureDepth`, `PacketMatch`, `ProfilerKnown`, `Prejit`.

## Open questions

- ~~Merge `sushi-shi/port` before porting, or stay on `enhanced`?~~ Merged:
  the pin moved to `sushi-shi/port`'s head on 2026-10-23.
- Whether `decomp` eventually replaces `main` or lives beside it.

Decided 2026-10-22: the platforms are Windows, Linux, macOS, Android, Switch
and 3DS, with Vulkan as the main renderer ([RENDERER_PLAN.md](RENDERER_PLAN.md)).
The branch is pushed to `origin/decomp`.

## Order

1. ~~Build and boot on Linux from the user's disc.~~ Done 2026-10-22. The
   user plays the baseline to judge it by eye.
2. ~~The holyland layout.~~ Done 2026-10-22. ~~Move the pin to
   `sushi-shi/port`'s head and bring `port/` up to its interfaces.~~ Done
   2026-10-23 (`7bd4567c`, 37 commits with the readability pass).
3. Infrastructure the rest leans on: ~~headless/turbo, state beacon + command
   channel~~ (done 2026-10-23, "Driving the game" above), ~~then the fuzzer~~ (done
   2026-10-25, "Fuzzing" above).
4. The renderer frontend and the Vulkan backend, in the order of
   [RENDERER_PLAN.md](RENDERER_PLAN.md). That also fixes the frame-rate gap
   ([RUNTIME.md](RUNTIME.md)).
5. User-facing features, cheapest first: ~~Discord~~ (done 2026-10-23), ~~pad layout~~ (done 2026-10-23), ~~analog tuning~~ (done 2026-10-23),
   debug mod (on the modding API's first two steps), render distance, picture options.
6. Extract the shared parts into the C++ core subtree. Then KF2 on the same
   core once its US decomp and port layer exist; KF3's decomp starts.
