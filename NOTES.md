# King's Field II — RecompOne port

Static recompilation of **King's Field** (NTSC-U, `SLUS-00158`) using
[RecompOne](https://github.com/BlackLabelHQ/RecompOne) (MIT).

**This file is the index.** It carries what the project is and where it stands;
everything else lives in `docs/`, split by what you would be doing when you need
it. **Nine documents became ten**: `docs/PACKAGING.md` is how the port becomes
something a person can download, which is a different job from building it. The
eleventh, `docs/REMASTER.md`, is the design for a remaster's authoring tools,
and the record of the work against it; Phase 1 is in, in two slices.

**Source comments still say `See "X" in NOTES.md`, and the text they mean is no
longer in this file.** The section titles are unchanged, so the map below resolves
X to a document — but grepping this file for one will only find its bullet in that
map, and the passage itself is one hop away. Four workflow headings were renamed
in the move (`1. Set up the tools` → "Setting up the tools", and its three
siblings); nothing in the source cites those.

## Which game this is

The series was renumbered for the West, so the name is ambiguous:

| Chronological | Japan | North America |
|---|---|---|
| 1st (1994) | King's Field, SLPS-00017 | *not released* |
| 2nd (1995) | King's Field II, SLPS-00069 | **King's Field, SLUS-00158** |
| 3rd (1996) | King's Field III, SLPS-00377 | King's Field II, SLUS-00255 |

This project targets the **second game**. The disc in use is the North American
release, which is the English localization of the Japanese *King's Field II* —
same game, so the "KFII" project name is accurate even though the disc says
`SLUS-00158`. If you ever swap in the US-boxed "King's Field II" (`SLUS-00255`),
that is a *different game* and every address and function map here is wrong for it.

## Status

**The game is playable.** `OPEN.EXE` streams the intro movies off the disc
through the MDEC, the title screen loads and runs, the boot stub swaps in
`GAME.EXE`, the main game gets through its data load and memory-card check, and
the first area comes up and can be walked around in.

Reaching that took three CD paths that a static recompilation breaks in three
different ways — see "The three ways a CD read can hang" in `docs/RUNTIME.md` —
and it turned up the fact that **`GAME.EXE` is not the whole game**: per-area
logic is MIPS code loaded off the disc at run time (see "GAME.EXE loads code" in
`docs/RECOMPILATION.md`). The area modules are confirmed by play, not just by
static analysis: walking around is `fdat02` executing.

The in-game menu opens. It used to hang the port dead, and the two bugs behind
that are worth reading before touching anything interrupt- or input-related:
the runtime was **guessing the address of PSY-Q's interrupt-callback table** and
eventually called a game data word as a function, and **host input was only
polled from `VSync`**, so the menu's wait-for-button-release loop — which draws
nothing and never vsyncs — could never see the release.

`libetc` (VSync), `libcd`, `libgpu`, `libcdstream` and libapi's `DMACallback`
are mapped to the runtime's HLE — 66 patches. A steady-state second of
`KF2_LOG=sdk` looks like this, and is what "working" should look like:

```
[SDK] StFreeRing
[SDK] DrawSync(0)
[SDK] PutDrawEnv env=0x800A8044 clip=(0,240)-320x240 ofs=(0,240) isbg=0
[SDK] PutDispEnv env=0x800A80A0 disp=(0,0)-320x240
[SDK] DrawOTag   ot=0x800A80B4
```

— one frame off the ring, one buffer flip, one ordering table, repeating.

Two areas have been played, half an hour in one sitting, across an area-module
swap and a memory-card save, and the audio sounds right. **A save has now been
loaded back** — slot 2, from the title screen, correct area and correct character
state (see "Saving and loading" in `docs/GAME_INTERNALS.md`).

The picture, all four in `docs/RENDERING.md`: **textures are
perspective-correct** — the depth the GPU never receives is recovered from the GTE
and matched back by screen position, which changes 76.6% of a frame's pixels (see
"Perspective correction"). The same table now also recovers the **sub-pixel
position** the GTE truncates, so vertices need not snap to whole pixels; that one
ships on (see "Sub-pixel vertex positioning"). **A Z-buffer is available from the same
recovered depth** — per-pixel occlusion instead of the ordering table — and ships
on (see "Z-buffer"); the cause that had it looking
unfixable was found, and it was the clip W rather than the depth (see "PGXP").
Its depth now comes from the C# assemblers' own packet records, so the skybox and
the HUD keep painter's order by construction (see "The assemblers write the depth").
**Ambient occlusion runs on that same depth**, and the point of it is where the
G-buffer comes from: there is no depth prepass to be had here — the geometry
arrives incrementally through GP0 and nothing knows the frame is finished until it
is — but `DrawOTag` walks the ordering table back to front, so a depth *write* with
the test left at `GL_ALWAYS` ends the frame holding the nearest visible surface at
every pixel. Everything with no recovered depth writes the far plane instead, which
is the HUD mask for free (see "Ambient occlusion"). On by default.
**Upstream's own PGXP is backported and is the second source of that depth**,
measured as the same coverage for a fifth of the frame rate in this game, and
kept for what it decides exactly rather than by luck of the copy — but it is
chosen on the console and nowhere else: it has no control in the settings window
and neither does the Z-buffer (see "PGXP has no control in the window"). Nearby walls and floors no longer
pop back to affine the moment one vertex clamps off-screen, and a pixel that two
vertices share no longer hands one polygon the other's depth (see "The table is
not unique").
**Anisotropic filtering is the minification half of that first sentence**: a
screen pixel covers an *area* of the texture rather than a point, and on a floor
running away to the horizon that area is a long thin sliver of texels the console
sampled once — which is the crawling, sparkling floor, and which the port's own
render scale makes more visible rather than less. It cannot be sampler state,
because the VRAM texture is one sheet holding every page and every CLUT (so no
filter may run across it) and a paletted texel is an *index* (so no filter may run
before the lookup); the kernel therefore lives in both prim shaders, after the
CLUT. It needs no "is this 3D" test, unlike anything else that softens a texel: a
HUD sprite is axis-aligned and unminified, so its footprint is isotropic, the tap
count comes out 1 and it takes the unfiltered path by construction — measured,
not argued: at a 1:1 footprint 1 tap and 16 return identical pixels. **Frame rate
cannot check this feature at all** (861.6 fps against 860.7, the port being
CPU-bound at ~860, so "costs nothing" and "never runs" read the same), so
`scripts/shader_probe.c` runs the real fragment shader headless and measures the
spread between neighbouring pixels — that spread being the sparkle — which
collapses from sd 51.2 to 11.4 across the levels. Every tap is now held inside the
polygon's texture rectangle — the edge was still reading the neighbouring art, the
floor speckle — and **mipmaps** are built where a texture is decoded, in an atlas
of its own, since VRAM cannot hold them (`0060`). Both off by default, the picture
not looked at (see "Anisotropic filtering").

