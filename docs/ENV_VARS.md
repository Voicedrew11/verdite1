# Environment variables

Every `KF1_*` switch the port reads, in one list, with what each does and its
default, the way Verdite3's `docs/ENV_VARS.md` does. Add an entry here when a
switch is added.

| variable | default | what |
|---|---|---|
| `KF1_LOG` | off | the runtime's log channels: `bios,cd,gpu,dma,sdk,spu,mdec,irq` or `all` |
| `KF1_AGENT` | off | `1`: the state beacon, a JSON line a second (`docs/DEVELOPMENT.md`) |
| `KF1_SHELL` | off | `1` or a port: the command channel on `127.0.0.1:27901` |
| `KF1_AUTOSTART` | off | `new`: Start through the title into a New Game |
| `KF1_AUTOPAD` | off | scripted pad input, `seconds:button:holdMs,…` (`docs/DEVELOPMENT.md`) |
| `KF1_AUTOPAD_FROM` | `game` | the overlay whose load starts `KF1_AUTOPAD`'s clock: `open` or `game` |
| `KF1_FPS` | off | `60`, `144`, … or `off` (uncapped): frame pacing, the picture's rate (`docs/DEVELOPMENT.md`) |
| `KF1_TICKRATE` | `15` | the world's rate under pacing; the frame gate's own is 20 |
| `KF1_FPS_PROBE` | off | `1`: a pacing line a second |
| `KF1_PACING_NOBOUNDARY` | off | `1`: leave `DrawOTag` unhooked, to test the watchdog |
| `KF1_VBLANKPACING` | on | `0`: `VSync` calls outside the main loop's renderer call, and `DrawOTag`s with no `VSync` before them (the event picture), on the runtime's clock, a comparison |
| `KF1_VBLANKPACING_PROBE` | off | `1`: a line a second of held `VSync` and `DrawOTag` calls |
| `KF1_RATECENSUS` | off | seconds: the words that change on frames no stage ran |
| `KF1_TINTPROBE` | off | `1`: full-screen tints, and the largest flat semi-transparent primitive, on tick and idle frames, every 2 s (`docs/SMOOTHING.md`) |
| `KF1_VRAMCENSUS` | off | seconds: the VRAM blocks outside the display buffers that change between ticks |
| `KF1_VRAMCENSUS_AFTER` | `3` | seconds after GAME.EXE loads before `KF1_VRAMCENSUS` counts |
| `KF1_STAGEPROBE` | off | `1`: per stage A-I and the gate, calls a second, ordering-table words written, presents, every 5 s; and, when the in-game menu runs, its present and clear, and its writers' calls and cursor words per call, on a `menu:` line |
| `KF1_PRIMBUF_PROBE` | off | `1`: the frame's primitive buffer spent, peak and mean, and frames that ran out, every 2 s |
| `KF1_SEMI_ABR` | off | `1`: the blend mode each untextured semi model face's packet draws with, walked from the table at `DrawOTag` and checked against the GPU's own state; a line every 2 s (`patches/SemiAbr.cs`, "Semi-transparent model faces", `docs/GAME_INTERNALS.md`) |
| `KF1_GEOPROBE` | off | `1`: what each renderer draw call adds to the ordering table, by packet type, every 5 s (`patches/GeometryProbe.cs`) |
| `KF1_MAPCOVERAGE` | off | `1`: the address map's coverage by packet writer: packets, vertices and hit % a second, every 2 s (`patches/MapCoverage.cs`) |
| `KF1_DRAWCENSUS` | off | `1`: the primitive buffer bytes a frame spends, exclusive and inclusive, by renderer routine, every 2 s (`patches/DrawCensus.cs`) |
| `KF1_PROFILE` | off | `1`: the frame profiler, a section summary every 5 s; `panel` also opens its panel (Shift+P toggles it) |
| `KF1_PROFILE_OUT` | off | a path: every frame's sections as CSV rows, for `scripts/profile_report.py` |
| `KF1_PROFILE_SPIKE` | off | milliseconds: a console line for each frame whose work passes it |
| `KF1_PROFILE_FUNCS` | off | `stages`, or `game:hex+hex` to time more functions: an empty pre-hook makes each one a section |
| `KF1_RENDERER` | on | `0`: the recompiled renderer (stage I); `verify`: both, compared (`docs/GAME_INTERNALS.md`) |
| `KF1_CAMERABLOCK` | on | `0`: the recompiled camera block; `verify`: both, compared |
| `KF1_LITERAL` | on | `0`/`1`/`verify` for all 24 draw routines at once; each `KF1_<NAME>` below overrides it ("The draw path in C#", `docs/GPU_RENDERER.md`) |
| `KF1_<NAME>` | on | one draw routine: `0` recompiled, `verify` both, compared. Names: `VERTEXCACHE`, `VERTEXCACHE_ARM`, `VERTEXCACHE_COMPASS`, `POLYASSEMBLER`, `POLYASSEMBLER_CACHED`, `MAPASSEMBLER`, `SPRITEASSEMBLER`, `HUDSPRITEASSEMBLER`, `MAPCELL`, `MAPWALK`, `SUBMIT_OBJECT`, `SUBMIT_INTERACTION`, `BANKMODEL`, `SUBMIT_SPRITE`, `SUBMIT_CREATURE`, `SUBMIT_PERSON`, `MODELWALK`, `ARM`, `COMPASS`, `HUDSPRITES`, `ITEMDISPLAY`, `POSEBLEND`, `POSEMARK`, `POSESWEEP` |
| `KF1_LITERAL_DUMP` | off | a directory: the entry, recompiled and C# RAM images at each routine's first verify mismatch |
| `KF1_LITERAL_PROBE` | off | `1`: each draw routine's C# runs every 5 s |
| `KF1_ITEMDISPLAY_HOLD` | on | `0`: the selected item's display steps on every frame under pacing, not once a tick |
| `KF1_SMOOTH` | on | `0`: no camera carried between ticks under pacing (`docs/SMOOTHING.md`) |
| `KF1_SMOOTH_PROBE` | off | `1`: a view-smoothing line a second |
| `KF1_SMOOTH_MODELS` | on | `0`: the model walk's records and clips not carried under pacing |
| `KF1_SMOOTH_MODELS_PROBE` | off | `1`: a model-smoothing line a second: drawn and carried by table, poses, held cels, snaps |
| `KF1_NODITHER` | off | `1`: no ordered dither (`docs/PICTURE.md`) |
| `KF1_NODITHER_PROBE` | off | `1`: a dither line every 2 s |
| `KF1_TRUECOLOR` | off | `1`: 24-bit output |
| `KF1_PERSPECTIVE` | off | `1`: perspective-correct textures |
| `KF1_PERSPECTIVE_PROBE` | off | `1`: the address map's counters every 2 s |
| `KF1_SUBPIXEL` | off | `1`: sub-pixel vertices |
| `KF1_SUBPIXEL_PROBE` | off | `1`: the fractions carried every 2 s |
| `KF1_ZBUFFER` | off | `1`: per-pixel occlusion from the C# writers' packet depths, or the address map's when a world writer is recompiled |
| `KF1_ZBUFFER_PACKETS` | on | `0`: the address map's depths even when the packet records are complete |
| `KF1_ZBUFFER_PROBE` | off | `1`: every 2 s, packet depths recorded and found, triangles tested |
| `KF1_BLENDORDER` | on | `0`: blended surfaces in table order (`0079`) |
| `KF1_ZBUFFER_THRESHOLD` | `0` | restart the depth buffer on a step this large (`0051`) |
| `KF1_GPU_WORLD` | `0` | `0`/`reference`: the packets only. `capture` (or `shadow`): the scene published to the runtime's `RetainedScene` beside the packets, nothing suppressed. `1`/`on`/`retained`: the map and models drawn by the GPU from meshes kept on it, their packets not written; turns the Z-buffer on ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_MAP` | on | `0`: the map left to its packets ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_MODELS` | on | `0`: the models left to their packets ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_ARM` | on | `0`: reserved; the arm is always packets for now ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_SPRITES` | on | `0`: the flipbook sprites left to their packets; a sprite is retained only while the world is drawn ("Sprites on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CHECK` | off | `1`: under `capture`, every map and model vertex placed as the GPU places it, checked against the GTE's vertex cache (screen position, depth, behind-the-eye agreement, the depth cue, the map's corner colours, and the blended poses); reported with the census ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CENSUS` | off | `1`: a line every 5 s: builds, cache hits, GPU draws, packet bytes per domain and fallbacks by routine, caller and reason ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CENSUS_FILE` | off | a path: the census as JSON, every 5 s; implies `KF1_GPU_CENSUS` ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_KEYS` | `fps` | `stock` leaves RecompOne's keyboard bindings (`docs/INPUT.md`) |
| `KF1_MOUSE` | on | `0`: no mouse look |
| `KF1_MOUSE_TURN`, `KF1_MOUSE_LOOK` | `1.0` | the mouse's sensitivities |
| `KF1_MOUSE_INVERTY` | off | `1`: invert the look |
| `KF1_MOUSE_LEAD` | on | `0`: the view shows the mouse when the tick spends it |
| `KF1_MOUSE_BUTTONS` | `Triangle,Square,Circle` | left, right, middle, as pad buttons |
| `KF1_MOUSE_KEY` | `Escape` | the key that captures and releases the pointer |
| `KF1_WIDESCREEN` | off | `16:9`, `16:10`, `21:9`, a ratio, or `off` (`docs/WIDESCREEN.md`) |
| `KF1_WIDESCREEN_PROBE` | off | `1`: the margin census; `2` also lists screen-wide primitives |
| `KF1_WIDESCREEN_EFFECTS` | on | `0`: leave full-screen tints 320 wide |
| `KF1_CULLCONE` | on | `0`: the game's stencils at any aspect |
| `KF1_CULLCONE_PROBE` | off | `1`: cells drawn a frame, cells added |
| `KF1_TRANSLATION` | saved, off | `1`/`0`: the English fan translation this run, over the setting (`docs/GAME_INTERNALS.md`) |
| `KF1_TRANSLATION_PPF` | a `*.ppf` beside the disc | the translation's patch file |
