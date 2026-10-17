# TODO

Next steps and open questions. The first task is Phase 2 bring-up: get the game
recompiled and running, following Verdite3's method but applied to this disc.

## Phase 2: bring-up

- ~~Find the executables and their load bases.~~ Done 2026-10-05: no
  `SYSTEM.CNF`, so `PSX.EXE` boots (fork `0090`); see "What is on the disc" in
  `docs/RECOMPILATION.md`.
- ~~Write `config/kf1.json`; sweep the function maps; identify the PSY-Q
  functions~~ (`VSync`, `PutDispEnv` and `DMACallback` by hand).
- ~~Recompile. Boot.~~ 1632 functions, 42 bindings; OPEN.EXE plays the attract.
- ~~Reach an area.~~ Three Starts reach GAME.EXE's first area, which runs at 20
  frames a second once the interrupt poll delivers the vblanks (fork `0091`;
  "The frame gate" in `docs/GAME_INTERNALS.md`). **Not looked at by eye.**
- ~~Walk, and change areas. Save and load through the memory card. Record the
  acceptance test.~~ Done 2026-10-05, by program: see "The acceptance test" in
  `docs/DEVELOPMENT.md`. **By eye, still to do**: the picture, the menus, the
  save and load screens, the area transition.
- ~~The HUD and the menus do not draw.~~ Reported from play 2026-10-05: their
  palettes wrap past VRAM's bottom edge and the GL backend dropped them (fork
  `0092`; "The HUD and the menus" in `docs/GAME_INTERNALS.md`). Measured, **not
  yet looked at by eye**.

## Phase 3: the enhancements

- **Frame pacing** (2026-10-05): built and measured, off until judged
  (`KF1_FPS`); menus wait a vblank under it.
- **The camera carried between ticks** (2026-10-05): built and measured, on
  under pacing (`docs/SMOOTHING.md`). So is everything the model walk draws,
  at its submitters, with clip times and the sprites' cels held to the tick;
  measured on animated objects, a person, a door and a staged falling pickup, and
  (2026-10-15) a creature walking in area 4. A monster's real drop is still to see.
- **The picture** (2026-10-05, `docs/PICTURE.md`): no dither, 24-bit,
  perspective, sub-pixel and a Z-buffer from the address map, built and measured,
  all off until judged.
- **Keyboard and mouse** (2026-10-05, `docs/INPUT.md`): the WASD layout and mouse
  look through stage A's own velocities, measured with a synthetic hand. To judge:
  the pitch direction, sensitivity, the lead. Twin-stick is not ported yet.
- **Widescreen** (2026-10-05, `docs/WIDESCREEN.md`): the margin, the tints and
  the cull stencils widened with the aspect, measured, off until judged.

- **The renderer and camera block in C#** (2026-10-05, "The renderer in C#" in
  `docs/GAME_INTERNALS.md`): verified, 0 mismatches over 8,588 frames with all
  twenty sites called; on by default. Next on the way to the retained renderer:
  the model walk and its five submitters in C# (with the smoothing passed in), then
  the pose routine `func_800205D4`, then the map path. Done 2026-10-14 as Step 1
  of `docs/RENDERER_PLAN.md` ("The draw path in C#" in `docs/GPU_RENDERER.md`):
  all 24 draw routines in C# and verified, and the selected item's display held
  to the tick. Step 2 done 2026-10-15: nothing a frame shows steps at the drawn
  rate. The event picture and the area-5 scene wait a vblank. There are no tints
  or texture animations to hold. **To judge**: the event picture's fade, now
  1.6 s against the console's (instant before; the console's is not known), and
  whether a death's dimming, which steps 20 times a second, shows under the carried
  fall. Step 3 built 2026-10-15 (`KF1_GPU_WORLD`, "The retained scene" in
  `docs/GPU_RENDERER.md`): the map and models drawn from meshes kept on the GPU,
  checked against the GTE. **To judge**: the picture at `KF1_GPU_WORLD=1`, the
  sprites and arm against the retained world, an area's exit fade, the
  semi-transparent models' blending (ABR 0, now drawn as blended faces). Done
  2026-10-15: the sprites as instances, the semi faces retained, and no dirty map
  update needed (a door changes no cell). **Open**: a lift or a gate that changes
  a cell, if one is found; the blend mode of mesh `800EED50` and area 5's semi
  meshes; area 5's fault after a tour of warps, which the plain recompiled game shares
  ("`warp`" in `DEVELOPMENT.md`), not re-run since the warp writes the floor.
  Step 4 built 2026-10-16 ("The picture on the retained scene" in
  `docs/GPU_RENDERER.md`), every switch off. **To judge**, at `KF1_GPU_WORLD=1`:
  per-pixel lighting, the fog modes (corners, depth, distance), AO and its
  quality, anisotropy, mipmaps, the enhancement distance, and the render distance
  with its fade (at 16:9 especially, where it fills the side cells the stencils
  miss). **Open**: `AoWorld` (a game-neutral fork patch for the grid's sizes, a KF1
  adapter, and which cells stop sight); `ProjectileLights` (identify the glowing
  effects first).
  Step 5 (water) not applicable, 2026-10-16: nothing scrolls, nothing textured
  is blended, and no blended mesh is a surface. **To say, if known**: any place
  that shows water or lava. Step 0's open items worked through the same day
  (`RENDERER_PLAN.md`). **Open**: the title's quiet 10 s a cycle; whether the
  ending waits on a button; what sets the ending's flag `0x8009F847` in play.
  **To look at**: the ending's last screen and the title's quiet stretch.
  Step 6 not applicable, 2026-10-16: nothing clips at the screen's edge, the
  menus clear to black, and the packet buffer peaks at 46.2% at 21:9. **To look
  at**: the margin behind the card message and the event picture at 16:9.
  Step 7 done 2026-10-16: the profiler's retained sections and `CrossProbe`,
  `TexProbe`, `PacketMatch` (`FrameCapture` not ported). **To look at**: area 2's
  arrival after a warp, where `CrossProbe`'s luminance swings 39-177 on a period
  of about 0.4 s (`KF1_CROSSPROBE_OUT` writes the frames). **Open**: the models'
  ordinal displacement in `PacketMatch` standing still.
  Step 8 measured 2026-10-16: every area, context and switch combination
  runs clean by program ("Whole-game coverage (Step 8)" in
  `docs/GPU_RENDERER.md`). **Next: the manual pass**, `docs/MANUAL_PASS.md`,
  which gathers every item above that is still to judge, with where to go and
  what to decide. Defaults change only after it. **Open**: 145 of 1,015,223
  blended faces noted but not drawn under the render distance in area 4.

Port what Verdite2 and Verdite3 have, mechanism by mechanism, in Verdite3's
order: the agent harness, frame pacing, the camera and models carried between
ticks, the picture (24-bit, perspective, sub-pixel, Z-buffer), widescreen,
keyboard and mouse, the debug mod, and the shipped launcher.