**The frame rate is a free number and the world's tick rate is a fixed 20.** The
port skips the game's own frame gate at every rate — it decides both together and
knows one answer for both, 30 — paces the picture itself, holds the world to a
fixed timestep and carries the camera between ticks, so the picture can run at 60,
120 or 165 while the world keeps a console's timing. **The render rate defaults to
60**, three pictures per tick; 20 is still the world's clock and what every
rate in this port is measured against. The literal 2
in that gate is a *ceiling* rather than a target — it forbids a frame faster than
30 and asks nothing of a slower one — and the console missed that deadline under
load and landed in the three-vblank band, so since the game's speed is its frame
rate, 20 is the speed it was played at and built at. That is a judgement no counter here can
settle — which is a reason to make it, not a reason to hand it to the player, so
the tick rate is **not** a setting and `KF2_TICKRATE=30` is the comparison (see
"Any frame rate" in `docs/PATCHES_AND_MODS.md`). **The ending runs**: `END.EXE` plays the two STR
movies and holds "The End", and **any button then returns to the title** —
holding it forever is what the original does, and on a window that is
indistinguishable from a crash. See "The ending screen" in `docs/RUNTIME.md`.

**A first run is 16:9, sub-pixel, the Z-buffer, 24-bit shading, even fog, per-pixel
lighting, ambient occlusion, smoothed 60 fps, map fog, and mouse look.** Those
were the picture-never-checked switches; they ship on now that the picture has
been judged. Anisotropic filtering is still off. `KF2_*` is the comparison for
any of them. What is open and undiagnosed is in `docs/TODO.md`.

## Layout

```
config/kf2.json          recompiler config (schema: RecompOne.Recompiler/Config/ConfigLoader.cs)
config/funcmaps/         generated function maps (address/name/size)
patches/                 hand-written C# replacing recompiled functions
mods/<id>/               runtime-loaded mods (mod.json + C#), toggled in-game
scripts/inspect_disc.py  SYSTEM.CNF + ISO9660 listing from a .cue/.bin
scripts/extract_file.py  extract a disc file and dump its PS-X EXE header
scripts/match_overlays.py  carry a function identified in one overlay to the other two
disc/                    your own dump (gitignored)
generated/               recompiler output (gitignored, derived from the disc)
tools/RecompOne/         upstream tool checkout (gitignored)
Program.cs               hand-owned entry point
KingsField2Recomp.csproj
```

## The documents

