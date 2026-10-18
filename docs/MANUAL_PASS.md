# The manual pass

Everything the port has measured but nobody has looked at, in one list, in the
order to look at it. Each item says where to go, what to switch, and what the
counters already say, so the only thing left is the eye. Written 2026-10-16 at
the close of Step 8 of `RENDERER_PLAN.md`. The measurements behind it are in
"Whole-game coverage (Step 8)" in `GPU_RENDERER.md`.

**Every enhancement here is on by default** (2026-10-17, the user: on, so it can
be debugged in play, rather than off until judged), at the value Verdite3 judged
for its own; the defaults are in `ENV_VARS.md`. Each item ends with a decision:
keep it on, or change it. Record the answer by striking the item through here and
noting it in `TODO.md`; a changed default goes in its own commit after that. The
comparisons below still work: each switch turns off in Settings ▸ Testing.

## Setting up

```bash
dotnet build KingsField1Recomp.csproj -c Release
KF1_SHELL=1 dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
```

- **Settings ▸ Testing** holds every switch, live. The renderer (`World on the
  GPU ▸ Renderer`), the picture switches, frame pacing, the tick rate, smoothing,
  the aspect and mouse look are kept in `interface.ini` and come back next boot.
  A `KF1_*` variable that is set wins over the kept value (`ENV_VARS.md`).
- **Getting somewhere**: with `KF1_SHELL=1`, `python3 scripts/kf1sh.py spots`
  lists the named places below, and `python3 scripts/kf1sh.py spot area4-busy`
  goes there (about 6 s). `scripts/kf1sh.py warp <area> <x> <z>` goes anywhere,
  and `scripts/kf1sh.py poke 800A0790 e703e703` gives 999 HP (area 5's creatures
  hit for 155).
- **Comparing**: flip `Renderer` between *Reference packets* and *Retained*
  while standing still; nothing else changes with it except the Z-buffer, which
  *Retained* turns on.
- **Card A** holds your saves; the save screen writes it. The runtime keeps
  `carda.sav.bak` beside it each session.
- **Shift+P** opens the frame profiler, if anything feels slow.

| spot | where | why |
|---|---|---|
| `area1-start` | area 1 (31000, 4000) | the New Game's view, the HUD, the compass |
| `area1-door` | area 1 (46000, 68500), yaw 0 | Circle swings the double door |
| `area1-save` | area 1 (52850, 73500), yaw 0 | Circle opens the save screen; Cross leaves |
| `area1-sign` | area 1 (27000, 19700), yaw 0 | Circle shows the event picture; Circle closes it |
| `area1-exit` | area 1 (51000, 25000), yaw 0 | walk backwards (Down) onto the exit to area 3 |
| `area2` | area 2 (57000, 37000) | a low cell: arriving above the floor drops onto it |
| `area2-raised` | area 2 (61000, 113000) | the arrival `CrossProbe` saw swing in brightness |
| `area3` | area 3 (51000, 23000) | the exit's arrival |
| `area4-busy` | area 4 (79000, 71000) | the busiest frame in the game: 805 map quads |
| `area4-west` | area 4 (13000, 45000) | a raised cell |
| `area4-creature` | area 4 (17000, 72000) | a walking creature, for the carried poses |
| `area5` | area 5 (79000, 139000), entry 1 | the arrival room; the cached object writer; creatures |

## 1. The reference picture (Renderer: Reference packets)

The packet path is what the console drew, plus the switches below. Look at it
first: the GPU path is judged against it.

