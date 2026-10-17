# Plan: porting the renderer

Prepared 2026-10-14. **This is planned work, not a record of anything done.**
It covers everything Verdite2 (`KFII-PC/`) and Verdite3 have in their renderers
that this port lacks, and the order to build it in. Findings made along the way go
in `GAME_INTERNALS.md`, `PICTURE.md`, `SMOOTHING.md` and a new `GPU_RENDERER.md`,
not here. When a step's gate closes, strike it through here, as `TODO.md` does.

The model is Verdite2's `docs/VERDITE3_GPU_PLAN.md`, the plan Verdite3's renderer
was built from. Its rules hold here:

- **Build the final path once.** C# owns the game's scene enumeration and
  submission; retained GPU meshes and poses supply the geometry; the GPU projects,
  lights, fogs and depth-tests. Do not ship an intermediate renderer and then port
  it again.
- **The packet path stays** as the reference and the explicit fallback. Every C#
  replacement gets a literal mode that can be verified against the recompiled
  routine before it is changed or used to suppress work.
- **No fact carries over.** The siblings' addresses, layouts, table sizes, biases,
  fog formulas, light-record strides and water classifications belong to their
  discs. Copy mechanisms and conventions, then re-derive every number from
  `SLPS-00017`. The `kf1-port` branch has leads about this disc; re-measure them
  before writing them down.
- **Anything judged by eye is the user's.** Each step ends with counters and
  probes, plus a short list of what the user still needs to look at.

## Where this port stands

Done (2026-10-05):

- Stage I, the renderer `func_8001FDE4`, and the camera block `func_8001C184` in
  C#: 0 mismatches over 8,588 verified frames ("The renderer in C#" in
  `GAME_INTERNALS.md`).
- Frame and vblank pacing, the view carried between ticks, and models carried at
  the five submitters, with the clip time carried through the pose call
  (`SMOOTHING.md`).
- No dither, 24-bit, perspective, sub-pixel, and a Z-buffer **fed from the
  address map**, because there are no C# assemblers yet (`PICTURE.md`).
- Widescreen with the map stencils widened (`WIDESCREEN.md`).

What the renderer calls, as known so far:

| role | routine |
|---|---|
| map walk, 16 stencils | `func_8001E83C` (`func_8001E5EC`, `func_8001DE18`) |
| model walk | `func_8001F218` |
| its five submitters | `func_8001EBB8` (interaction objects), `func_8001E9A4` (objects), `func_8001ED90` (flipbook sprites), `func_8001EEDC` (creatures), `func_8001F0C4` (people) |
| pose blender | `func_800205D4`; pose slots at `0x800910C0`, marked by `func_800209A8`, swept by `func_80020A98` |
| projected-vertex cache | `func_8001C60C` fills `0x800911B0`-`0x800912A0` from the list at `0x800910BC` |
| arm | `func_8001F798` (model `0x14`) |
| HUD | compass `func_8001F8B0`, sprites `func_8001F9D4` → `func_8001E480`, models `func_8001E230`, item display `func_8001FAFC` |
| frame | `func_8001BFB8` (head; `ClearOTagR(ot, 0x4000)`), `func_8001C050` (present) |

**Unknown**: the routines that write the polygon packets (the "assemblers").
Every later step depends on finding them.

## Step 0: inventory and baseline

1. **Pull the shared subtrees to the commits the siblings pin.** The fork at
   `main` already contains this port's `0090`-`0093` byte for byte, so the pull
   only adds `0094`-`0110`. The rendering ones are `0094` (GPU dither
   suppression), `0095` (direct OT clear), `0096` (retained view under the world),
   `0097` (live render scale), `0098` (retained-model fade), `0099` (HUD over the
   surface passes), `0100` (radial fog), `0103` (model near clip) and `0110` (keep
   the displayed target). Verdite Core adds `GpuFrames`, `FrameProfiler` and
   `ProfilerPanel`. Re-run the acceptance test (`DEVELOPMENT.md`). Then decide
   whether `patches/NoDither.cs` gives way to `0094`.