A section name in quotes anywhere in these files is an exact heading, in the file
named beside it here.

### [DEVELOPMENT.md](docs/DEVELOPMENT.md)

Build it, run it, and measure what it did.

- Prerequisites
- Disc structure
- Setting up the tools
- Recompile
- Build and run
- Diagnostics
- The GL backend reported nothing
- For a hang, take the managed stack of the live process
- Compile a mod without launching the game
- Getting pixels out without a screenshot
- Profiling a frame
- Watching a frame being built
- What counts as verification

### [PACKAGING.md](docs/PACKAGING.md)

Turning the port into something a person can download.

- The problem a release has to solve
- The two projects
- What the release contains
- First run
- Two things that were nearly wrong
- Versioning
- Building a release
- The one patch this needed
- Not done

### [RECOMPILATION.md](docs/RECOMPILATION.md)

Making the recompiler produce correct code: config, overlays, function maps, SDK addresses.

- Two traps worth knowing
- Two patches change the generated code, and only two
- Generating function maps
- Fixing bad output
- GAME.EXE loads code
- fdat32 is a cut area, and nothing can load it
- The SDK naming problem
- The overlay delta: identify once, get all three
- libgpu: found and mapped
- libcdstream: found and mapped
- VSync: found and mapped
- Where the rest of the mapped addresses are written up

### [RUNTIME.md](docs/RUNTIME.md)

What a static recompilation loses (interrupts, VSync-driven work) and the patches to the checkout.

- DMA callbacks: the thing that was actually missing
- The three ways a CD read can hang
- The interrupt-callback table cannot be guessed
- The vblank fired when the game asked
- There were three fixed 60s, and only one of them is the host's
- The intro movie ran at the render rate, because its pacer never armed
- The menu deadlock: input only moved when the game drew
- The ending screen
- Holding the frame is faithful, and it still reads as a crash
- The patches to the checkout, one by one
- The interface only fits a monitor whose scale is a whole number
- The scale can put the settings out of reach
- The picture is inset inside its own panel
- The interface's font
- CHD disc images
- Minimising froze the game on Wayland
- Two general shapes worth keeping
- Upstream contribution policy

### [RENDERING.md](docs/RENDERING.md)

Recovering the depth and the sub-pixel fraction the GP0 packet threw away: perspective correction, sub-pixel positions, Z-buffer, ambient occlusion, reflections, anisotropic filtering, dither, true color.

- Perspective correction: the depth is one step upstream, and the screen position is the key
- Sub-pixel vertex positioning: the same number's other half
- The table is not unique: remaining wobble and the "far away" pop
- Following the value through memory: the address is the vertex
- Z-buffer: the same depth, used as occlusion
- The world lost its textures on NVIDIA
- PGXP: upstream's own recovery, and what taking it actually bought
- Ambient occlusion: painter's order is the G-buffer
- Screen-space reflections: the water is the one surface the depth buffer does not have
- Planar reflections: the world walked twice, from under the water
- The retained scene: the world kept on the GPU, so a reflection can draw it again
- PGXP has no control in the window
- Dithering: one flag, and it lives in the draw environment
- True color: the other answer to 15-bit banding
- Anisotropic filtering: a pixel covers an area, and the console read a point
- The render scale did not survive a menu
- A shop overwrote the textures with the atlas's old texels
- The first intro movie never reached the screen
- The display list cannot name a face: why packet-level smoothing failed
- "No textures on the other machine": splitting the three layers

### [WIDESCREEN.md](docs/WIDESCREEN.md)

Aspect ratio, the HUD and screen-space effects authored 320 wide, and the three culls the margin runs into.

- Widescreen: the runtime renders the margin, the game fills a quarter of it
- The HUD does not widen with the world, and finding it is the problem
- The screen-space effects are 320 wide too, and one drawer makes all of them
- Widescreen became a patch, and the default stayed 4:3
- Three checkboxes that were not choices
- The cull the margin runs into: a 24×24 tile grid, and a trapezoid drawn on it
- A widened cone dropped stock tiles, in the middle of the picture
- The primitive buffer ran out
- The second cull: a view-space clipper, and it is set to twice the screen
- Is the 24-tile window worth lifting? Measured: binding, and barely
- There is a third cull and it is none of the obvious ones

### [GAME_INTERNALS.md](docs/GAME_INTERNALS.md)

The reverse-engineered game: main loop, player state, stats, death, movement, areas, saves, the boot stub.