1. **Frame pacing and the camera carried between ticks.** Pacing on, 144, tick
   rate 15. Turn and walk at `area1-start`. Measured: 144.0 fps, 15 ticks a
   second, 0 view snaps on a walk. Look at: smoothness, any hitch at a tick.
   Then the tick rate at 20 (the console's) against 15. *Decide*: pacing's
   default, and 15 or 20.
2. **Creatures, objects and the arm carried.** `area4-creature`, then swing
   (Triangle). Look at: the creature's walk and the arm's swing between ticks,
   and the charge bar refilling after the swing. Turning, the compass needle
   (sub-pixel on and off).
3. **The menus and screens** at pacing on: Cross (the menu, 60 presents a
   second), the save screen at `area1-save`, the load screen (menu, Down x5,
   Circle). Look at: the cursor, the windows, the Yes/No.
4. **The sign's picture** at `area1-sign`. It fades in over 1.6 s at 60 a
   second; the console's speed is not known (it was bound by the GPU). Look at:
   whether the fade reads right. At 16:9, also the margin either side of it,
   which shows the last world frame.
5. **The area change**: walk onto `area1-exit`. Look at: the fade out and in.
6. **Death**: `area5` without the HP poke, and wait (4-11 s in the tours). The
   dimming steps 15-20 times a second while the fall is carried. Before
   2026-10-17 the fall itself jumped back on every tick (the mouse lead's
   clamp, "The mouse leads the tick" in `INPUT.md`), and on the retained
   world the map was rebuilt every tick, a 36 ms stall each
   (`GPU_RENDERER.md`). Look at: the fall, the switch to the packet map
   for the death, and whether the dimming's steps show.
7. **The picture switches**: Shading (Dither / None / Smooth), perspective,
   sub-pixel, the Z-buffer. Measured on the packet path: 94.7% of triangles
   depth-tested at 16:9, the arm and the HUD in painter's order. Look at:
   whether walls cut the arm with the Z-buffer on. On the packet
   path (`KF1_GPU_WORLD=0`) at a high render scale, the map at grazing angles for
   missing or extra faces, against `KF1_SUBPIXEL_CULL=0 KF1_QUAD_FACING=0`.
   *Decide*: each default.
8. **Widescreen**: 16:9 and 21:9 at `area4-busy`, turning. Look at: cells and
   models appearing at the sides, the HUD's place, the margin behind the card
   message and the sign. Then Testing ▸ HUD at the screen edges: the gauges, the
   numbers and the compass against the edges, and nothing left behind in the 4:3
   box. Then open the menu: its background tiled across the margin, and the seam at
the 4:3 edges. *Decide*: the default aspect, and whether the HUD goes to the edges.
9. **Mouse look** (Escape captures): the pitch direction, the sensitivity, and
   "Instant mouse look". *Decide*: the defaults.
10. **The translation**, if used: the save point's Yes/No.

## 2. The world on the GPU (Renderer: Retained)

The map and the models are drawn from meshes kept on the GPU. Measured
in Step 8 over every area and context, at every combination below: 0 missed
draws, nothing left to packets but the listed fallbacks, and the GPU's vertices,
fog and light within 1.83 px and 1 of 255 of the GTE's. What the counters cannot
say:

1. **The whole picture against the reference**, at every spot above, flipping
   `Renderer` standing still. Look at: seams, texture warping (the GPU path is
   perspective-correct), colours, the fog's edge.
2. **Retained since 2026-10-16**: the arm, area 5's objects (the cached
   writer) and the creatures drawn as sprites. Look at:
   - the arm while swinging at `area1-start` and against a wall: whether it is
     cut, and whether its shading matches `KF1_GPU_ARM=0`;
   - area 5's objects: their textures against the reference;
   - the creature sprites at `area4-creature`: whether they sit right.

   The HUD and compass stay packets by decision. Look at whether anything shows
   through or behind a wall it should not.
3. **Blended faces**: the semi-transparent models (area 2-5's domes and
   spheres, spell effects by their shape) drawn as blended faces at ABR 0. One
   mesh in area 1 (`800EED50`) and area 5's stay packets. Look at: whether they
   blend like the reference.
4. **An area's exit fade** on the retained path: walk onto `area1-exit`.
5. **`area2-raised`**: `CrossProbe` saw the luminance swing 39-177 on about a
   0.4 s period after this arrival, at both renderers. Look at: a flicker.
6. **Per-pixel lighting**, then **Fog** (corners, depth, distance; depth and
   distance need per-pixel). Look at: the light and the fog against the corner
   look.
7. **Ambient occlusion**, its quality and "Geometry normals". Look at: the
   creases at `area4-busy`, haloes at edges, cost.
8. **Anisotropic filtering** and **mipmaps**. Look at: floors at a glancing
   angle, shimmer.
9. **Enhancement distance**: the enhancements stop past it and the game's look
   returns. Look at: the boundary.
10. **Render distance** and its **fade**, at 16:9 especially, where it fills the
    side cells the game's stencils miss (about 124 cells added a frame at 16
    cells). The fog is black by 9.17 cells, so past that it adds only black at
    4:3. Look at: the sides as you turn, and the fade's edge.

*Decide*: whether *Retained* becomes the default renderer, and each picture
switch's default. The reference stays either way, for regressions and for
backends without the retained path.

## 3. Outside the game proper

1. **The attract**: about 25 s at 20 fps, then the title, held 17 s by a timed
   wait, every 50 s. Look at: the attract's speed. *Decide*: 3 vblanks a frame
   (the default) or 2 (`KF1_OPENPACING=2`, 30 fps).
2. **The ending** (`scripts/kf1sh.py poke 8009F847 01`, `scripts/kf1sh.py warp
   1 31000 5000`, then step off that cell and back onto it): OPEN.EXE presents for 4-6 minutes and does not return to the
   title. Look at: whether its last screen waits for a button.
3. **Dialogue as text** (on by default; Settings ▸ Testing ▸ *Dialogue as
   text*, or `KF1_MESSAGETEXT=0` to compare), with the translation on and off. Talk to anyone; the
   counters say 27 of 30 test pictures redraw pixel-exact. Look at: whether the
   text and its fade look the same as with the switch off. A glyph the tables lack is magenta in a box (*Mark glyphs the tables lack*); C03/T12032 has one (`KF1_MESSAGETEXT_TEST=C03/T12032` and Circle at `area1-sign`). *Decide*: whether it
   stays the default.