2. **Find every drawing routine.** Using the recompiled `GAME.EXE` and the rate
   census, record which routine writes each polygon type (POLY_F/FT/G/GT 3 and 4,
   sprites, lines), for the map, the five submitters, the arm and the HUD. Also
   find any packet built outside stage I: the menus, the transitions, the
   modal loops, `OPEN.EXE`.
3. **Probes, in Verdite3's form, with `KF1_` names.** Port `StageProbe` (which
   stages submit), `GeometryProbe` (what each renderer site adds to the table, by
   packet type), `MapCoverage` (how the address map covers the frame, by writer)
   and `PrimBufferProbe` (how close the packet buffer comes to full). Port
   Verdite2's `DrawCensus` (which routine drew how much of the frame).
   `ProfilerKnown` names the routines for the core profiler.
4. **Census of the content.** Enumerate each area's map meshes, model banks, face
   types and clips; enumerate the object kinds the walk skips; and list the
   drawing contexts: gameplay, area change, menu, save/load, dialogue, death,
   title/attract, ending.
5. **Driving.** The shell has `view`, `poke` and the scripted Starts. For a
   repeatable corpus, add a game-thread `warp` to an area and cell (Verdite3's
   `SceneDriver`, `mods/kf3debug/Warp.cs`), working on copied cards.

**Status 2026-10-14** (findings in `GPU_RENDERER.md`): 1 done (`NoDither` moved to
`0094`; the load step of the acceptance test does not reproduce, before the pull
or after). 2 done: every writer and its codes (the full table at `0x8001222C`),
the projection and the depth; death and the ending located. 3 done: `StageProbe`
(now with the menu), `PrimBufferProbe`, `GeometryProbe`, `MapCoverage`,
`DrawCensus` and `ProfilerKnown`, measured. 4 done: meshes and face kinds per
area; the banks counted, clips not identified. 5: `warp` measured in areas 1-4.
Measured: areas 1-5, walking, the arm swinging, the menu, Start's message,
death. **Left open, by owner**: area 5's warp (every cell tried fell or died;
the verb's entry 0 is suspected), the ending and title (read, not run), OPEN.EXE's
`func_8001A82C` (caller not found), the map TMD's F3/G3/G4 primitives (who reads
them), the bank records' meaning, and the save and load screens (not entered).

**Status 2026-10-16**, the items left open, worked through (`GPU_RENDERER.md`):
area 5's warp stands, now that the warp writes the floor word, and its deaths are
a creature's 155-HP hits on a 30-HP player; the ending ran (from its flag at
`0x8009F847`) and the title cycled; the save and load screens were entered and
left by program; `func_8001A82C` has no caller anywhere on the disc (a debug
display adjuster, read); the "map's" F3/G3/G4 were the models', the doc having
swapped chunks 0 and 1; the bank records are decoded (clip and key tables). **Still
open**: the title's quiet 10 s in each 30 s cycle; whether the ending waits on
a button at its end; what sets `0x8009F847` in play; the bank records' repeated
keys.

**Gate**: every emitter and context has an owner and a way to verify it; anything
not reached is listed as open. Counters are taken with the GPU path off, so a zero
later cannot mean the probe never attached.

## Step 1: the draw path in C#, literal

Each routine below is transcribed literally from `generated/`: the same reads and
stores, in the same order, with the callees called through delegates so that hooks
still fire. Each is verified with `KF1_<NAME>=verify`, using the `Differential`
the camera block uses or the renderer's call-by-call record. A routine is not
changed or optimised until it verifies at 0 mismatches over a walk, a menu, an
area change and the busy scenes from Step 0.

1. **The model walk** `func_8001F218` (Verdite3 `ModelWalk`, Verdite2 `ModelWalk`).
   Move `ModelSmoothing`'s hooks from the submitters' pre/post into the walk,
   without changing what is carried. Fix the item display's per-call step
   (`TODO.md`) here, holding it to the tick.