- The main game loop, stage by stage
- The loop's own rate gate is `func_80017880`, and the number is a literal 2
- Stage 2 is the object-table state machine
- The map is an 80x80 tile grid, and a tile's height is one byte
- The model pipeline has no skeleton
- Stage 8 is the render camera, and it is the only copy
- What in the renderer draws what
- Stage 13's HUD block, and the compass needle
- The frame's applied position delta is a triple of its own
- Player state: found, and it was in stage 3 all along
- The inventory is one byte per item, and the item id is the name-table index
- Saving and loading
- Debug tools
- The Items tab gives through the game's own give, and the names are read live
- Full-screen messages are pictures, and the world behind them is a 1x copy

### [PATCHES_AND_MODS.md](docs/PATCHES_AND_MODS.md)

How the port's own code attaches, where its settings go, plus frame pacing, auto reload and the map.

- Where each patch is written up
- Mods
- Patch settings: a patch's knobs go in the runtime's own sections
- Frame pacing: the port is pinned to the fastest band
- Any frame rate: three gates, one logic clock, and a smoothed view
- One switch for all of the smoothing
- The water still steps at the tick
- Menus draw the world live
- Messages draw the world live
- Drawing message text
- Auto reload
- A dynamic map
- Five map controls that were not choices
- What the Map page is down to
- Auto start and the agent beacon
- The command channel
- `ending` exists because the last ten minutes of the game are otherwise untestable
- The MCP layer
- The polygon assembler in C#
- The HUD's transform in C#
- The gauges lost their shadow
- The map tile walk in C#
- The object and creature walk in C#
- A verify pass replays, it does not re-run
- Stage 13 in C#
- Drawing the frame from another camera
- The compass needle is held to the tick
- The compass is carried with the view
- The gauges are carried like the needle
- The hooks on stage 13 are ordered by what they need
- A pass of the port's own

### [INPUT.md](docs/INPUT.md)

Pad, analog sticks, keyboard layout, mouse look and the menu pointer.

- Analog twin-stick control
- The Input pane is the port's
- The keyboard layout, and changing a default RecompOne provides
- Mouse look
- The menu pointer

### [AUDIO.md](docs/AUDIO.md)

The SPU mixer, its reverb and interpolation, XA resampling and the host output.

- What the audio path is
- Measuring it
- The reverb skipped the console's half-band filter
- An enhanced reverb sized from the game's registers
- Voice interpolation
- XA resampling
- The host output
- Positional audio

### [ENV_VARS.md](docs/ENV_VARS.md)

Every `KF2_*` switch the port reads, in one list.

### [RECOMPONE_FORK.md](docs/RECOMPONE_FORK.md)

How the vendored RecompOne checkout is kept, why it is not a patch stack, and what the merges to `0409bc2` and `d81dec8` decided.

### [RECOMPONE_PATCHES.md](docs/RECOMPONE_PATCHES.md)

Every change the port made to RecompOne, `0001`-`0072`, one entry each.

### [TODO.md](docs/TODO.md)

Next steps, and an index of what is reported but not diagnosed.

- Open questions — reported, not diagnosed
- Next steps

### [REMASTER.md](docs/REMASTER.md)

The authoring tools (identity, packs, editor) and the engine work for a remaster the user places, tunes, saves and shares. Phase 1 is in: a material per tile half or per face, from the working pack, through the editor (Shift+E) or `KF2_SHELL`; `snap` hashes the presented picture, and the switch is under Video ▸ Enhancements. Phase 2's first slice adds authored point and spot lights (`0071`).

- What this is, and what it is not
- What exists, and what each piece gives the remaster
- Identity: what authored data attaches to
- Architecture: five layers and the seams between them
- Data model and file format
- The editor
- Lighting: authored lights beside the game's own
- Level editing: what can be edited and what can only be decorated
- Rules every phase keeps
- The phased roadmap
- Phase 1, the first slice
- Phase 1, the second slice
- A tile half is a whole mesh, and a face is the key under it
- Faces, picked from the frame
- Phase 2, the first slice
- Open decisions

## Where to write a new finding

Same rule as before, one level down: **the finding goes in the document, not in
the commit message.** Pick by what a reader would be doing when they need it, not
by what the finding is about — a GTE fact learned while chasing a cull belongs
with the culls if that is where it will be looked for. If it fits nowhere, put it
in the general-purpose part of the nearest file rather than starting an
eleventh document.

Three markers are used where a passage's standing is not obvious from its prose:
**Confirmed** (measured, or read straight out of the emitted code), **Inferred**
(a reading that fits the evidence and has not been tested), and **Open**
(reported or suspected, not diagnosed). Keep the two states apart when you write
it down, the way these files already do:
**a mechanism that has been measured** and **a picture that has been looked at**
are different kinds of evidence, and most of the port's defaults turn on which
one a feature has. Mark what is inferred as inferred; a plausible story about a
branch has twice been wrong here (the pitch sign, and attack/use), and both times
the write-up is what made the correction cheap.
