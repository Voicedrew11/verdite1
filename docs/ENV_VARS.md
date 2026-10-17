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
| `KF1_OPENPACING` | `3` | vblanks a frame for OPEN.EXE's 3D present (the attract, its fades, the ending); `1` is the runtime's 60 (`docs/GAME_INTERNALS.md`) |
| `KF1_RATECENSUS` | off | seconds: the words that change on frames no stage ran |
| `KF1_TINTPROBE` | off | `1`: full-screen tints, and the largest flat semi-transparent primitive, on tick and idle frames, every 2 s (`docs/SMOOTHING.md`) |
| `KF1_VRAMCENSUS` | off | seconds: the VRAM blocks outside the display buffers that change between ticks |
| `KF1_VRAMCENSUS_AFTER` | `3` | seconds after GAME.EXE loads before `KF1_VRAMCENSUS` counts |
| `KF1_STAGEPROBE` | off | `1`: per stage A-I and the gate, calls a second, ordering-table words written, presents, every 5 s; and, when the in-game menu runs, its present and clear, and its writers' calls and cursor words per call, on a `menu:` line |
| `KF1_PRIMBUF_PROBE` | off | `1`: the frame's primitive buffer spent, peak and mean, and frames that ran out, every 2 s |
| `KF1_SEMI_ABR` | off | `1`: the blend mode each untextured semi model face's packet draws with, walked from the table at `DrawOTag` and checked against the GPU's own state; a line every 2 s (`patches/SemiAbr.cs`, "Semi-transparent model faces", `docs/GAME_INTERNALS.md`) |
| `KF1_GEOPROBE` | off | `1`: what each renderer draw call adds to the ordering table, by packet type, every 5 s (`patches/GeometryProbe.cs`) |
| `KF1_MAPCOVERAGE` | off | `1`: the address map's coverage by packet writer: packets, vertices, hit % a second and the table slots each writer used, and every non-polygon packet by command and slot, every 2 s (`patches/MapCoverage.cs`) |
| `KF1_DRAWCENSUS` | off | `1`: the primitive buffer bytes a frame spends, exclusive and inclusive, by renderer routine, every 2 s (`patches/DrawCensus.cs`) |
| `KF1_PACKETMATCH` | off | `1`: whether each primitive is found again in the previous tick's table, by context (models, map cells, other), once a second; `2` also the worst-matched and the moving contexts (`patches/PacketMatch.cs`, "Diagnostics on the GPU path", `docs/GPU_RENDERER.md`) |
| `KF1_TEXPROBE` | off | `1`: a line a second, on the console and into `texprobe.log`: the packet census (textured, flat, gouraud, semi), the retained scene's draws, triangles and instances, and the distinct words in each of the 32 texture pages (`patches/TexProbe.cs`) |
| `KF1_CROSSPROBE` | off | `1`: every presented picture from 400 ms before an area transition (`func_80036850`) to 3 s after it, read back and reduced to a row: luminance, repeat, frame time, the retained world's counters, the tick and position (`patches/CrossProbe.cs`) |
| `KF1_CROSSPROBE_OUT` | off | a directory (`1`: `scratch/crossprobe`): with `KF1_CROSSPROBE`, the same pictures as PNGs, the last 12 before the transition and every one after |
| `KF1_PROFILE` | off | `1`: the frame profiler, a section summary every 5 s; `panel` also opens its panel (Shift+P toggles it) |
| `KF1_PROFILE_OUT` | off | a path: every frame's sections as CSV rows, for `scripts/profile_report.py` |
| `KF1_PROFILE_SPIKE` | off | milliseconds: a console line for each frame whose work passes it |
| `KF1_PROFILE_FUNCS` | off | `stages`, or `game:hex+hex` to time more functions: an empty pre-hook makes each one a section |
| `KF1_RENDERER` | on | `0`: the recompiled renderer (stage I); `verify`: both, compared (`docs/GAME_INTERNALS.md`) |
| `KF1_CAMERABLOCK` | on | `0`: the recompiled camera block; `verify`: both, compared |
| `KF1_LITERAL` | on | `0`/`1`/`verify` for all 35 routines (the 24 draw routines and the 11 menu writers) at once; each `KF1_<NAME>` below overrides it ("The draw path in C#", `docs/GPU_RENDERER.md`) |
| `KF1_<NAME>` | on | one draw routine: `0` recompiled, `verify` both, compared. Names: `VERTEXCACHE`, `VERTEXCACHE_ARM`, `VERTEXCACHE_COMPASS`, `POLYASSEMBLER`, `POLYASSEMBLER_CACHED`, `MAPASSEMBLER`, `SPRITEASSEMBLER`, `HUDSPRITEASSEMBLER`, `MAPCELL`, `MAPWALK`, `SUBMIT_OBJECT`, `SUBMIT_INTERACTION`, `BANKMODEL`, `SUBMIT_SPRITE`, `SUBMIT_CREATURE`, `SUBMIT_PERSON`, `MODELWALK`, `ARM`, `COMPASS`, `HUDSPRITES`, `ITEMDISPLAY`, `POSEBLEND`, `POSEMARK`, `POSESWEEP` |
| `KF1_MENU_<NAME>` | on | one menu writer, as above; `verify` also runs its `MenuDraw` twin. Names: `FRAMEHEAD`, `QUAD`, `LINK`, `TEXT`, `DIGITS`, `NUMBER`, `PANEL`, `PANELINSET`, `WINDOW`, `PREBUILTA`, `PREBUILTB` ("The menu path in C#", `docs/GPU_RENDERER.md`) |
| `KF1_LITERAL_DUMP` | off | a directory: the entry, recompiled and C# RAM images at each routine's first verify mismatch |
| `KF1_LITERAL_PROBE` | off | `1`: each draw routine's C# runs every 5 s |
| `KF1_ITEMDISPLAY_HOLD` | on | `0`: the selected item's display steps on every frame under pacing, not once a tick |
| `KF1_ITEMDISPLAY_CARRY` | on | `0`: the item display's turn-out drawn at the tick's angle, not carried between ticks (`docs/SMOOTHING.md`) |
| `KF1_ITEMDISPLAY_PROBE` | off | `1`: a line a second while the turn is carried: frames carried, distinct angles drawn |
| `KF1_PAD_LAYOUT` | `west` | `jp`: the controller's face buttons as the disc has them (Triangle attack, Circle examine, Cross menu, Square item); `west` is the US releases' (Square attack, Cross examine, Circle menu, Triangle item) (`docs/INPUT.md`) |
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
| `KF1_GPU_WORLD` | `0` | `0`/`reference`: the packets only. `capture` (or `shadow`): the scene published to the runtime's `RetainedScene` beside the packets, nothing suppressed. `1`/`on`/`retained`: the map and models drawn by the GPU from meshes kept on it, their packets not written; turns the Z-buffer on; kept as `kf1.gpuworld` ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_MAP` | on | `0`: the map left to its packets ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_MODELS` | on | `0`: the models left to their packets ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_ARM` | on | `0`: the arm left to its packets ("The arm on the retained scene", `docs/GPU_RENDERER.md`); its runs otherwise drawn where the walk reaches their slots |
| `KF1_GPU_SPRITES` | on | `0`: the flipbook sprites and the creatures drawn as sprites left to their packets; a sprite is retained only while the world is drawn ("Sprites on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CHECK` | off | `1`: under `capture`, every map and model vertex placed as the GPU places it, checked against the GTE's vertex cache (screen position, depth, behind-the-eye agreement, the depth cue, the map's corner colours, and the blended poses), the map's fog inside its faces against DPCS, and the models' light at every corner against NCDT/NCDS; reported with the census ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CENSUS` | off | `1`: a line every 5 s: builds, cache hits, GPU draws, packet bytes per domain and fallbacks by routine, caller and reason ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_CENSUS_FILE` | off | a path: the census as JSON, every 5 s; implies `KF1_GPU_CENSUS` ("The retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_GPU_SURFACE_PROBE` | off | `1`: with the census, the main view's depth against its surface buffer, sampled every 5 s: pixels with a depth but no surface (`missing`), surfaces ahead or behind it, by surface id ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_PERPIXEL` | off | `1`: the retained world lit and fogged per pixel (`GteLightMap.Enabled`); kept as `kf1.perpixel` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_FOG` | `corners` | `corners`/`0`: the game's fog at the corners; `depth`/`1`: per pixel from its view depth; `distance`/`radial`/`2`: per pixel from its distance to the eye. The last two act through `KF1_PERPIXEL`; kept as `kf1.fog` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_AO` | off | `1`: ambient occlusion; kept as `kf1.ao` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_AO_NORMALS` | on | `0`: the occlusion's normals from depth differences instead of the geometry; kept as `kf1.ao.normals` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_AO_QUALITY` | `medium` | `low`/`medium`/`high`: the occlusion's resolution and samples; kept as `kf1.ao.quality` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_AO_PROBE` | off | `1`: the runtime's occlusion probe ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_ANISO` | `1` | `1`-`16`: anisotropic taps; `1` is off; kept as `kf1.aniso` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_MIPMAPS` | off | `1`: mipmapped textures on the retained world; kept as `kf1.mipmaps` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_ENHANCEDIST` | `0` | cells (2-16): per-pixel shading, filtering and occlusion only nearer than this, the game's look past it; `0` at every distance; kept as `kf1.enhancedistance` ("The picture on the retained scene", `docs/GPU_RENDERER.md`) |
| `KF1_RENDERDIST` | `0` | cells (1-30): the retained map and models drawn out to this distance; `0` the game's reach. Retained only; kept as `kf1.renderdistance` ("Render distance", `docs/GPU_RENDERER.md`) |
| `KF1_RENDERDIST_FADE` | `0` | cells (0.25-8): the world faded out over this band at the edge of what is drawn (the render distance, or the game's edge); `0` none. Retained only; kept as `kf1.renderdistance.fade` ("Render distance", `docs/GPU_RENDERER.md`) |
| `KF1_RENDERDIST_MODELS` | on | `0`: the models kept at the game's reach under `KF1_RENDERDIST` ("Render distance", `docs/GPU_RENDERER.md`) |
| `KF1_RENDERDIST_PROBE` | off | `1`: the game's edge when worked out, and a line every 5 s: cells and models added past the stencil ("Render distance", `docs/GPU_RENDERER.md`) |
| `KF1_KEYS` | `fps` | `stock` leaves RecompOne's keyboard bindings (`docs/INPUT.md`) |
| `KF1_MOUSE` | on | `0`: no mouse look |
| `KF1_MOUSE_TURN`, `KF1_MOUSE_LOOK` | `1.0` | the mouse's sensitivities |
| `KF1_MOUSE_INVERTY` | off | `1`: invert the look |
| `KF1_MOUSE_LEAD` | on | `0`: the view shows the mouse when the tick spends it |
| `KF1_MOUSE_BUTTONS` | `Triangle,Square,Circle` | left, right, middle, as pad buttons |
| `KF1_MOUSE_KEY` | `Escape` | the key that captures and releases the pointer |
| `KF1_ANALOG` | on | `0`: the sticks handed back to the pad bindings (`docs/INPUT.md`, "Analog sticks") |
| `KF1_ANALOG_LOOK`, `KF1_ANALOG_MOVE_ENABLE` | on | `0`: the right stick does not turn and look; the left does not walk |
| `KF1_TANK` | off | `1`: 3D controls, the left stick walking and turning, the right not read |
| `KF1_ANALOG_TURN`, `KF1_ANALOG_PITCH`, `KF1_ANALOG_MOVE` | `1.0` | the sticks' sensitivities; 1 is the D-pad's and L2/R2's rate |
| `KF1_ANALOG_DEADZONE`, `KF1_ANALOG_MOVEDEADZONE` | `0.15` | the deadzones; the first sets both unless the second is set |
| `KF1_ANALOG_CURVE`, `KF1_ANALOG_MOVECURVE` | `1.35`, `1.0` | the response curves |
| `KF1_ANALOG_EASE` | on | `0`: the right stick direct, without the game's ease in and out |
| `KF1_ANALOG_INSTANTSTOP` | off | `1`: the camera stops dead when the stick is let go |
| `KF1_ANALOG_ACCEL`, `_ACCELMAX`, `_ACCELTIME` | off, `2.2`, `0.5` | the look ramp past the D-pad's rate |
| `KF1_ANALOG_INVERTY`, `_INVERTTURN`, `_INVERTSTRAFE`, `_INVERTFWD` | off | `1`: invert that axis |
| `KF1_ANALOG_PROBE` | off | `1`: a line every 10 s of what the sticks asked and stage A did; `trace`: a line a tick as well |
| `KF1_WIDESCREEN` | off | `16:9`, `16:10`, `21:9`, a ratio, or `off` (`docs/WIDESCREEN.md`) |
| `KF1_WIDESCREEN_PROBE` | off | `1`: the margin census; `2` also lists screen-wide primitives |
| `KF1_WIDESCREEN_EFFECTS` | on | `0`: leave full-screen tints 320 wide |
| `KF1_CULLCONE` | on | `0`: the game's stencils at any aspect |
| `KF1_CULLCONE_PROBE` | off | `1`: cells drawn a frame, cells added |
| `KF1_TRANSLATION` | saved, off | `1`/`0`: the English fan translation this run, over the setting (`docs/GAME_INTERNALS.md`) |
| `KF1_TRANSLATION_PPF` | a `*.ppf` beside the disc | the translation's patch file |
| `KF1_DEBUG_NOCLIP`, `KF1_DEBUG_GODMODE`, `KF1_DEBUG_INFINITEMP`, `KF1_DEBUG_PEACEFUL` | off | `1`: the debug mod's switch on at boot, when the mod is enabled (`docs/MODS.md`) |
| `KF1_DEBUG_HOTKEYS` | saved, on | `0`: the debug mod's hotkeys off |
| `KF1_DEBUG_NOCLIP_SPEED` | saved, `6000` | noclip units a second |
| `KF1_DEBUG_SPEED` | off | a multiplier: the walk/turn speed multiplier on at that value |
| `KF1_DEBUG_ITEMS_PROBE`, `KF1_DEBUG_MAGIC_PROBE` | off | `1`: print the inventory or the spell book once in an area; `2` also gives one of each / learns all |
