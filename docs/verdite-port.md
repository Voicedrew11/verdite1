# Verdite1 on the decomp

Branch `decomp` of `Voicedrew11/verdite1`: Verdite1's features carried onto
[acranon/kf1-enhanced](https://github.com/acranon/kf1-enhanced) (`enhanced`),
which is built on [sushi-shi/kings-field-decomp](https://github.com/sushi-shi/kings-field-decomp)
(`port`). All three are CC0 or ours. Started 2026-10-22.

`main` is the RecompOne recompilation and shares no history with this branch.
Nothing from `main` is merged; features are rewritten here in C/C++.

## Remotes

| remote | repo | branch we follow |
|---|---|---|
| `origin` | Voicedrew11/verdite1 | `decomp` (this) |
| `kf1-enhanced` | acranon/kf1-enhanced | `enhanced` (base, `ac147450`) |
| `sushi-shi` | sushi-shi/kings-field-decomp | `port` |

At the branch point `enhanced` was 23 commits ahead of `sushi-shi/port` and 37
behind it. Nothing goes upstream to either: no pull requests, no issues.

## Build and run (Linux)

SDL3 is not a system package here; the flake supplies the toolchain.

```bash
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
- Target: Linux first (where it is run here), Windows via the `windows` preset
  as kf1-enhanced ships?
- Whether `decomp` eventually replaces `main` or lives beside it.

## Order

1. ~~Build and boot on Linux from the user's disc.~~ Done 2026-10-22. The
   user plays the baseline to judge it by eye.
2. Decide the questions above.
3. Infrastructure the rest leans on: headless/turbo, state beacon + command
   channel, then the fuzzer.
4. User-facing features, cheapest first: Discord, pad layout, analog tuning,
   debug mod, render distance, picture options.
