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
[RUNTIME.md](RUNTIME.md).

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

### A C++ Verdite Core

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
- **Saves.** Players coming from a Verdite build want their saves. kf1-enhanced
  writes the game's own format (`.kfs`), so a memory-card importer is cheap.
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
- **The pin is `150ae354`**, the `sushi-shi/port` commit kf1-enhanced is based
  on, so the first layout reproduces kf1-enhanced exactly. The submodule's
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
`--disc <cue|iso>`, `--english-patch <ppf|rar>`, `--language ja|en`,
`--saves DIR`, `--skip-intro`, `--extract-only`. The disc is the one
`main` uses, `../verdite1/disc/KingsField1.cue`; it is already the English
v1.0 image (extraction verified 428 English files), so it went to `data/en`:

```bash
mkdir -p data && nix develop -c build/linux/kings-field \
  --disc ../verdite1/disc/KingsField1.cue --extract-to data/en --extract-only
```

Run with `scripts/run-linux.sh`, not inside `nix develop`: nix's libglvnd
does not see Fedora's Mesa ("Could not get EGL display"), so the script points
`__EGL_VENDOR_LIBRARY_DIRS` at `/usr/share/glvnd/egl_vendor.d` and puts
`/usr/lib64` on the library path. With that it booted (2026-10-22): 960x720
target, overlay ready, "Starting King's Field (English v1.0)". User files go
to `~/.local/share/KingsField/SLPS00017/`. The `offscreen` SDL driver finds no
EGL device even so, so there is no headless run yet.

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
| widescreen, HUD/menu/tints widened, cull cone | aspect setting | the widened cull, tints at the edge |
| frame pacing, view/model/HUD smoothing | frame smoothing | what is carried between ticks (models? HUD needle?) |
| no dither, true colour, perspective, sub-pixel | `true_color`, `perspective`, `subpixel` | |
| Z-buffer | `depth_buffer` | |
| AO, per-pixel lighting | dynamic lights, AO, shadows, flame glow | different designs: judge, do not merge blindly |
| mouse look, free look | mouse look, `look_limit` | |
| English translation | language switch, ppf/rar import | |
| keyboard layout | controls | |
| settings UI (Verdite Core) | F1 overlay | |

### Missing: port

| Verdite1 | where on `main` | notes |
|---|---|---|
| Discord Rich Presence, 41 place names | `DiscordIpc.cs`, `DiscordPresence.cs`, `DiscordPlaces.cs` | IPC over the Discord socket; place table is data, carries as is; collision grid `0x80098018` is a named global in the decomp |
| render distance + fade, models past the game's reach | `RenderDistance.cs`, `CullCone.cs` | the decomp's stencil/cell loop can be widened directly |
| distance fog, anisotropic, mipmaps, enhance distance | `KF1_FOG`, `KF1_ANISO`, `KF1_MIPMAPS`, `KF1_ENHANCEDIST` | in `src/renderer/` |
| analog tuning: curves, deadzones, ease, accel, tank, inverts | `Analog.cs` | kf1-enhanced has one deadzone |
| pad layout `jp`/`west` | `PadLayout.cs` | |
| rumble, gyro aim | Verdite Core `Rumble.cs`, `GyroAim.cs` | SDL3 has both |
| debug: noclip, god mode, infinite MP, peaceful, speed, warp | `Menu/`, `Warp.cs` | candidates for a Lua mod |
| message text (dialogue pictures as glyph text) | `Text/` | may be moot if the decomp's English resources are text already |
| crash reports, hang watchdog, focus pause | Verdite Core | |
| state beacon + command channel (port 27901) | `AgentBeacon.cs`, `AgentServer.cs` | what lets an agent drive and measure the game |
| fuzzer: seeded headless turbo play, invariants, report | `Fuzz.cs`, `scripts/fuzz.py` | needs a headless, uncapped mode first |
| frame profiler | Verdite Core | |

### Not carried

Everything that measures or replaces recompiled code: `Literal`, `GpuWorld`,
`Retained*`, `*Probe`, `*Census`, `VBlankPacing`, `OpenPacing`, `LoopPacing`,
`FramelessWaits`, `PictureDepth`, `PacketMatch`, `ProfilerKnown`, `Prejit`.

## Open questions

- Merge `sushi-shi/port` (37 commits, incl. readability pass and typed menu
  outcomes) into this branch before porting, or stay on `enhanced` and follow
  acranon? Merging later gets harder as our changes grow.
- Whether `decomp` eventually replaces `main` or lives beside it.

Decided 2026-10-22: the platforms are Windows, Linux, macOS, Android, Switch
and 3DS, with Vulkan as the main renderer ([RENDERER_PLAN.md](RENDERER_PLAN.md)).
The branch is pushed to `origin/decomp`.

## Order

1. ~~Build and boot on Linux from the user's disc.~~ Done 2026-10-22. The
   user plays the baseline to judge it by eye.
2. ~~The holyland layout.~~ Done 2026-10-22. Next: move the pin to
   `sushi-shi/port`'s head (37 commits, the readability pass among them) with
   `patches.py upgrade`, and bring `port/` up to its interfaces.
3. Infrastructure the rest leans on: headless/turbo, state beacon + command
   channel, then the fuzzer.
4. The renderer frontend and the Vulkan backend, in the order of
   [RENDERER_PLAN.md](RENDERER_PLAN.md). That also fixes the frame-rate gap
   ([RUNTIME.md](RUNTIME.md)).
5. User-facing features, cheapest first: Discord, pad layout, analog tuning,
   debug mod, render distance, picture options.
6. Extract the shared parts into the C++ core subtree. Then KF2 on the same
   core once its US decomp and port layer exist; KF3's decomp starts.