2. **The five submitters**, then **the arm**. Each owns its matrix, light
   selection, pose choice, flags and assembler choice.
3. **The pose blender** `func_800205D4` (`MoPose`), with the pose-slot mark and
   sweep. It keeps the clip-time carry `ModelSmoothing` already applies through
   its arguments.
4. **The assemblers**, as Step 0 found them (`PolyAssembler*`). Once they are in
   C#, port **`PolyAssemblerDepth`**, which records corner depths beside each
   packet, so the Z-buffer takes `GtePacketDepth` instead of the address-map
   fallback. Port the HUD's transform (`PolyAssemblerHud`) with the fraction it
   drops handed to the vertex map. If this game clips or subdivides (Verdite2's
   `Clip3FTP`/`Clip4FTP`, Verdite3's libgte division routines), port that too:
   read which one the code uses, and do not assume.
5. **The map walk** `func_8001E83C` and the stencil routines (Verdite2's
   `TileWalk`, Verdite3's `NativeSceneWalk`/`Cell`/`Half`). Keep `CullCone`'s
   widening working through it.
6. **The vertex cache** `func_8001C60C`, if Step 0 shows it is the projection
   every assembler reads.

**Gate**: with every routine on its C# body, the renderer's own verify still
reads 0 mismatches. Every routine has an exercised fixture, and the census shows
no world packet written by a recompiled body.

**Status 2026-10-14**: done ("The draw path in C#" in `docs/GPU_RENDERER.md`). All 24
routines read 0 mismatches over the corpus, area 5 and the menu. The only
exception is the pose slots, where the HLE `malloc` hands the re-executed call
a different block. The renderer's verify reads 0 mismatches over 1,683 frames
with all 24 on. Two writers are exercised only in particular places: the cached object writer in area 5, and the people submitter by area 1's person. Also
done:

- the smoothing carry is in the walk;
- the item display is held to the tick;
- packet depths feed the Z-buffer.

There is no HUD transform to port (the compass goes through RTPT), and no clip
or subdivide in the draw path.

## ~~Step 2: things that run at the drawn rate~~

Done 2026-10-15: the gate holds. The census with creatures, objects and the arm
moving shows only the frame's working memory with nothing carried. The two loops
that waited for nothing are held. There are no tints or texture animations to
hold, and the sprites follow the carried camera ("Sprites and the eye", "Loops
that draw their own frames", "The tints between ticks" and "Textures" in
`SMOOTHING.md`; "What runs at the render rate" in `GAME_INTERNALS.md`).

Run before and after Step 1 with `KF1_RATECENSUS` and moving creatures and
objects (the census so far was taken standing):

- `SpriteAnim`: replace the cel hold in `ModelSmoothing` with a step once a tick,
  if the walk in C# makes that simpler.
- `TextureScroll`/`FluidSmoothing`: if this game scrolls textures (water,
  lava), hold the scroll to the tick, then carry it between ticks.
- `TintHold`: full-screen tints (the death fade, the damage flash, an area's
  fade-in) drawn on every frame or only on ticks; Verdite3's `TintProbe` measures
  it first.
- `LoopPacing`/`LoadPacing`/`MenuPacing`: the modal loops and loading screens
  that present frames themselves, held to the console's rate.
- `BillboardFacing`: whether the sprites turn to the eye correctly while the
  camera is carried.

**Gate**: the rate census, taken while things move, shows nothing a frame shows
advancing at the drawn rate.

## ~~Step 3: the retained scene~~

**Done 2026-10-15** ("The retained scene" in `docs/GPU_RENDERER.md`): the gate's
counters hold. The map, the models, the seven measured semi meshes (as blended
faces) and the flipbook sprites are retained. No cell changes mid-area, so there
is no dirty update to write. The arm, the HUD, the creature sprites and area 5's
cached writer stay packets by recorded decision. Off by default until judged.

From here on the work runs on the shared runtime's `RetainedScene`, `GlRetained`,
`GlMainView`, `GlModelMeshes` and the world shaders. Read their APIs before
adding any. Changes to the runtime are game-neutral, go in their own commits as
fork patches, and must leave Verdite2's and Verdite3's acceptance tests reading
the same.

1. **`GpuWorld`**: the switch (`KF1_GPU_WORLD`: reference / retained beside
   reference / retained), per-domain toggles, and the fallback census
   (`GpuWorldCensus`/`SceneCensus`). A fallback is counted, never silent.
2. **`RetainedMap`**: the area's map as a world-space mesh on the GPU, with its
   face types, texture rectangles, light records and its own record layout and
   fog pair, re-derived from this game's walk. Doors, lifts, and anything else that
   rewrites the map mid-area get dirty updates, not a rebuild. This game's levels
   share one coordinate frame (`GAME_INTERNALS.md`, "Changing areas"); check
   whether two areas' meshes can be resident at once.
3. **`RetainedModels`**, **`RetainedPoses`** and **`RetainedAssets`**: meshes
   uploaded once per bank, instances published by the C# submitters, and poses
   blended on the GPU from cached keys. This covers rigid and animated models,
   sprites, the arm and the HUD models (or the HUD left as packets, by a
   recorded decision).
4. **The carried camera and object placement** published for the presented
   frame, so the smoothing still applies on the GPU path.
5. Draw order: blended after opaque, the arm over the world, and the HUD and menus
   over everything (`0096`, `0099`). Find whether this game has a sky or a
   front table, and give each its domain.

**Gate**: stable assets upload once per area; a pose change uploads no vertices.
GPU projection and pose results match the C# reference on sampled vertices,
including near and negative depth. With the GPU on, the census shows no world
packets except those listed by caller and reason.

## ~~Step 4: the picture on the retained scene~~

**Built 2026-10-16** ("The picture on the retained scene" and "Render distance" in
`docs/GPU_RENDERER.md`). The numerical gate holds: interior fog and model light
are within 1 of 255 everywhere checked, and the map and the models reach the
depth, normal and surface buffers. Built: render and enhancement distance with the
fade, the game's fog (GTE curve, one pair) with corners/depth/distance modes,
per-pixel lighting, AO, anisotropy and mipmaps, all off until judged. Not built:
`EvenFog` (not applicable: there is no per-tile fog level), `Pgxp` (not used by
Verdite2's retained path or Verdite3), `AoWorld` (needs a fork patch for the
grid's sizes, and a KF1 adapter) and `ProjectileLights` (effects not identified).
The user's judging is what is left.

The enhancements, in the order Verdite2 shipped them. Each is a switch, off until
the user has looked at it, and live in Settings ▸ Testing:

1. **`RenderDistance`** and **`EnhancementDistance`**: the map and models past the
   stencils' reach, faded rather than popped (`0089`, `0098`), with
   `CullCone`'s cone extended.
2. **Fog**: the game's own depth cue first, exactly (re-derive whether it is
   GTE-curve fog like Verdite2's or near/far fog like Verdite3's); then
   **`Fog`**/`SceneFeatures` with corners, per-pixel and radial (`0100`), and
   **`EvenFog`** (neighbour blending across tile edges, `0088`).
3. **`PerPixelLighting`** with `PolyAssemblerLight`'s light-input records, checked
   against the GTE's products numerically.
4. **`AmbientOcclusion`**, then **`AoWorld`** (occluders off-screen, needs a map
   adapter).
5. **`Anisotropic`** filtering and mipmaps.
6. **`Pgxp`**, if the retained path leaves any packet geometry that still needs
   it.
7. **`ProjectileLights`**: a light at each glowing effect, once the effects are
   identified.

**Gate**: numerical light and fog checks at corners and interior depths; buffer
counters showing every eligible family contributes to depth, normal and surface.
Then the user judges seams, fog, lighting, AO and transparency.

## ~~Step 5: water~~

**Not applicable, 2026-10-16** ("Water: none to classify" in `GPU_RENDERER.md`):
nothing scrolls in VRAM or in the meshes' RAM, no textured face is blended, and
no blended object has a level surface wider than 250 units. Nothing is built.

Only if this game has water to classify. The classification must come from this
disc's own data (Verdite3's `WaterRects`, from its scrolling slots; Verdite2's
`ModelWater`), never from texture reuse alone.

1. `Murk`, `Waves` and `WaterSwell`.
2. Reflections: `Reflections`/`ReflectionReach`, screen-space; then planar ones,
   `PlanarMirror` (Verdite3) or `PlanarWalk`/`PlanarCull`/`RetainedPlanes`/
   `ScenePass` (Verdite2). Use whichever fits the retained scene as built in
   Step 3. A second camera must not touch the simulation, the cels or the guest
   matrices.

## ~~Step 6: widescreen and menus on the new path~~

**Not applicable, 2026-10-16** ("Widescreen and menus: nothing to open" in
`GPU_RENDERER.md`): no clip in the draw path is bound to the screen, the menus
clear to black and store no frame, and the packet buffer peaks at 46.2% at 21:9.
Nothing is built.

- **`ViewClip`** (Verdite2) and **`NearScreen`** (Verdite3): open the game's
  view-space clip and near-screen bounds to the aspect, where the C# assemblers
  expose them.
- **`MenuWorld`**: draw the world live behind a menu or message, instead of the
  frozen 320-wide copy; with **`MenuDraw`** (the menu's quads in C#) if the
  menus need it.
- **`PrimBuffer`**: move and enlarge the packet buffers if Step 0's probe shows
  them close to full at wide aspects or long distances.

## ~~Step 7: diagnostics worth keeping~~

**Done 2026-10-16** ("Diagnostics on the GPU path" in `GPU_RENDERER.md`): the
profiler's sections for the retained scene, `CrossProbe` (with `BlackProbe`'s
PNGs), `TexProbe` and `PacketMatch`. `FrameCapture` and `FrameViewerPanel` are
not ported: they replay the software path.

`FrameCapture` and `FrameViewerPanel` (Shift+F; one renderer run captured and
scrubbed a command at a time), `TexProbe`, `CrossProbe`, `BlackProbe` and
`PacketMatch`. Port them when a bug calls for them, not before. `CrossProbe`
and `BlackProbe` belong with the first area-change bug.

## Step 8: whole-game coverage and adoption

- Visit every area, confirming the area actually loaded, with near views, busy
  views, doors and lifts; then every context from Step 0.
- Run the supported combinations: GPU against reference, pacing and smoothing
  on and off, 4:3 and wide, each picture switch, at `KF1_FPS=off` and at 144.
- Record CPU extraction, uploads, draw calls and GPU time per pass
  (`GpuFrames`). Compare uncapped against the Step 0 baseline (751-783 fps
  standing, recompiled).
- Defaults change only after the user has looked. The reference path stays for
  regressions and unsupported backends.

## Per-step bookkeeping

Each step updates `ENV_VARS.md` with its switches, `GAME_INTERNALS.md` with the
addresses it found, the relevant topic document with what it measured, and
`TODO.md` with what is still to be judged. Each fork change is numbered in
`tools/RecompOne/docs/RECOMPONE_PATCHES.md`, committed alone, and pushed to the
fork.

## Sources

- Verdite2 (`../KFII-PC`): `docs/VERDITE3_GPU_PLAN.md`, `docs/GPU_RENDERER.md`,
  `docs/RENDERING.md`; `patches/` as named above.
- Verdite3 (`../verdite3`): `docs/GPU_RENDERER.md`, `docs/GEOMETRY.md`,
  `docs/PICTURE.md`, `docs/WATER.md`; `patches/Stage15.cs`, `NativeScene*.cs`,
  `NearPath*.cs`, `Retained*.cs`, and the probes named in Step 0.
