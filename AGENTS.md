# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A static recompilation of **King's Field (NTSC-U, `SLUS-00158`)** — the North
American release of the Japanese *King's Field II* (`SLPS-00069`) — using
[RecompOne](https://github.com/BlackLabelHQ/RecompOne). The series was renumbered
for the West: the US-boxed "King's Field II" (`SLUS-00255`) is a *different game*
and every address here is wrong for it.

There is no decompilation, no ELF and no `.map`. Function boundaries come from a
linear sweep and PSY-Q library functions are identified by hand, so most work in
this repo is *reverse engineering*, not application coding: find an SDK function's
address in the disc image, map it to the runtime's HLE implementation, re-run the
recompiler, run the game, read the logs.

**`NOTES.md` is the index, and `docs/` is the working log.** `NOTES.md` carries
what the project is and where it stands, plus a map of the documents under
`docs/` and the exact section titles in each. Read the index before starting
anything, then the one or two documents your task touches — they are split by what
you would be doing when you need them:

| file | when |
|---|---|
| `docs/DEVELOPMENT.md` | build, run, diagnose, measure |
| `docs/ENV_VARS.md` | every `KF2_*` switch, in one list |
| `docs/RECOMPILATION.md` | config, overlays, function maps, SDK addresses |
| `docs/RUNTIME.md` | interrupts, HLE, the `patches/recompone/` stack |
| `docs/RECOMPONE_FORK.md` | the vendored checkout, and merging from upstream |
| `docs/RECOMPONE_PATCHES.md` | every change the port made to RecompOne, `0001`-`0079` |
| `docs/RENDERING.md` | perspective correction, sub-pixel, Z-buffer, dither |
| `docs/WIDESCREEN.md` | aspect ratio, the HUD, the three culls |
| `docs/AUDIO.md` | SPU interpolation, reverb, XA resampling, the host output |
| `docs/GAME_INTERNALS.md` | the game's own addresses and routines |
| `docs/PATCHES_AND_MODS.md` | hooking, settings UI, frame pacing, smoothing, the map |
| `docs/INPUT.md` | pad, sticks, keyboard, mouse, the menu pointer |
| `docs/PACKAGING.md` | the redistributable: the launcher, the first-run build, CI |
| `docs/TODO.md` | next steps and open, undiagnosed questions |

Update the right document when you learn something — that is where findings
belong, not in commit messages, and not in this file. Source comments still say
`See "X" in NOTES.md`, and **the text they name is not in `NOTES.md` any more** —
the titles are unchanged, so the index resolves X to a document, but it is a hop
rather than a direct hit. Grep `docs/` for the title, not `NOTES.md`.

## Build and run

Nothing here builds without the disc (gitignored, `disc/KingsField2.cue`).
`tools/RecompOne` is **vendored** — its sources are tracked here, so a fresh
clone already has it and nothing needs cloning.

```bash
bash scripts/setup_tools.sh          # build the vendored recompiler

# recompile MIPS -> C# into generated/ (~2234 functions, ~182k lines)
dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build -- config/kf2.json

dotnet build KingsField2Recomp.csproj -c Release
dotnet run --project KingsField2Recomp.csproj -- disc/KingsField2.cue
```

`setup_tools.sh` builds; `--sync-upstream` starts the next three-way merge from
upstream, and `--signatures` fetches the 15.7 MB PSY-Q bank (gitignored, read
only by the standalone `--autoconfigure`). The cue path is needed at *play* time
as well as at recompile time.

There are no tests. Verification is empirical: run the game with log channels on
and check the trace against what the SDK sequence should look like (see the
steady-state `KF2_LOG=sdk` excerpt under "Status" in `NOTES.md`).

**Anything that has to be judged by eye is the user's job, not yours.** Do not
try to capture, screenshot or otherwise scrape the game window — it burns a lot
of context and produces nothing a person could not tell you in one sentence.
Measure what a counter can measure, then say plainly what still needs looking at
and ask. That distinction is already all over `docs/`, which repeatedly
separates "mechanism measured" from "picture never checked"; keep writing it down
that way.

### Diagnostics

The switches used most; the full list is `docs/ENV_VARS.md`, imported here.

@docs/ENV_VARS.md

```bash
KF2_LOG=bios,cd,gpu,dma,sdk,spu,mdec  # or KF2_LOG=all; wired up in Program.cs
KF2_CDTRACE=1                          # stack trace on first CD register access (patch 0002)
KF2_AUTOPAD=8:Start:400,20:Circle:200  # scripted pad input: seconds:button:holdMs
KF2_FPS=120                            # 60 (default), any number, or off
KF2_FPS_PROBE=1                        # a line a second: fps drawn, ticks taken, what each smoother is doing
KF2_TICKRATE=30                        # the world's tick rate (20, and not a setting); a comparison only
KF2_PRESENT_PROBE=1                    # proves frames reach the screen; prints nothing if they do not
KF2_AUTOSTART=2                        # boot straight into save slot 1..3, past the title menus
KF2_AGENT=1                            # [KF2-AGENT] state lines on stdout
KF2_SHELL=1                            # TCP 127.0.0.1:27900 command channel
KF2_BOOTEXE=end                        # boot straight into OPEN.EXE, GAME.EXE or END.EXE
```

Prefer the mods under `mods/` (**enable them in the game's Mods panel** — mods
default to off and load silently when disabled) to `KF2_LOG=sdk`, which is
gigabytes a minute. `KF2_LOG=bios` is very expensive during play — the game polls
`PAD_dr` hundreds of thousands of times a second.

`Program.cs` is hand-owned (RecompOne would otherwise generate one into
`generated/`); add new env-var-driven diagnostics there, and add them to
`docs/ENV_VARS.md`. The docs mention `KF2_TRACECALL` — that was an ad-hoc local
edit to the dispatcher and is *not* in any committed patch; re-add it by hand if
you need indirect-call tracing.

**For a hang, take the managed stack of the live process instead of adding
logging.** Recompiled functions carry their MIPS address in their name, so the
trace names the routine directly:

```bash
~/.dotnet/tools/dotnet-stack report -p $(pgrep -f net10.0/KingsField2)
```

Start the game from the same shell you run that in, or the diagnostic socket in
`TMPDIR` will not be found.

### Driving the game without a human

- **The attract demo is a free live session**: leave the port at the title and it
  walks itself into an area about a minute later, with a character, an HP bar and
  (eventually) a death. `AutoReload.Simulate()` kills on demand from there, and the
  death clock at `0x8019951A` can be pinned to hold any frame of the death sequence.
- **`KF2_AUTOSTART=<1..3>`** — an agent left at the title waits forever (the boot
  menus take no input by the usual routes, and `KF2_AUTOPAD` only arms once an area
  has loaded). This drives the pad through `PAD_dr`: Start, Cross into a New Game
  in `fdat02`, then loads the slot over it through `AutoReload.LoadSlot`.
  `KF2_AUTOSTART=new` stops in that New Game, which faces scrolling water.
- **`KF2_AGENT=1`** prints `{"overlay":…,"inGame":…,"hp":…,"area":…,"slot":…}` on
  each overlay change and about once a second; `inGame:false` is how a program
  tells "stuck at the title" from "in an area" without a screenshot.
- **`KF2_SHELL=1`** — one request per line on TCP 127.0.0.1:27900, one
  single-line JSON response back: `state`, `nearby`, `load <slot>`,
  `warp <area>`, `press <button> [ms]`, `kill`, `ending [boss|kill]`,
  `map [on|off|toggle]`, `waves [on|off|<setting> <value>]`, `goto <x> <y> <z> [yaw [pitch]]`, `view [<x> <y> <z> <pitch> <yaw> <roll> | off]` (the frame's camera and a cull-grid digest; with a camera, draw from it), and the remaster editor's `edit`, `select`, `set`, `pack`, `remaster`, `textures`, plus `snap [hash|PATH.png]`, which reads the presented picture back and hashes it (see `help`). The `kf2` MCP server in `mcp/` exposes the same channel.
  `ending kill` is the form that reproduces the final-boss crash; reaching it
  needs `KF2_DEBUG_GODMODE=1`, or `warp 7` kills the player on the way in.
  `press` reaches Cross but not the in-game menu's Up/Down.
- **`KF2_AUTOPAD`**'s clock starts when the first area module loads, the only
  point in the boot sequence that reliably means "in game".

See "Auto start and the agent beacon" and "The command channel" in
`docs/PATCHES_AND_MODS.md`.

## The port's own patches

Everything under `patches/*.cs` attaches at run time through `HookManager`; each
has a write-up, and the specifics (addresses, measurements, why each default is
what it is) live there, not here.

| patch | what | default | doc, section |
|---|---|---|---|
| `FramePacing` | skips the game's frame gate `func_80017880`, paces frames itself, runs the gated stages on a 20 Hz world clock | 60 fps drawn, 20 ticks/s | PATCHES_AND_MODS, "Any frame rate" |
| `FrameSmoothing`, `ObjectSmoothing`, `AnimSmoothing`, `FluidSmoothing` | carry the camera (and the compass needle and the HP/MP gauges with it, bracketing the HUD builder), the four world tables, MO pose and the scrolling textures between ticks | on; one checkbox | PATCHES_AND_MODS, "One switch for all of the smoothing", "The compass is carried with the view", "The gauges are carried like the needle" |
| `LoopPacing` | modal loops (fades, cutscenes, item/spell animations) run once per tick, gaps filled with stage-13 redraws | on | PATCHES_AND_MODS, "Loops that render their own frames" |
| `MenuPacing` | menu cursor repeat and blink held to the 60 Hz grid | on | PATCHES_AND_MODS, "The menu's cursor repeat" |
| `MenuWorld` | replaces the menu presenter `func_800226A8` and the message fade `func_800356F4`: the world is redrawn live behind menus, shops, signs and dialogue (full width, AO, Z) instead of the frozen 320-wide copy | on | PATCHES_AND_MODS, "Menus draw the world live", "Messages draw the world live" |
| `MessageText` | sign and dialogue text decoded from the message TIM in RAM (`patches/MessageGlyphs.cs`, built by `scripts/msg_glyphs.py`) and drawn as text on an opaque box; the TIM's palette is zeroed so the game's text quads draw nothing | off (judged: the mixed look is unwanted) | PATCHES_AND_MODS, "Drawing message text" |
| `LoadPacing` | loading screen's walking figure held to the vblank grid | on | PATCHES_AND_MODS, "The loading screen's walking figure" |
| `SpriteAnim` | billboard cel animation held to the tick | on | PATCHES_AND_MODS, "The flames run at the render rate" |
| `TintHold` | stage 1 `func_8002C944` in C#: it clears the screen tint every frame while the stages that set it are gated, so the reset now runs only on a tick and the death fade and damage flash stop strobing; `KF2_TINTHOLD=verify` diffs it against the recompiled routine | on | PATCHES_AND_MODS, "The tints strobed between ticks" |
| `FullRateLogic` | `KF2_FPS_LOGIC=full`; comparison only, **not shippable** | off | PATCHES_AND_MODS, "Any frame rate" |
| `Prejit` | compiles the recompiled code, the patches and the runtime on a background thread at boot, so walking into an area does not stop to JIT it (QuickJit is off, so a first call is a full JIT: 234 methods and 297.87 ms in one frame without it) | on | DEVELOPMENT, "The first frame of an area was the JIT" |
| `FrameProfiler` | per-frame time by section: every hook, the present path, the waits (`0045`); Shift+P | records while its panel is open | DEVELOPMENT, "Profiling a frame" |
| `FrameCapture`, `FrameViewerPanel` | capture one run of stage 13 and scrub it GP0 command by command on a detached software GPU: owner routine, send cost, fragments, GL batch submits and why, GPU time per batch and for AO, every runtime section, vertex-map work per routine (`0046`); Shift+F | idle until a capture; routines hooked from the first | DEVELOPMENT, "Watching a frame being built" |
| `PolyAssembler` | `func_80030540` in C# as a replace hook, rejecting polygons the view-space clipper would clip to nothing; also `func_8002FECC` (the far map tiles' unclipped assembler), the vertex transforms `func_8002E650`/`func_8002E7CC` and the HUD's `func_8002E910` (orthographic, so it publishes each vertex's fraction to the vertex map itself, and only for pieces the matrix turns), `func_8002F214`/`func_8002EAEC` (the models' lit assembler) and the clipper `Clip4FTP`/`Clip3FTP`; the GTE ops they call have a fast path in the runtime (`0047`); `KF2_POLYASM=verify` diffs each against the recompiled routine, GTE included; a clipped polygon or quad is culled on its whole area at the fractional corners, not its first three corners, through a hook on `NormalClip` (`KF2_POLYASM_FACING=0` to compare); Video ▸ Fast geometry switches them all, with the GTE fast path | on | PATCHES_AND_MODS, "The polygon assembler in C#", "The lit model assembler", "The HUD's transform in C#", "The clipper in C#", "The GTE fast path"; RENDERING, "A floor quarter missing at a short edge", "An edge-on wall lost its strips" |
| `TileWalk` | the map tile walk in C#: `func_80031C94` (the 24×24 cell sweep), `func_80031B1C` (a cell's two halves) and `func_80031950` (a half, set up and assembled). Taken for the scene it enumerates, not for time (0.013 ms a frame); `KF2_TILEWALK=verify` diffs each against the recompiled routine | on | PATCHES_AND_MODS, "The map tile walk in C#" |
| `ModelWalk` | the object and creature walk in C#: `func_800331B4` (the creature, object, effect and billboard tables) and `func_80032588` (the model submitter). Taken for the scene it enumerates, not for time (3 us a frame); `ModelWalk.Scene` publishes each submit's record, model, position and assembler; `KF2_MODELWALK=verify` diffs both against the recompiled routines | on | PATCHES_AND_MODS, "The object and creature walk in C#" |
| `Stage13`, `CameraBlock` | stage 13 `func_800342D8` and its camera block `func_8002E22C` in C#: nineteen calls, each through its hooks, and the HUD block, with the compass needle's spring at `0x8006E608` stepped on the tick (`KF2_STAGE13_NEEDLE=0` to compare); `Stage13.HookOrder` orders the hooks on it (`0070`); `Stage13.ViewOverride` draws the frame from a `Camera` of the port's, the cull grid following; `Stage13.DrawScene` is the drawing half (`MenuWorld`); `CameraBlock.Build` (`PlanarWalk`); `ScenePass` points the frame at a table and arena of the port's and puts everything back (both); `KF2_STAGE13=verify` records the recompiled routine's calls and replays ours against them, `KF2_CAMERABLOCK=verify` diffs both | on; no override | PATCHES_AND_MODS, "Stage 13 in C#", "Drawing the frame from another camera", "The compass needle is held to the tick", "The hooks on stage 13 are ordered by what they need", "A pass of the port's own"; GAME_INTERNALS, "Stage 13's HUD block, and the compass needle" |
| `Perspective` | perspective-correct textures (`0009`, `0012`) | on | RENDERING, "Perspective correction" |
| `Subpixel` | sub-pixel vertex positions (`0010`); under it, the C# assemblers' backface cull is taken at the fractional corners (`0052`, `KF2_SUBPIXEL_CULL=0` to compare) | on | RENDERING, "Sub-pixel vertex positioning", "A thin face was culled on whole pixels" |
| `ZBuffer` | per-pixel occlusion; depth from the C# assemblers' packet records (`0050`), coplanar tolerance on the test (`0051`), the address map without Fast geometry (`0014`, `0036`); blended surfaces drawn after the opaque ones the table put behind them, so a fish or the floor under the water no longer paints over it (`0079`, `KF2_BLENDORDER=0` to compare); Video ▸ Enhancements, with two tolerance sliders | on | RENDERING, "Z-buffer", "The assemblers write the depth", "Water was painted over by what lay under it" |
| `Pgxp` | upstream's PGXP as the vertex source (`0034`-`0036`); env only | off | RENDERING, "PGXP has no control in the window" |
| `AmbientOcclusion` | SSAO from painter's-order depth (`0040`), normals from the frame's own geometry redrawn into a G-buffer (`0058`); optional world-space term marching the area's tile grid, so off-screen geometry occludes (`0059`, off); the *SSAO* slider is Off/Low/Medium/High, and the three qualities cap the pass at 1x/2x the game's pixels or runs it at the render scale | on, Medium | RENDERING, "Ambient occlusion", "The normal was the guess", "Occluders the camera cannot see", "What the pass costs" |
| `Reflections` | screen-space reflections on water (`0067`): the normal pass gains a surface buffer (normal, depth, material per pixel) that keeps the translucent water the depth buffer cannot; water found by the fluid slots' VRAM rects, translucent in an averaging blend; the material id is there for lighting later; the pass runs for any of the march, `Murk`, `PlanarWalk` or `RetainedMap`, each on its own switch | off (not judged) | RENDERING, "Screen-space reflections", "The reflection pass runs for each term on its own" |
| `Murk` | murky water: the reflection pass darkens water by the view ray's run through it to the floor (`WaterMurk`, `0067` amended); needs no reflection on | off (not judged) | RENDERING, "Murky water" |
| `Waves`, `WaterSwell` | water waves: a swell moves the water's interior vertices (the tile walk points each water mesh's bank header at a moved copy in `PrimBuffer.WaveScratch`, so the transforms, clipper and subdivider all read it; rims and vertices shared with other geometry held), and ripples push and shade the water's texture per pixel from a world-space wave field (`0078`, `WaterWaves`); one world clock; six sliders and the `waves` shell verb | off (not judged) | RENDERING, "Water waves" |
| `PlanarWalk` | planar reflections (`0068`): after the object walk, the tile walk runs again and the walk's model submits are replayed from a camera mirrored in the water, into an arena and ordering table `PrimBuffer` keeps past its buffers; drawn at the frame's `DrawOTag` into a planar texture per target, fragments under the water discarded; the reflection pass takes it where a surface lies on the plane and marches elsewhere if `Reflections` is on; the plane is binned from the water the backend classifies | off (not judged) | RENDERING, "Planar reflections" |
| `RetainedMap`, `RetainedPlanes`, `RetainedModels` | the retained scene (`0072`): the map built into world-space triangles on the GPU from the map data (each corner within 1 px of the GTE's own), the object walk's models captured each frame before culling, drawn at present for the presented frame through `WorldVs` in front of the unchanged `PrimFs`: up to four planes (water, authored reflective floors, ranked on the frame's own camera) mirrored straight into one planar texture, and a camera cubemap with depth marched in world space in place of the screen march; chunk-culled per view; `KF2_RETAINED_PROBE=1` the check, the planes, GPU time and the planar-vs-cubemap agreement | off (not judged); stands the planar walk down | RENDERING, "The retained scene: the world kept on the GPU, so a reflection can draw it again" |
| `Remaster.*` (`patches/remaster/`) | authored data from a pack: area identity and fingerprint, the working pack (JSON, watched, undo), a material per tile half written into the packet's depth record for the reflection pass, the editor (Shift+E, pauses the world, picks the faces under a click from the frame's own triangles, or a whole half from the docked map); materials per face of a half or of a mesh area-wide, gated on a hash of the mesh; authored point and spot lights added to the lit colour in `shade8` before the depth cue (`0071`, `RemasterUniforms`; needs per-pixel lighting and Fast geometry; `KF2_REMASTER_LIGHTS=0`), placed at the eye or on a click, dragged over the picture; materials on a model wherever the area draws it (kind and id, picked from the frame), with roughness (a blur of the reflection, from a mip chain), metalness, a highlight, an occlusion strength, and a glow, added over the texture by default, pulsing if asked, and giving off a light of its own that leaves its material alone, from a 256-id table on the GPU (`0067`, `0071` amended); the `edit`/`select`/`set`/`pack`/`remaster`/`light` shell verbs; upstream's texture packs on the port's path (`0073`): a key per uploaded image, filtered by the port's slider, and the `textures` census verb (`KF2_TEXCENSUS=1`); materials by texture, in every area (`TextureKeys`, `remaster/textures.json`, the least specific rule; a scrolling texture keyed on its source image), set from the editor's pick or `set texture material NAME`; `snap` hashes the presented picture (`0069`); the game's own light records (back colour, the three lights, the fog word) overridden after stage 1's copy, gated on each record's hash (`Atmosphere`, `atmosphere.json`, the `atmos` verb, `KF2_REMASTER_ATMOS=0`), once a census (`KF2_LIGHTCENSUS=1`) showed only the renderer reads them, and the area's *Darkness* slider over them (`"record": "all"`, `"darkness"`: a scale on records 0-63's back colour and light colours, computed from the source every pass; `atmos darkness`); shadows from the authored lights (`0077`): a depth cubemap per light, up to four, drawn from the retained map with the light at its centre, again only when the light or the map changes, sampled in `authored()` along the receiving surface (`KF2_REMASTER_SHADOWS=0`, `light shadows tune`); creatures and objects cast too, the frame's captured models drawn over a copy of the map's cubemap, again only while one in reach moves, a blended face with its opaque texels, effects not at all (`KF2_REMASTER_SHADOW_MODELS=0`, `light shadows models off`); the area's fog colour and curve (`fogColour`, `fogPower`, `fogMax` on the `"all"` entry), added past the texture by the packet's own depth-cue weight so a surface fades into the colour instead of black (`0074`; needs per-pixel lighting and Fast geometry; a face fogged to black keeps its record while one is set), and a `sky` the game's own background clear draws, the fog's colour by default (a pre and post on `PutDrawEnv`; `atmos fog|curve|sky`); Video ▸ Enhancements ▸ Remaster packs, with the packs listed under their own heading | off; nothing authored | REMASTER, "Phase 1, the first slice", "Phase 1, the second slice", "Faces, picked from the frame", "Phase 2, the first slice", "Phase 3, the first slice", "The glow is a light source", "Phase 3, the second slice", "Phase 4, the first slice", "Phase 4, the second slice", "Metal is a tinted mirror", "The light records are read only by the renderer", "Phase 5, the first slice", "The area's darkness", "Shadows, the first slice", "Shadows, the second slice", "Phase 5, the second slice" |
| `Anisotropic` | post-CLUT footprint supersampling (`0041`), every tap held inside the polygon's texture rectangle; one *Texture filtering* slider, Off / Trilinear / 2x-16x, every position past Off with mipmaps: minified textures decoded into an atlas with a mip chain each (`0060`, `KF2_MIPMAPS`) | 16x, mipmaps on | RENDERING, "Anisotropic filtering", "Mipmaps where the texture is decoded" |
| `PerPixelLighting` | the depth cue and the models' light evaluated per pixel from what `PolyAssembler` recorded per packet (`0048`) | on | RENDERING, "Per-pixel lighting" |
| `EvenFog` | clipped map tiles refogged on the tiles' curve instead of `func_800302E8`'s `IR0 >> 1`, and each tile vertex's fog blended between the light records of the tiles around it (hooks `func_80031950`; `0049`); and the records' colour matrix and back colour the same way; one *Even fog and lighting* checkbox, dimmed without Fast geometry (`KF2_EVENFOG_BLEND=0`, `KF2_EVENLIGHT=0` drop a part); stands down under verify | on | RENDERING, "A clipped tile is fogged at half, and that is the block on the floor", "Fog changes at a tile edge" |
| `NoDither`, `TrueColor` | one *Shading* slider: Dither / None / Smooth (24-bit, `0021`) | Smooth | PATCHES_AND_MODS, "Two shading checkboxes were one question asked twice" |
| `Widescreen`, `CullCone` | aspect ratio; widened view cone and screen tints follow it | 16:9 | WIDESCREEN, "Widescreen", "The cull the margin runs into" |
| `PrimBuffer` | the frame's primitive buffers moved above 2 MB into 4 MB of guest RAM, 4× as large, so a wide view no longer runs out and drops geometry (`0056`); `KF2_PRIMBUF=1` is the comparison, `KF2_PRIMBUF_PROBE=1` the measurement | on | WIDESCREEN, "The primitive buffer ran out" |
| `AutoReload` | reload the last save on death, fixed 2 s delay | on | PATCHES_AND_MODS, "Auto reload" |
| `Map*` | full-screen map (touchpad / `M`), minimap (`N`), fog of war, markers; full map pauses the world | map on; fog on; minimap, markers off | PATCHES_AND_MODS, "A dynamic map", "What the Map page is down to" |
| `Analog` | twin-stick control | on | INPUT, "Analog twin-stick control" |
| `Mouse` | mouse look, spent inside `Analog.BeforeLook` | on | INPUT, "Mouse look" |
| `MenuMouse` | point-and-click in the in-game menus | on | INPUT, "The menu pointer" |
| `KeyLayout` | the port's WASD layout | on | INPUT, "The keyboard layout" |
| `CardIcon`, `DesktopEntry` | the window icon is the game's own memory-card icon, read off the player's disc at boot; the shipped orb is the fallback. On Linux the same pixels go into the icon theme under the app id, which is the only way a Wayland compositor can show one (`0061`) | on | PACKAGING, "The icon comes off the disc", "Wayland takes the icon from the desktop entry" |
| `EndingHold`, `BootExe` | hold "The End", any button returns to the title | on | RUNTIME, "The ending screen" |
| `HitGuard` | fences the final-boss hit-path fault | on | TODO, "The crash on the final boss's last hit" |
| `AudioQuality`, `AudioProbe` | voice interpolation and reverb (`0043`); probe and WAV dump | sinc, enhanced reverb | AUDIO, "Voice interpolation", "An enhanced reverb sized from the game's registers" |
| `PositionalAudio` | 3D sound effects re-aimed every frame, for speakers or headphones (`0044`) | headphones | AUDIO, "Positional audio" |

Features ship **off** when the mechanism is measured but the picture has never been
judged by eye; say which of the two a change has when you write it up.

### Rules that bite when you change a patch

- **Attach through `patches/HookAttach.cs`, and read back what committed.**
  `AddPre`/`AddPost` only queue a delegate; the detour is made later in
  `HookManager.Commit`, which fails per function without throwing (`0027`), so
  counting `Add*` returns claims what was *queued*. Use `HookManager.IsCommitted`
  (`0028`), retry until it holds, and never latch `attached = true` before
  `Attach()` — an exception thrown inside an `OverlayLoadedEvent` listener is
  swallowed by `Event.Dispatch` into one stderr line. See "A registration is not
  a hook" in `docs/PATCHES_AND_MODS.md`.
- **The frame boundary is a single point of failure and it fails open.** It is a
  `DrawOTag` that follows a `VSync` call; `_tickThisFrame` starts `true`, so a lost
  boundary uncaps the picture *and* runs the whole world at the render rate,
  silently. The stage gate's 500 ms watchdog (`FallbackTick`) is what saves it; a
  hold keyed on the frame (like `SpriteAnim`) fails *closed* and needs the same
  watchdog. Check any pacing change with `KF2_FPS=144 KF2_FPS_PROBE=1`: 144.0 fps
  drawn at 20.0 ticks/s.
- **A gated stage must not draw** (`scripts/check_gate.py`; its `KNOWN` holds the
  two recorded exceptions). Something stepped inside a drawing function's own body
  cannot be gated and needs a hold/restore pair instead.
- **A tick is a frame identity, not a flag**: use `FramePacing.FirstWalkOfTick`,
  not `TickedThisFrame`, when something must happen once per tick inside stage 13.
- **`Widescreen` owns the one `Replace` of `DrawOTag`**, so every other `DrawOTag`
  hook must be a pre or a post, and a replacement must pass the source address to
  `WriteGp0` or perspective correction silently turns off.
- **A hook whose place among the others matters declares it** (`order` on
  `AddPre`/`AddPost`, `0070`), never by where its `Install()` sits in `Program.cs`.
  On stage 13 the orders are `Stage13.HookOrder`: `LoopPacing`'s redraw post is
  `Redraw`, after every smoother's restore.
- **A pass that draws into a table of the port's own goes through `ScenePass`**, so
  it puts back everything any pass moves.
- **A liveness test is the renderer's, not the owning stage's**: an object is drawn
  when `u16[+0x6] != 0xFF`, a creature when `u8[+0x9] == 1`.
- **Settings**: a page registers against a runtime section with
  `PatchSettings.Register("display", ...)` (or `RegisterSlot`, `0013`); pages
  sharing a `Title` share a heading and need adjacent `IPatchPage.Order`s or the
  heading is drawn twice. `Extend` has **no un-extend**, so nothing may register
  against `"input"` — `patches/settings/InputSection.cs` owns that pane outright
  and `Register` refuses the id. `gameplay` is the port's own section
  (`GameplaySection.cs`); **a new localisation key must supply all three of the
  runtime's languages** (en, pt-BR, es-419), and **a new sidebar entry must not
  collide with an existing one in any of them** (`Controles` already exists in two).
  See "Patch settings" in `docs/PATCHES_AND_MODS.md` and "The Input pane is the
  port's" in `docs/INPUT.md`.
- **A control that stops being a setting stops reading its saved key**, so nobody
  is stranded by a value with no control to show it; the env var is the
  comparison.
- **Changing the shipped keyboard layout** means bumping `KeyLayout.Version` and
  recording the old layout in `Superseded`, or an existing config reads as
  customised and is never corrected.
- **Anything the runtime refreshes only at `VSync` is invisible to a game that
  stops calling `VSync`**, and **a handler that never runs loses only the work
  inside it** — both failure modes are silent. See "Two general shapes worth
  keeping" in `docs/RUNTIME.md`.

### Regenerating function maps

Only needed if the sweep is wrong or a new executable is added:

```bash
RC="dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build --"
$RC --generate-function-file -linear-sweep -disc disc/KingsField2.cue \
    -file OPEN.EXE -base 80011000 -skip 800 -out config/funcmaps/open.json
```

An `unmapped call: 0x…` has three causes, so check which it is before reaching
for a script. **Is the address a function start the map is missing, an address
inside a function that already exists, or a few instructions past a mapped start
that has no prologue?**

- Missing start — `scripts/add_call_targets.py` recovers it by harvesting `jal`
  targets and splicing them into the map. Only sites already inside a known
  function are harvested; a `.data` word that decodes as `jal` is not a call.
- Interior address — the sweep split one real function in two, because it ends a
  function at any `jr`/`j` plus delay slot and PSY-Q emits both *inside* a
  function (a `jr` through a switch table, a `j` to a shared epilogue). A
  conditional branch that crosses a boundary proves it, since MIPS branches never
  leave their function. `scripts/merge_branch_spans.py` merges on that proof (and
  on jump-table entries), checks nothing `jal`s a start it swallowed, and is
  idempotent — run it after any sweep. See "The sweep splits a switch" in
  `docs/RECOMPILATION.md`.
- False split from data — `add_call_targets.py` once treated a table word as
  `jal` and cut a real function. The crash address sits just past a mapped start
  that has no prologue and that nothing in code `jal`s; the previous function
  falls through into it. `merge_branch_spans.py` cannot see a fallthrough, so
  rejoin by hand (the previous start's size should reach the next real function).
  See "add_call_targets can split a function" in `docs/RECOMPILATION.md`. The script no longer
  harvests sites outside a known function, so re-running it will not re-cut.

```bash
python3 scripts/merge_branch_spans.py --dry-run   # all overlays, writes nothing
python3 scripts/merge_branch_spans.py fdat17
```

The FDAT overlays deliberately use `"skip": 0` so the overlay covers the module
header: the modules' switch jump tables live in it, past the 32 dispatch slots,
and the recompiler reads a jump table out of the overlay's own bytes. Raising
`skip` past them makes every `jr` through a table dispatch to nothing at run time.

## Architecture

```
config/kf2.json          recompiler config: overlays, funcMaps, stubs[], patches[]
config/funcmaps/*.json   swept function maps (address/name/size; size is mandatory)
patches/                 hand-written C# replacing recompiled functions
mods/<id>/               runtime-loaded mods (mod.json + C#, Roslyn-compiled)
mcp/                     stdio MCP server exposing the KF2_SHELL command channel as tools to MCP hosts
Verdite2.Launcher/       the SHIPPED executable; builds with no disc, and makes the
                         game at first run from the player's own image. See docs/PACKAGING.md
packaging/               AppImage and Windows packaging, plus placeholder icons
patches/recompone/*.patch  the record of the port's changes to the vendored RecompOne
generated/               recompiler output (gitignored — derived from copyrighted disc data)
scripts/*.py             disc inspection, address-hunting, and the rate tooling:
                         merge_sdk_names (write the PSY-Q names a signature
                         match found into config/funcmaps/, refusing the ones
                         SdkPatches would bind -- see "Merging the SDK names" in
                         docs/RECOMPILATION.md),
                         shader_probe.c (run a real prim fragment shader
                         headless over a known texture and read the pixels back:
                         the only thing here that can say a shader change moved a
                         pixel, since the port is CPU-bound and frame rate cannot),
                         rate_census (which words move at the render rate),
                         find_writers (which code moves them), rate_matrix (did
                         the fix work), check_gate (does the gate obey its rule).
                         kf2run/callgraph/kf2model are their shared halves.
                         See "Finding the rate defects" in docs/DEVELOPMENT.md
Program.cs               hand-owned entry point; calls Entry.Run(PSMemory, cuePath)
```

The disc holds a 4 KiB boot stub (`SLUS_001.58`, named by SYSTEM.CNF) plus three
real executables — `OPEN.EXE` (title/intro), `GAME.EXE`, `END.EXE` — which **all
load at `0x80011000`** and are mutually exclusive. They are declared as
**overlays** in `config/kf2.json`. Left alone the recompiler would only find the
2 KiB stub. Per-area logic is more MIPS loaded at run time: the `fdat` modules.

Because the three overlays share an address range, *every* address-based config
entry must name its overlay explicitly. Prefer a named overlay over `"*"`.

### The core problem: SDK functions must be mapped by address

The recompiler's `SdkPatches.cs` binds PSY-Q calls to the runtime's HLE by exact
**function name**. A linear sweep names everything `func_800xxxxx`, so it always
reports `applied 0 reimplementations`. Every SDK entry point therefore has to be
mapped by address in `patches[]`:

```json
{ "overlay": "open", "address": "0x80016078",
  "target": "RecompOne.Runtime.Sdk.LibGpu.DrawOTag", "mode": "replace" }
```

63 such patches exist today (`libetc`, `libcd`, four `libgpu` entry points, six
`libcdstream`, libapi's `DMACallback`). `libpad` is the notable gap — and a
signature match confirms it is not a gap at all: the game links no `libpad`, which
is consistent with it reading `PAD_dr` through `BiosB` instead.

**997 non-binding functions now carry their real PSY-Q names** (`rsin`, `rcos`,
`SsSetMVol`, `RotTransPers`...), from upstream's signature bank via
`scripts/merge_sdk_names.py`. That is legibility only: the script **refuses every
name `SdkPatches` binds**, reading that list out of the patched checkout rather
than copying it, so all 63 entries above still do the binding and the recompiler
still reports `applied 63 patches, 0 reimplementations`. See "Merging the SDK
names" in `docs/RECOMPILATION.md`. Only map a function the runtime actually
implements — check `tools/RecompOne/RecompOne.Runtime/sdk/Lib*.cs` first; unmapped
library routines run fine as recompiled MIPS because `PSMemory` traps their
register writes.

`mode` is `"replace"`, `"pre"` or `"post"`. **Use config patches for `replace`
only** — binding an SDK entry point, which has to happen before any mod could
load. For anything else, do not add a config entry: RecompOne's `HookManager`
detours a recompiled function by address at run time, so a hook needs neither a
config entry nor a recompile. See "Mods" in `docs/PATCHES_AND_MODS.md`; `patches/FramePacing.cs`
is the in-project example and `mods/` holds the loadable ones.

**Generated code is one class per overlay** — `Recompiled.KingsField2_game`,
`_open`, `_end`, `_fdat`, `_main` — because CoreCLR caps a class at 65535 methods.
Code that calls a recompiled function directly carries a one-line
`using KingsField2 = Recompiled.KingsField2_game;` alias (sixteen call sites, in
`patches/AreaWarp.cs`, `AutoReload.cs`, `CullGrid.cs`, `MenuMouse.cs` and
`mods/kf2debug/Noclip.cs`, `Attributes.cs`), so do not also name a namespace
`KingsField2`. A direct call is *not* the same call as `Dispatcher.Call`, which
goes through `HookManager`.

### Identifying an address: the techniques that work

In rough order of payoff (full reasoning and worked examples in
`docs/RECOMPILATION.md`):

1. **The overlay delta.** The three executables are three links of the same
   libraries, laid out at a constant offset *per translation unit* (not per
   library — `libcd`'s `cdio` and `stream` differ from each other). Identify once,
   derive the other two. `scripts/match_overlays.py` does this mechanically
   against a relocation-insensitive normal form; validate any new delta by
   re-deriving already-known addresses with it.
2. **Data-side search for hardware addresses.** PSY-Q reaches I/O through pointer
   tables in `.data`, never through literals in code — there are five
   `lui …, 0x1F80` instructions in all of `OPEN.EXE` and none is the GPU. Search
   the *data* for `0x1F801810` (GPU) or `0x1F801800` (CD) to find the table; the
   functions loading through it are the library.
3. **Diagnostic strings.** `func_80014C0C` is the `printf` thunk (BIOS A(3Fh), 69
   call sites). PSY-Q error text (`VSync: timeout`, `CdInit: Init failed`,
   `GPU timeout:QUE=…`) names the calling function for free.
4. **Struct offsets as evidence.** Two independent offsets agreeing (a `DR_ENV`
   packet at `env+0x1C` *and* a `0x5C`-byte copy) turns a guess into an ID.
5. **Indirect-call tracing.** Public entry points can have zero `jal` references
   because PSY-Q dispatches through driver tables filled in at init (`libgpu`'s
   15-slot table, libapi's `DMACallback`). Log the address in `Dispatcher.Call`
   rather than searching statically.

### Two traps

**Number bases differ between the config and the CLI.** In `config/kf2.json`,
`base` is a hex *string* while `size`/`skip`/`offset`/`lba` are decimal numbers;
on the `--generate-function-file` CLI, `-size`/`-skip`/`-offset` are hex.
`"skip": 2048` in the config is `-skip 800` on the command line.

**Overlays are read as raw bytes.** `ResolveOverlay` does not parse the PS-X EXE
header, so every `.EXE` overlay needs `"skip": 2048` to step past the 0x800-byte
header, and `base` must be the header's real text address (read it with
`scripts/extract_file.py --header-only`). The *boot* executable is different — it
goes through `Psx/Parser.cs`, which strips the header itself.

### csproj

`generated/` and `patches/` are picked up by the SDK's default globs — adding an
explicit `Compile Include` causes NETSDK1022. `tools/**` must stay explicitly
removed, or the build compiles RecompOne's own sources and its `obj/`
AssemblyInfo files (CS0579); `mods/**`, `mcp/**` and `Verdite2.Launcher/` are
removed for the same reason.

## The RecompOne checkout

**`tools/RecompOne/` is vendored: an edit inside it is a change to this
repository like any other.** `patches/recompone/*.patch` are kept as the record of
what the port changed and why, and the numbers (`0001`-`0079`) are how the source
refers to each change, but they are **no longer replayed**. The merge base is
`tools/RecompOne/UPSTREAM` (currently `d81dec8`); the fork's history is the
gitignored `tools/RecompOne.git/`, reached with
`git --git-dir=tools/RecompOne.git --work-tree=tools/RecompOne <cmd>`.

- **Three changes force a recompile**: `0004`, `0035` and `0037`. Everything else
  is runtime-only.
- **The acceptance test for a merge** is `open → game → fdat02 → fdat05`, slot 2
  restored at hp 46/86 in area 1, 144.0 fps drawn at 20.0 ticks/s, every hook
  attached — **paired with `KF2_PRESENT_PROBE=1`**, because every number in that
  run was once still true with a completely black window. The last merge also
  silently broke the vertex map (`KF2_PERSPECTIVE_PROBE=1` reading `0 caught/s`)
  and the widescreen margin's clear; check both.

**Nothing goes upstream. Not a pull request, and not an issue either.** Upstream
rejects AI-authored pull requests outright, and this project does not file
issues against it: a defect found here is recorded in `docs/` and fixed in the
vendored tree, which is the whole point of vendoring it. If the user wants
something reported upstream they will write it themselves.

@docs/RECOMPONE_FORK.md
@docs/RECOMPONE_PATCHES.md

## Shipping it

**The port cannot ship a playable binary.** `generated/` is a translation of
FromSoftware's code, so the assembly that plays the game has to be built on the
machine of somebody who owns the disc. The release ships every **input** —
`config/`, `patches/**`, `Program.cs`, the vendored RecompOne — and makes the
output at first run. That is also a correctness win: the generated dispatch tables
bake **absolute LBAs from one mastering**, so a prebuilt binary would silently
fail to load area modules on a differently mastered dump.

- **`Verdite2.Launcher/` is the shipped executable and compiles neither
  `generated/` nor `patches/`** — it carries them as payload under `content/` and
  compiles them at first run. Anything added there must keep that property;
  `.github/workflows/ci.yml` asserts it on every push, because it is invisible
  locally where `generated/` exists.
- **The recompiled output and the port's sources compile in ONE Roslyn pass**
  (`GameCompile`). Its reference set comes from `TRUSTED_PLATFORM_ASSEMBLIES`, not
  loaded assemblies, and it supplies `GlobalUsings.g.cs` itself, since
  `ImplicitUsings` is an SDK feature.
- **`GameCompile`'s options and `KingsField2Recomp.csproj`'s properties are two
  statements of one thing and must stay in step** — a difference is a bug that
  exists only in the release. Check the packaged binary with
  `KF2_FPS=144 KF2_FPS_PROBE=1`: 144.0 fps drawn at 20.0 ticks/s.
- **The version is one line in `VERSION`**; everything else reads it, and
  `release.yml` asserts the tag equals `v$(cat VERSION)`. `bash scripts/release.sh
  0.2.0` bumps, commits and tags, and deliberately does not push.
- Packaging is `packaging/linux/build-appimage.sh` and
  `packaging/windows/build-windows.ps1`, neither of which needs the disc.
  **Trimming is off and must stay off** (MonoMod detours, Roslyn, `AutoStart`'s
  reflection).
- **QuickJit is off and must stay off, in both `KingsField2Recomp.csproj` and
  `Verdite2.Launcher.csproj`** (`TieredCompilationQuickJit`). Tier-up recompiles a
  hooked method and MonoMod's detour does not reliably follow, so a committed hook
  stops firing for the session with `IsCommitted` still true — that was the boot
  that ran at twice the chosen rate. `FramePacing`'s sentinel prints
  `[KF2] pacing sentinel:` if a pacing hook is ever lost again. `DiscCheck.Validate` fills `Runtime.DiscValidator` and refuses
  `SLUS-00255` by name.

See `docs/PACKAGING.md`.

## Repository conventions

Never commit disc data or recompiler output — `disc/`, `generated/`, `*.sav` and
`settings.json` (written by the runtime at play time) are gitignored for
copyright and cleanliness reasons.

Commit messages in this repo state the *finding*, in the imperative, with the
observable consequence: "Map the PSY-Q CD library; boot now reaches the main
loop".
