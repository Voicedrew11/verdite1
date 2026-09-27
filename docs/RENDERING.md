# Rendering: recovering what the GP0 packet threw away

Everything in this file comes from one fact: **the GPU is handed no depth at
all.** The GTE does the perspective divide, the game writes the resulting 2D
screen coordinate into a GP0 packet, and what arrives at the GPU is screen
positions, UVs and colours with nothing left to say how far away any of it is —
so textures can only be interpolated affinely, vertices can only sit on whole
pixels, and occlusion can only be the order of the ordering table. Each of the
three is recovered here from the same discarded number, one step upstream in
`Gte.Rtp`.

Aspect ratio, the HUD and the culls are in [WIDESCREEN.md](WIDESCREEN.md).

| feature | mechanism | picture | default |
|---|---|---|---|
| Perspective correction | **measured**, 92% hit | **checked**, 76.6% of pixels | **on** |
| Sub-pixel vertex positions | **measured**, offsets uniform | **not checked** | off |
| Z-buffer | **measured**, same recovered SZ | **checked and still wrong** — a second cause remains | off |
| Ambient occlusion | **measured**, occlusion read back | **not checked** | off |
| Dithering (removal) | **measured**, all three routes | **checked**, twice-drawn pair | off (no crosshatch) |
| True color (24-bit) | **measured**, RGBA8 target + shader | the point of the switch | off (authentic 15-bit) |
| Anisotropic filtering | **measured**, sparkle sd 51.2 -> 11.4 | **not checked** | off |
| Per-pixel lighting | **measured**, every corner within 1 of the GTE, shader exact headless | **not checked** | off |
| Even fog and lighting | **measured**, clipped matches far tiles bin for bin; shared tile points within 3 (fog), identical (light) | **checked**, "looks good" | off |

That "mechanism measured / picture never checked" split is the rule the whole
port is written to: a feature whose mechanism has counters behind it but whose
picture nobody has looked at ships switched off, and the reason is recorded with
it.

## Perspective correction: the depth is one step upstream, and the screen position is the key

**Confirmed and on by default:** 92% hit rate measured, 76.6% of a frame's pixels
changed, HUD provably untouched.

The swimming, rippling texture on every floor and wall — the most recognisable
thing about a PlayStation picture — is not a bug in anything here. **The GPU has
no depth at all.** The GTE does the perspective divide, the game writes the
resulting 2D screen coordinate into a GP0 packet, and what arrives at the GPU is
screen positions, UVs and colours with nothing left to say how far away any of it
is. Linear interpolation of U and V across the screen is the only thing it *can*
do, and that is exact only for a surface square-on to the camera. Hence the
sliding texture on a floor you walk over, and the crease down the diagonal of a
quad where its two triangles disagree about where the middle of the texture went.

So it cannot be fixed at the GPU. It has to be fixed by not losing the depth in
the first place, and the useful observation is that **one function knows both
halves at the same moment**: `Gte.Rtp` computes the screen coordinate *and* the
view depth `SZ3` that produced it, in the same call. Better, the screen coordinate
it produces is bit-for-bit what turns up in the packet — `SatX`/`SatY` clamp it to
11 bits signed, which is exactly the 11 bits `GpuRaster.CoordX` decodes back out.

That makes the screen position a key the two ends can share. `GteDepth` is a hash
table of `(x, y) -> z` that the GTE writes as it projects and the GPU reads as it
decodes a vertex word, and it reunites the two halves **without following a single
register or store** — no tracking of the game's own copies, no knowledge of its
packet layout, nothing to identify. This is the cheap half of what PGXP does in
the emulator world; PGXP proper follows the values through memory, which buys the
cases below and needs far more machinery.

`Gpu._drawOffsetX/Y` is added *after* the lookup and the `RenderPrimEvent` fires
after that, so a hook is still free to move X — widescreen does — and the depth
stays attached to the vertex.

### Why leaving it on is safe: a miss is the old behaviour

Every route out of the table falls back to what the port did before, which is what
makes this a default rather than an experiment:

- **2D corrects itself.** A HUD sprite, a menu box, a font glyph — the CPU
  computed those coordinates and they were never in the table, so they miss and
  keep the affine mapping 2D actually wants. Measured: the title screen, which is
  entirely 2D, asks the table ~930 times a second and hits **0.0%**.
- **A primitive is all-or-nothing.** Correction is applied only when *every*
  vertex of the triangle hit. One vertex left at W = 1 among two real depths would
  shear the triangle's texture in half, which is worse than the problem.
- **Saturated coordinates used to be dropped, and that was the remaining pop.**
  A vertex projecting off screen clamps to ±1024, so several different vertices
  at different depths can share one key. The first version of the table refused
  to record those at all, which made every large nearby wall and floor — the
  polygons that want correction most — fall back to affine the moment one vertex
  left the window. The clamp is still the key, because that is what the packet
  carries; uniqueness is recovered by keeping the last few samples at each key
  and picking the set whose depths belong on this primitive. A leftover that is
  still an obvious high outlier is dropped, so that triangle stays affine rather
  than tearing. See "The table is not unique" below.
- **Nothing is corrected bit-differently.** A vertex with no depth carries W
  exactly 1 and the vertex shader writes the *original* expression for that case
  (`vec4(p, 0, 1)`, not `vec4(p*1, 0, 1)`), so untouched geometry lands on the
  same pixels to the last bit.

The table itself has no frame boundary in it and never gets cleared: entries carry
a monotonic sequence number and go stale once a table's worth of vertices — 16384,
about a frame or two of geometry — has been written past them. That deliberately
keeps double buffering, `DrawOTag` and the frame loop out of the file entirely.

### Both renderers, two different mechanisms

The software rasterizer interpolates U/W, V/W and 1/W with the barycentrics it
already has and divides per pixel. The hardware backend does not interpolate
anything by hand: it puts the recovered depth in `gl_Position.w` and lets the
rasterizer's own perspective divide do it, which is exact and free. Colour is
marked `noperspective` in the core-profile shaders so Gouraud shading stays as
flat-interpolated as the console's — the only thing corrected is the texture
coordinate. GLSL 120 has no `noperspective`, so on the **GL 2.1 backend alone**
colour is corrected along with the texture; that shows as a slightly different
Gouraud gradient on a steeply angled textured polygon, and that backend was not
available to test on (Mesa gives 4.6 here).

### What it measures, and what it looks like

`KF2_PERSPECTIVE_PROBE=1` reports the table's hit rate per two-second window. The
hit rate is the whole measurement: it is the only thing that says the coordinate
the GTE saturates into SX2/SY2 really is the coordinate that reaches the packet.
A rate near zero would mean the two ends never agreed on a key and every polygon
had quietly stayed affine.

Steady state in `fdat05`, walking:

```
[KF2] perspective: 46576 vertices projected/s, 91878 looked up/s, 92.3% hit, over 30 frames/s
```

— 85–94% in the world, **0.0%** on the title screen, and 30 fps throughout, so it
costs nothing measurable.

The picture: one ordering table drawn twice, once with the table live and once
with it switched off, so the pair is identical geometry, lighting and textures one
bit apart — the same trick "Getting pixels out without a screenshot" in
[DEVELOPMENT.md](DEVELOPMENT.md) describes for the dither.

| | affine (off) | corrected (on) |
| --- | --- | --- |
| pixels differing from the other | 76.6% | — |
| mean absolute difference | 22.7/255 | — |
| the HUD panel, glyphs and bars | identical | identical |

76.6% of the frame changing is what "the largest single change to the picture"
means numerically. In `fdat05` the stone wall in the middle distance goes from a
curved smear to straight courses of brick, and the vaulted ceiling stops sliding.

**Run the control before believing a pair.** Drawing the ordering table twice can
itself change the picture — a semi-transparent primitive blends twice on the
second pass — so the same shot was taken with *both* passes left on: **0.0% of
pixels differ, 0 in the HUD box**. That is what licenses reading the 76.6% as the
setting and not the method. It also turned a false alarm around: 3298 pixels
differ inside a 120x40 box over the HUD, which looked like the HUD being corrected
until the crop showed the panel, the glyphs and both bars pixel-identical and the
*wall showing through the semi-transparent panel* carrying all of the difference.

**Two things that do not work for the pair, both already learned from the dither
work** (and both written up with the method in [DEVELOPMENT.md](DEVELOPMENT.md)).
Consecutive frames of one run are different views, and the same frame
number in two runs is not the same view — disc timing drifts, and frame 120 was a
320-wide menu in one run and a 640-wide screen in the next. One ordering table,
twice, in one run is the only honest comparison.

## Sub-pixel vertex positioning: the same number's other half

**Confirmed mechanism, on by default.**

The depth is not the only thing `Gte.Rtp` computes and the packet does not carry.
The projection is done in **16.16 fixed point** — `sx` and `sy` in that function
are exact to a 65536th of a pixel — and then `SX2`/`SY2` keep the whole part and
drop the rest. The game copies the whole part into the GP0 packet, and so a vertex
that should drift a twentieth of a pixel per frame holds still for twenty frames
and then jumps a whole one. Every corner of a polygon jumps on its own schedule, so
the polygon twitches and shears between jumps; walk slowly towards a wall and its
edges crawl. That is the wobble, and it is the other recognisable half of a
PlayStation picture.

It is the *same discarded number* as the depth, one shift earlier in the same
expression, so it needs no new mechanism at all:

```csharp
int rx = (int)(sx >> 16), ry = (int)(sy >> 16);
...
if (GteDepth.Active)
    GteDepth.Record(nx, ny, sz, sx * (1f / 65536f), sy * (1f / 65536f), rx != nx || ry != ny);
```

`GteDepth` grew two floats per slot and a second switch; `Enabled` serves the depth
and `Subpixel` serves the fraction, one probe of the table either way. Everything
that makes the depth safe to recover makes the fraction safe too — a miss leaves
the vertex on the whole pixel the packet named, 2D never hits the table so the HUD
stays on the pixel grid it wants, and a vertex that saturated off screen is still
recorded for its depth against the clamped key, and left on the packet coordinate
so a shared edge does not open (see "The table is not unique"). **A vertex behind
the eye is dropped for both halves rather than one**, which is what keeps turning
the fraction on from changing which vertices carry a depth.

Note `>> 16` on a negative `long` floors, so `nx + fx` is the projected position on
the left of the screen exactly as it is on the right; a truncation-toward-zero
shift would have put the left half of every polygon a pixel out.

### The rule that is deliberately not carried over

Perspective correction is **all-or-nothing per primitive** — one vertex left at
W = 1 among two real depths shears the triangle's texture in half. The fraction is
**per vertex**, and the difference is what the two things are. W is an
interpolation parameter, so a corner disagreeing about it corrupts the whole
surface. A fraction is just where a corner is: a triangle with one corner moved a
half pixel is a triangle with one corner moved a half pixel.

The thing that could have gone wrong here is a **crack along a shared edge**, if
two triangles disagreed about where their common vertices are. They cannot: both
look up the same key and get the same answer, so a shared edge keeps identical
endpoints on both sides of it.

### The software rasterizer had to learn a finer grid

The hardware backend needed **nothing**. `HleVertex.X` has been a `float` the whole
time and `GlCore` passes it straight through, so adding the fraction in
`GpuHleForward.HV` is the entire hardware path.

`GpuRaster` is the one that walks whole pixels with integer edge functions, and it
now works in **sixteenths of a pixel** for any triangle where some vertex recovered
a fraction. What makes that a safe edit rather than a rewrite is that scaling every
coordinate by 16 scales the three edge functions and the area by 256 and leaves
every ratio taken from them — the barycentrics, the UVs, the Gouraud colours —
identical. So:

- a triangle where nothing was recovered runs at **shift zero**, which is the
  arithmetic the file always did, to the bit;
- the pixel is still sampled at its own coordinate, and the bounds are shifted back
  down with an arithmetic shift, which floors, so the covered pixels are the ones
  whose sample point lies inside the span;
- the `-1` fill-rule bias stays `-1`. It is applied to the edge *function*, not to
  a coordinate, so at either shift it breaks an exact tie on a shared edge and
  nothing else;
- 1024 pixels is the widest primitive the GPU accepts, so a coordinate stays under
  2^15 and the products stay far short of overflowing the `long` they already used.

`IsTopLeft` takes coordinates rather than vertices now, because by that point the
triangle is in the rasterizer's units and those may not be pixels.

**Exercising that path at all takes an edit.** `HostWindow` sets
`GpuHle.Active = _glBackend.Ready`, so on any machine where GL comes up — which is
every machine this has run on, Gl45 here — `GpuRaster.RasterTriangle` is dead code
and a change to it will be silently untested. Forcing it is one throwaway line in
`Program.cs`:

```csharp
Event.AddListener<RuntimeReadyEvent>(_ => RecompOne.Runtime.Hle.GpuHle.Active = false);
```

Both renderers were run that way for this: the software path holds 30 fps in the
attract demo and reports the same offsets as the hardware one.

### What it measures

`KF2_SUBPIXEL_PROBE=1` reports the **displacement**, not the hit rate — a different
question from the one `KF2_PERSPECTIVE_PROBE` asks, and it reads and resets only its
own counters so the two probes can be on at once without eating each other's
windows.

Steady state in the attract demo:

```
[KF2] subpixel: 47480 vertices/s carrying a fraction, mean offset 0.770 px, max 1.411 px, over 30 frames/s
```

**The mean is the measurement.** A point spread evenly inside a pixel sits
0.7652 of a pixel from that pixel's corner on average, and at most √2 = 1.4142 from
it. Measured across the demo: **0.760–0.773, max 1.413**. That is the recovered
fraction being a genuinely uniform fraction rather than a table full of zeroes or a
rounding artefact, and it is the number that says the low sixteen bits really were
being thrown away.

The hit rate is shared with perspective correction and is the same 90%: 47k
vertices a second recovered at 30 fps, so the second half costs nothing measurable
either. With the setting **off**, the perspective probe reports what it always did
(85–94%, 30 fps) — the extra table lookups only happen for untextured polygons when
the fraction is actually wanted.

### Why it is on by default

The "a miss is the old behaviour" argument that licensed
perspective correction covers this identically, and the mechanism above is
measured. The picture has been judged for shipping.

### Open: slits and grey texels with Sub-pixel on

Reported from play, with Sub-pixel on, and gone with it off:

- **A black slit** at a corridor's floor-wall junction and wall corner. Area 1, the
  shell's `goto 10530 -12800 87420 4091` after save 2 has loaded. Ordinary gaps also
  exist with Sub-pixel off; this one does not.
- **Save 2's guidestone shimmers** while it stands still and the camera turns, and
  **shows grey where its emblem should be gold**. With Sub-pixel off it wobbles, but
  no gold is missing.

None of the following fixed either, so all of it was removed rather than committed.
The measurements are what is worth keeping.

1. **A mixed polygon** (some corners with a fraction, some without). A counter
   read 0.0% mixed.
2. **A wrong fraction.** The branch recorded each corner's exact projection per packet,
   from the SVECTOR and the GTE's matrix in double, and compared it with the address
   map. Within 0.25 px on screen; off-screen corners up to about 1.5 px. **Placing
   corners from those records instead changed nothing by eye.**
3. **Welding** every fractional vertex on one whole pixel to one fraction per
   frame. Nothing by eye. So was a switch keeping fractions for only tiles, clipped
   fans or models; its result was confounded, because dropping one kind opens slits
   where it meets the others.
4. **The tile and model matrices are rounded before any vertex reaches them.**
   `func_80031950` `RotTrans`es the tile's camera-relative position under the camera
   matrix `0x80192E18`, so each tile's translation drops its low twelve bits. Its
   rotation is the camera's turned by exact quarter turns (`func_80014B88`).
   `func_80032588` does the same for a model's position less `0x80192E78`, and
   `MulMatrix2` (`0x8005BFD4`) of the camera pointer (its seventh argument) with the
   model's rotation truncates to 1/4096. Rebuilt in double from the same inputs, the
   translation equalled the register on every corner once truncated (0 refused of
   70k-114k a second). Measured on screen while turning, in the corridor and in save
   2's room: **neighbouring tiles disagree about their shared edge by at most
   0.07 px** (mean 0.01). Model centres moved at most 0.16 px, and model rotations
   rounded by 0/4096, those models being turned in quarter turns. Not the cause.
5. **The clipper's crossings are rounded by far more.** `ZClipFT` finds `t` from view
   coordinates already rounded, to 12 bits, and truncates the blended SVECTOR. Against
   the exact edge-plane intersection, over 550-780 crossings a second in the
   corridor: 30-42% under 0.25 px, 35-41% under 0.5, 14-21% under 1, 3-4% under 4,
   worst 12.9 px. A crossing sits on the view volume's side or near plane, so its
   error moves the far end of a fan edge along the screen border. Placing crossings
   exactly, with 4 and 5 together as the sub-pixel source, **changed neither report
   by eye**. `KF2_POLYASM=verify` stayed clean, and the cost was not measurable
   (850 fps uncapped either way).

**So exact geometry is ruled out as the fix for both.** The lead is texture
sampling: the prim shader reads one texel per pixel (`rawU`/`rawV` in `PrimFs`),
and the guidestone's emblem lines are about a texel wide at about a texel a pixel.
Whole-pixel corners hold the sampling grid still relative to the texture, so every
texel lands on a pixel. Fractional corners slide the grid, so a thin gold line is
skipped in favour of the grey beside it, differently each frame. The same slide at a
polygon's edge could read a transparent or dark texel as a slit. **Not measured.**
The check by eye: walk up to the guidestone until its texels are several pixels
wide. Missing gold that stops close up and returns at medium distance is sampling.
Anisotropic filtering does not help at about one texel a pixel, where it takes one
tap. The candidate fix is a sharp texel filter: nearest texels, with only a pixel
that straddles a texel boundary blending its two by coverage, in both prim shaders,
under Sub-pixel only. DuckStation's clamping each primitive's UVs to its own range
under PGXP is the other thing to try for the edge case.

**What was kept**: the shell's `goto <x> <y> <z> [yaw]` ("The command channel" in
[PATCHES_AND_MODS.md](PATCHES_AND_MODS.md)).

### Welding, micro-dilation and supersampling, tried and removed

Three things were built for the guidestone before its cause was found, measured,
and taken out again:

- **Radius welding**: every recovered vertex within 0.125 px of one already placed
  that frame snapped onto it, before the fraction was added. Save 2 standing, 144 fps:
  **0 of ~58k vertex references a second moved** at 0.125 px, because corners shared
  by tiles and models are already bit-identical (item 4 above); ~15% at 0.5 px. Its
  representative is whichever vertex the ordering table draws first, which moves.
- **Micro-dilation**: each opaque triangle's edges pushed out 0.25 px, texture
  coordinates kept on the corners. **By eye it brought the gold back**, very likely
  by covering the holes the whole-pixel cull left (below). It also stretches every
  solid polygon's texture and makes neighbours overlap.
- **Supersampling**: display targets at the render scale times 2-4 a side,
  box-filtered at present, VRAM at the render scale. Scale 6 at 2x2 made 5136x2880
  targets, held 144.0 fps at 20.0 ticks/s, and **did not stop the shimmer by eye**. At
  12 samples a game pixel a sub-pixel triangle is not all or nothing, so that ruled
  out a sampling rate and left a per-frame decision.

### The guidestone is 307 triangles, most of them a pixel or less

Save 2's guidestone still shimmers while walking to and from it, with the gold back.
A frame capture (Shift+F) standing in front of it: `func_80032588` submitted 307
flat textured triangles and 57 quads, every triangle on its own ordering-table
entry, and the replay drew **97 of the triangles at 0 pixels, 91 at 1 and 37 at 2**.
The replay is 1x; at render scale 6 each is about 36 samples. Whichever sample
lands inside such a triangle changes every frame as its fractions move, which is
what supersampling is for; it did not help (above). The Z-buffer was also on in that
session, and the emblem lying on or just in front of the stone would fight over
depth at a rate that changes with distance. **Not separated by eye yet.**

### A thin face was culled on whole pixels

**The guidestone's shimmer is geometry that was never drawn.** Neither SSAA, the
Z-buffer, dilation, lighting, fog nor smoothing touched it, and only Sub-pixel off
did, so it was a per-frame decision rather than sampling. The decision is the
backface cull: `Facing` and `Visible` (the C# models' and tiles' `NormalClip`) test
`Gte.Nclip` on the cached **whole-pixel** screen words and keep a face only when
`MAC0 > 0`. With whole-pixel corners that is consistent with what is drawn: a face
whose corners collapse to a line or turn over draws nothing either way. With
fractions it is not. A face a pixel wide collapses to a line on whole pixels at some
distances and not others, is culled, and leaves a hole between neighbours drawn at
their real corners, which is the stone behind showing through the gold; the reverse
case is drawn back to front.

`0052`: while sub-pixel is on (and not under `KF2_POLYASM=verify`), both tests take
the cross product of the fractional corners, read with `GteVertexMap.Peek`, and fall
back to the game's answer when a corner has no fraction or clamped. `Gte.Nclip`
still runs, so the GTE is left as the game leaves it. `KF2_SUBPIXEL_CULL=0` is the
comparison. The recompiled assemblers (Fast geometry off) still cull on whole pixels.

Measured with `KF2_SUBPIXEL_PROBE=1`, save 2, 144 fps, scripted: facing away and
still, the census counts ~1 polygon a frame whose fractional corners wind backwards
(before and after). **Facing the guidestone and still, the whole-pixel test would
have culled 23,040 faces a second that are front-facing at their fractional corners
(160 a frame, against 144 a second the other way); walking near it, 16k-35k a
second.** Without the change the census, which only sees what reaches the GPU, read
140-570 polygons a second drawn back to front while walking near it and none of the
faces that were missing. With it, polygons facing the guidestone rose from 75k to 98k
a second and back-to-front ones fell to 0 standing still; 180-570 a second remain
while walking, from corners the cull could not read. 144.0 fps at 20.0 ticks/s, no
exceptions. **Looked at: the guidestone no longer shimmers.** That was with
micro-dilation still in; the gold without it has not been looked at since.

### A floor quarter missing at a short edge

**The black triangle at a wall's foot is a quarter of a subdivided floor quad that
the game culled on three of its corners.** Reported from play with a frame capture
(`KF2_FRAMEVIEW_OUT`): rasterising the capture's own `verts` gives a hole at
`(118,106) (148,105) (132,109)`, so the gap is geometry never sent, not a crack
between sent polygons. Its neighbours are three clipped fans, each two packets from
one 4-corner polygon, and they are three of the four quarters `func_80030C94` makes
of one floor quad.

The subdivider splits a quad `(V0, V1, V2, V3)` (drawn as the loop `0, 1, 3, 2`)
into four, with new vertices at the midpoints of the four edges and of the diagonal
`V0-V3`, which stands in for the centre. Read from the recompiled routine, the
quarters are `(V0, M01, M02, M03)`, `(M01, V1, M03, M13)`, `(M02, M03, V2, M23)` and
`(M03, M13, M23, V3)`, all wound the parent's way; the partition has no hole. The
missing one is the last. Both culls in the pipeline look at only three corners:

- `func_800302E8`, the clipped-polygon emitter, runs `NormalClip` on the clipper's
  first three records and drops the whole polygon when it is not positive. For the
  last quarter those are `M03, M13, V3`.
- `Visible` and `Facing` (the assemblers' own cull) test a quad's first triangle,
  `0, 1, 2`, and drop the quad with it.

**When the parent's edge `V1-V3` is short on screen, `M13` and `V3` are a pixel or
two apart**, the corner `M03, M13, V3` is almost a straight line, and on whole pixels
it reads zero or negative while the quarter plainly faces the camera. Which way it
rounds changes with the angle and the distance, which is the "certain angles and
distances" of the report. This is the game's own cull and the hardware does it too;
the port only makes it visible at a higher resolution.

The first fix rotated `ClipOut` so the emitter's own test started at a corner that
faced, and kept an unclipped quad whose two triangles together faced, both on whole
pixels. It is superseded by the next section, which decides on the same area at
the fractional corners; the measurement below is from that first version.

Measured with `KF2_TILEWALK_PROBE=1`, area 1 at 144 fps, sweeping the shell's `goto`
over a 5×5 grid of positions a tile apart around save 2, eight headings each: up to
**49 clipped polygons a second** (a third of a frame's worth) that the first corner
would have culled while facing, and up to **162 unclipped quads a second** culled on
their first triangle while the quad faced — 80-100% of those with the first triangle
a sliver (`-first × 8 ≤ second`), so the same defect rather than twisted quads.
144.0 fps drawn at 20.0 ticks/s, `[present] wide 288`, no exceptions. **Looked
at: the reported gap is gone.** Worth checking against the two open reports above
it that also describe gaps at a floor-wall junction: "Ordinary gaps also exist with
Sub-pixel off", and the corners that drop at the sides of a wide picture
(`docs/TODO.md`).

### An edge-on wall lost its strips

**A dark vertical slit down the edge of a wall seen almost edge-on.** Reported from
play with a capture; rasterising the capture's `verts` on whole pixels finds no hole,
which is itself the clue. The wall's side face at that angle is a column of polygons
about a pixel wide — `138:122 137:104 137:104` — many of them a line on whole pixels
and a sliver at their drawn, fractional corners. The capture lists what was *sent*,
so the missing strips are not in it: they were culled before the GPU saw them.

`0052` moved the assemblers' own cull to the fractional corners, but only the cull
in `Visible`/`Facing`. **A clipped polygon is decided by `func_800302E8`, which runs
`NormalClip` on the first three records' whole-pixel screen words**, and the
previous section's fix worked on whole pixels too — no rotation can make a polygon
that is a line on whole pixels pass. So the port now makes the decision itself:
`ClippedFacing` (`patches/PolyAssemblerClip.cs`) takes the polygon's signed area at
the fractional corners (`GteVertexMap.Peek` on each record's `+0x18`, falling back to
whole pixels unless every corner has a fraction and none clamped), and a replace
hook on `NormalClip` (`0x8005DC6C`) hands the emitter that answer for the one call it
makes. The GTE still runs, so it is left as the game leaves it, and every other
caller gets the GTE's own answer. The decision goes both ways: a polygon whose
fractional corners wind backwards is dropped even when whole pixels would keep it,
which is `0052`'s rule. `QuadFaces` takes an unclipped quad's area the same way, on
the loop `0, 1, 3, 2`. `KF2_POLYASM_FACING=0` is the comparison; neither runs under
`KF2_POLYASM=verify`.

Measured with `KF2_TILEWALK_PROBE=1` over the same sweep as above (5×5 positions
around save 2, eight headings, 144 fps): the whole polygon's facing differed from
the emitter's first three whole-pixel corners for up to **80 clipped polygons a
second**, in 29 of 38 two-second windows; unclipped quads differed from `0052`'s
fractional first triangle for up to 26 a second. `KF2_POLYASM=verify`: 0 RAM,
register and GTE mismatches over every routine. Uncapped (`KF2_FPS=1000`, standing at
save 2) 889-897 fps against 887-901 with `KF2_POLYASM_FACING=0`, so the hook costs
nothing measurable. 144.0 fps drawn at 20.0 ticks/s, `[present] wide 288`, no
exceptions. **Looked at: the slit is gone, and the floor gap stays fixed.**

## The table is not unique: remaining wobble and the "far away" pop

The 90% hit rate was never "10% of vertices the two ends disagreed about". It was
almost entirely vertices the first version of the table **refused to record**:
anything `SatX`/`SatY` had clamped to ±1024. Walk up to a wall, turn past a long
floor, and one corner of a large quad leaves that window. All-or-nothing then
drops the whole primitive back to affine, and affine on a floor you are standing
on looks exactly like the camera jumped to the horizon — the foreshortening
vanishes and the texture lies down. The same primitive, a step later, has every
vertex on-screen again and pops back to corrected. That is the "suddenly far
away" texture.

The other half is a collision, not a miss. Screen position is the key because it
is what survives into the packet, but it is not unique. Two vertices of different
depths land on the same pixel constantly — a distant wall behind a nearby
column, two off-screen corners stuck on the same clamp — and last-write-wins
hands one polygon the other's W. A nearby surface that inherits a far Z is
interpolated as if that corner were at the horizon, which is the same picture,
only tearing instead of flattening. The fraction is stolen the same way, so an
edge jumps by up to a pixel every time the winner changes, which is wobble that
sub-pixel recovery cannot kill because the vertex is being given *someone else's*
fraction.

`patches/recompone/0011-gte-depth-collisions.patch` is the rest of the same
mechanism, not a new one:

- **Saturated vertices are recorded for their depth**, so a large nearby polygon
  can stay perspective-correct instead of falling back to affine. They are **not**
  moved off the clamp wall. The first version of this patch placed them at the
  GTE's true 16.16 position; any neighbour still stuck at ±1024 then failed to
  meet, which showed as gaps in the geometry. The packet coordinate is what the
  GPU would have drawn, and a shared edge has to agree with it. Only the
  [0, 1) fraction of an on-screen vertex is served, and it is a function of the
  key alone — two triangles that share a vertex look up the same fraction even
  when they pick different depths.
- **Each key keeps the last four samples**, not the last one. A primitive is
  bound all at once: newest-at-the-key is the first guess, then any key with
  several samples is rebound to the depth that sits with the rest of the
  primitive (closest in log Z to the geometric mean of the hits).
- **A leftover high outlier is dropped.** Two depths in geometric progression
  (a corridor floor at 100, 500, 2000) pass; a cliff (100, 120, 8000) is a
  collision, that corner loses its W, and the triangle stays affine rather than
  tearing. Only the far end is tested — a vertex next to the camera among two
  distant ones is legitimate.

2D still corrects itself: a HUD sprite was never in the table, so it still
misses. The title-screen 0.0% hit rate is the control that this has not started
correcting menus.

What this does *not* do is follow the value through memory. That is PGXP proper,
and it is what would recover a vertex the game copied, offset, or interpolated
on the CPU after `RTPS`. The remaining wobble after 0011, if any, is that case,
or a primitive whose colliding samples are all similarly wrong so the pick has
nothing true to choose.

`KF2_PERSPECTIVE_PROBE=1` now also reports `saturated/s`, `refined/s` and
`rejected/s`. Saturated is the extra vertices that used to miss on purpose;
refined is a collision the pick resolved; rejected is a cliff it refused.

## Following the value through memory: the address is the vertex

The heuristics above did not fix the collision, they scored it, and a wrong score
is not a slightly wrong texture — W is the denominator of the perspective divide,
so a corner given a stranger's depth throws its texture across the screen. The
same wrong pick hands a corner a stranger's sub-pixel fraction, which is a vertex
that snaps a pixel for no reason the player can see. Both were still happening
after `0011`.

So the thing the previous section called "PGXP proper" and put out of scope turned
out to be the smaller change, because the recompilation makes the association
findable. What the disassembly says:

- **A screen coordinate leaves the GTE only through `swc2`.** There is not one
  `mfc2` of SXY0/1/2 in the whole recompilation (`Gte.Read(12|13|14)` has zero
  call sites; `Gte.StoreWord(12|13|14)` has thirteen). It emits as
  `m.WriteU32(addr, Gte.StoreWord(14))` — **the destination address is in hand at
  the store, and C# evaluates the argument before the call**, which is the entire
  plumbing. No `InstructionEmitter` change, so nothing has to be recompiled.
- **The game keeps a transform cache.** `func_8005D8E8` (`RotTransPers`) and
  `func_8005D914` (`RotTransPers3`) write the coordinate wherever the caller
  points them, and the caller is a loop like `func_8002E650` filling an
  8-byte-per-vertex array at `0x8018EB94` with `{sxy, otz, fog}` for a whole
  vertex list at once. Polygons are assembled afterwards, out of that array —
  which is why "the newest depth at this pixel" carried no information about the
  polygon being drawn: the table held the entire scene at once.
- **The assembler copies the coordinate as a whole word.** In `func_80030540`,
  `c.V0 = m.ReadU32(c.S4); m.WriteU32((c.S0 + 0x8u), c.V0);` and the same again at
  `+0x14`, `+0x20`, `+0x2C` — `xy0..xy3` of a POLY_GT3/POLY_GT4. Load and store are
  adjacent instructions. Only the UVs and the CLUT go by halfword.
- **The packet reaches the GPU from an address the runtime knows.**
  `LibGpu.DrawOTag` and `Dma.TransferGpu` both do
  `gpu.WriteGp0(m.ReadU32(addr…))`.

`patches/recompone/0012-exact-gte-vertex-map.patch` connects those four facts.
`GteVertexMap` is a map from **RAM word address** to `(z, fx, fy, the packed XY
word)`, filled by three exact hops:

1. `Gte.Rtp` keeps the depth and the truncated 16.16 fraction per screen-coordinate
   FIFO slot, shifted with `SX`/`SY`, so a read of SXY0/1/2 hands out the numbers
   belonging to *that* slot. `Gte.Read` of one of those registers publishes
   `(value, attributes)` into a small pending ring.
2. `PSMemory.WriteU32` of a value sitting in that ring binds the destination
   address to those attributes; `PSMemory.ReadU32` of an address the map knows
   publishes it again, so the attributes follow the game's `lw`/`sw` out of the
   transform cache and into the packet. A store with no match *clears* the
   destination, so a rewritten word stops answering.
3. `Gpu` keeps `_fifoSrc` beside `_fifo` — the address each command word was read
   from — and `DrawPolygon` asks the map for each vertex by its own address,
   **verifying the stored word against the word it is about to draw**.

The ring is what avoids tainting registers, which would have meant instrumenting
every instruction the recompiler emits. It is searched newest-first, preferring an
entry nothing has taken yet, so three `swc2`s of three coordinates that clamped
onto the same pixel still bind in the order they were stored.

Loading a coordinate *back into* the GTE invalidates the slot (`Write` cases
12-15). `func_8005DC6C` is `NormalClip` and hands all three vertices of a polygon
back for the cross product; without that, a later read would publish a stale depth
against a value that matches.

What this buys, measured at the attract-mode flythrough with
`KF2_PERSPECTIVE_PROBE=1`:

```
52k vertices projected/s, 52k caught/s, 87k copied/s, 94k looked up/s, 92.5% hit
```

`caught` equals `projected`, so every coordinate the GTE produced is picked up;
`copied` is half again as many, which is the assembler re-reading a shared vertex
for each polygon that uses it. The **92.5% hit rate is the same as the screen
position table's 92.0%** on the same scene (`KF2_PERSPECTIVE_FALLBACK=1` reports
both), so exactness costs no coverage — and the position table's figure was never
all correct answers, since a HUD quad it "hit" was being handed some 3D vertex's
depth. The remaining ~8% is 2D and anything the CPU computed, which wants affine.

`KF2_PERSPECTIVE_FALLBACK=1` keeps the old table filling and consults it for
vertices the map missed. It is off by default and exists to A/B the two in one
build; `refined/s` and `rejected/s` are gone from the report because there is
nothing left to pick between.

**Open, and this is now an option:** the cave section shows polygons alternating in
front of and behind each other. The map changes W and the sub-pixel position, and
the draw order is the game's ordering table, which the GPU walks back to front with
no depth buffer at all. Two coplanar surfaces the game sorted by a single OTZ per
polygon will flicker on hardware too. `patches/recompone/0014` is a Z-buffer from
the same recovered SZ; it is off by default until the picture has been looked at
in that cave. See "Z-buffer".

The cost lands on `ReadU32`/`WriteU32`, which is the hottest path in the port, so
it is gated twice: on `GteVertexMap.Active` (a static bool, false when perspective
correction, sub-pixel positioning and the Z-buffer are all off) and then on one bit of a
presence bitmap — 64 KB for the retail 2 MB of RAM. The attribute array itself is
10 MB, allocated on first use, and only touched on a bitmap hit. The frame rate
does not move.

### The RAM fast path went round both hooks

**Symptom, after the merge to upstream `0409bc2`:** textures affine and vertices
back on whole pixels, with every switch still reporting itself on — `[KF2]
perspective: on`, `[KF2] subpixel: on`. `KF2_PERSPECTIVE_PROBE=1` names the
failure exactly: `36720 vertices projected/s, 0 caught/s, 0 copied/s, 91872
looked up/s, 0.0% hit`. The GTE was projecting and the GPU was asking; nothing in
between was being *bound*.

**Cause.** Upstream added RAM fast paths to `PSMemory.ReadU32` and `WriteU32` —
an `Unsafe.ReadUnaligned`/`WriteUnaligned` straight into the array, taken by every
`lw` and `sw` in the game, returning before `ReadU32Slow`/`WriteU32Slow` is
reached. `GteVertexMap.NoteRead` and `NoteWrite` live in those slow paths, and the
merge kept them there. This whole mechanism *is* following a value through the
game's `lw`/`sw`, so a fast path that skips the hooks skips the mechanism: nothing
is ever published to an address, so every `TryGet` misses and both halves silently
fall back to what they do on a miss — affine, and the whole pixel.

**Fix:** offer the word to the map in the fast path too, still gated on the same
`GteVertexMap.Active` static bool, so the fast path keeps its speed and the
association is made where the game actually makes it.

**What the counters say to look at.** `Roots` (`caught/s`) reading exactly zero
while `projected/s` is healthy means the *store* side is not being seen; the hit
rate alone would not distinguish that from a game that stopped copying vertices.
Measured after, in area 2 at 144 fps: `364896-383616 projected/s, 369648-388368
caught/s, 465984-524100 copied/s, 93.0-93.7% hit`, inside the 92.2-97.1% band this
mechanism was first measured at, and 144.0 fps at 20.0 ticks/s with it on.

### What the map costs, and the filter in front of it

The map is paid by every feature that turns on `GteDepth.Active` — perspective,
sub-pixel, the Z-buffer and ambient occlusion alike — and it lands in **stage 13's
self time**, because the renderer makes most of the frame's `lw`/`sw`. Measured with
the frame profiler, slot 2 standing still, `KF2_FPS=1000`: stage 13 self 0.273 ms
with the map off, 0.505 ms with AO on and 0.503 ms with perspective on — AO's own
share of that was nothing (16 samples and 1 sample read the same), it was only
switching the map on. A capped run hides the frame-rate side of this; measure
uncapped.

Two cuts, neither changing what binds:

- **`NoteWrite` scans the 8-slot ring only when a match is possible.** Nearly every
  store carries no published value. It returns early when the newest pending entry
  is past `PendingMaxAge` (so all are), or when the value's bit is clear in a 64-bit
  hash mask of published values — a superset, so a stale bit only costs a scan. The
  mask empties once the whole ring has aged.
- **The `ReadU32` fast path tests the presence bit inline** (`MaybeBound`) and calls
  `NoteRead` only for a marked address.

After: stage 13 self 0.374 ms (AO) and 0.376 ms (perspective), about 55% of the
map's cost gone; 552 → 630 fps with AO on, 525 → 635 with perspective on; 0.281 ms
and ~700 fps with the map off, unchanged within noise. The probe is identical —
63.0% hit, 255 vertices projected and caught per frame, before and after.

## Z-buffer: the same depth, used as occlusion

**Confirmed mechanism; picture checked and still wrong under the address map. Reopened
on the assemblers' own depth, which has not been looked at** — see "The assemblers
write the depth" at the end of this section.

The GPU has no depth buffer. The game sorts every polygon into an ordering table
by one number — the GTE's OTZ, the average of its vertices — and `DrawOTag` walks
that table back to front. Two surfaces that actually interpenetrate can only take
turns in front of each other, because each polygon is wholly in front or wholly
behind. That is the cave flicker noted at the end of "Following the value through
memory" above, and it is what a Z-buffer turns off.

The depth is the same SZ3 perspective correction already recovers. Nothing new is
caught; the rasterizer is just allowed to test it per pixel instead of throwing
it away after the texture divide. `GteVertexMap` already follows the word from
`Gte.Rtp` into the packet, and `DrawPolygon` already asks by the address the
coordinate was read from. `patches/recompone/0014` is the rest:

- **All-or-nothing per triangle**, same rule as W. A corner left without a depth
  among two real ones would punch a hole, so that triangle keeps painter's
  order.
- **2D never hits**, so the HUD, the menus and the death fade still draw on top
  in table order with the depth test off.
- **Semi-transparent tests and does not write**, so two overlapping additives
  still blend in the order the table named. They are drawn after the opaque
  geometry the table put behind them, or that geometry paints over them (`0079`;
  "Water was painted over by what lay under it" below).
- **Untextured geometry is tested too.** Perspective correction only cares about
  textured polygons; a flat-shaded wall still has a view depth. `HasPersp` and
  `HasGteZ` are independent on `HleVertex` so putting SZ into clip W does not
  turn perspective correction on as a side effect.
- **Equal depths prefer the later table entry** (`GL_LEQUAL` / `>` reject), which
  is the painter's-algorithm tie the console had, so coplanar surfaces the game
  stacked on purpose keep their order.
- **The hardware path** attaches a 24-bit depth renderbuffer to each display RT
  and writes window depth `SZ/65536` from the fragment shader. Clip-space Z stays
  0, same as before this existed: these vertices are already projected, and
  putting SZ into `gl_Position.z` lets OpenGL clip them against a far plane the
  GPU never had — a hard line across the floor, far closer than the game's own
  fog. A vertex with no depth still emits `vec4(p, 0, 1)`, bit-identical to
  before. The first draw onto an RT after `Present` — or after the setting is
  flipped — clears the attachment, because this game's `PutDrawEnv` has `isbg=0`
  and would otherwise test against last frame.
- **The software path** keeps a float per VRAM pixel and tests it in the same
  inner loop that plots. Punch-through (texel 0) and a mask-bit reject skip the
  write, so a hole in the texture does not occlude what is behind it. Dead on
  any machine where GL comes up, as `GpuRaster.RasterTriangle` always was.

`KF2_ZBUFFER_PROBE=1` reports triangles tested against painter's-order fallbacks
per two-second window. The tested rate is the measurement: a rate near zero would
mean every triangle quietly kept the ordering table. Pixel rejects are a
software-rasterizer number and stay at zero on the hardware path.

**On by default.** The recovered number is the one perspective correction already
measures at 92% hit. A twice-
drawn ordering table cannot take this pair, because the second pass would fail
every test against the first. The cave is still the test.

**There is no longer a user-facing switch.** The player-facing checkbox was
removed from Video: recovering a usable depth here is effectively unbridgeable —
DuckStation's mature PGXP depth buffer, given the same per-polygon OTZ averages
this game submits, cannot produce a clean picture either, so offering a switch
that only ever half-works is worse than not offering it. The mechanism stays for
diagnosis, driven from the console alone: `KF2_ZBUFFER=1` forces it on for the
run and `KF2_ZBUFFER_PROBE=2` takes the census below. `patches/ZBuffer.cs` and
`patches/recompone/0014` are unchanged; only `patches/settings/ZBufferPage.cs`
and its registration are gone. That verdict was reached on the address map's depth,
before the assemblers were in C#; "The assemblers write the depth" below is the
reason to look again. **The switch is back** as `patches/settings/ZBufferPage.cs`
under Video ▸ Enhancements, on by default, with two sliders for the coplanar
tolerance's terms (`kf2.zbuffer.bias`, `kf2.zbuffer.slope`, 0-8 SZ and 0-4 px) and a
button that restores 1 and 0.5. The env vars still win for the run.

### The clear landed at the tail of the frame, not the head

Reported symptom: with the Z-buffer on, a large region of the picture shows the
background instead of the geometry in front of it — the sky drawn over houses and
walls a few metres ahead outdoors, a black hole in a cave. It moves with the
camera and it is not every frame.

`GlCore.PresentDisplay` opened with `_frame++; Flush();`. The depth clear above
keys on `rt.LastDrawFrame != _frame`, so incrementing first made that **trailing**
flush — the tail of the frame that is ending — look like the head of the next
one:

1. Frame N's draws stamp `LastDrawFrame = N`.
2. `PresentDisplay` sets `_frame = N+1`, then flushes. The guard sees `N != N+1`,
   **clears the depth buffer**, draws the last batch, stamps `LastDrawFrame = N+1`.
3. Frame N+1's first real draw sees `N+1 == N+1` and **skips its clear**.

So the clear happened one flush too late and suppressed the one that mattered.
Frame N+1 began with whatever depth frame N's trailing batch left. Nothing
rescues it: `PutDrawEnv` has `isbg=0` here, so there is no game-side full-screen
fill to reach `FillRtFull` and clear the attachment by another route. The trailing
batch is usually the 2D HUD, which does not write depth — that case leaves the
buffer wiped and looks correct, which is why the fault is intermittent. When the
last batch is 3D it stamps *near* depths, and the next frame's geometry is
rejected wherever they landed, leaving the earliest-drawn thing — the far
background — on screen. `patches/recompone/0016` swaps the two statements.

**The confirmation is a counter, not a screenshot.** `KF2_ZBUFFER_PROBE=2` reads
the depth attachment of the target the frame's depth batches actually went to and
prints a 32×16 min-per-cell map. Before: **67 of 91 blocks read `nothing
written`** on a target taking ~1500 depth batches a window, which had been written
off as a broken instrument. After: **11 of 11 populated**, a smooth near-to-far
gradient with no untested cells. The empty reads were the defect — the buffer was
being cleared at the end of every frame.

Two theories died on the way, both on the census's own evidence, and both worth
not re-deriving:

- *An early-sorted primitive claims a near depth.* Ruled out by `head` and
  `largest`: the far end of the table holds far depths (`ot 7492, z 5002..6088`)
  and the near end holds near ones (`ot 8490, z 803..3047`), in every area
  measured. The frame-wide maxima (22266, 23215) sit on small primitives.
- *A big surface wins the depth test against another.* `nothing is entirely in
  front of anything the table put nearer`, every block. The blocker was never in
  the frame.

The ordering table and the recovered SZ agree, so the "where they disagree, trust
the table" fix sketched for this would have had nothing to act on. **Note the OT
length varies per area** (8348, 8898, 9101, 9162, 9315 measured), so an `ot` is
only comparable inside its own frame.

`patches/recompone/0015` is the census itself: `GteDepth` keeps every polygon's
bbox, depth range, table position and flags for the window; `LibGpu.DrawOTag` and
`Widescreen`'s replacement of it publish the walk position (`OtEntry`, counted
from the far end); `GlCore` remembers which RT the depth batches went to, since
the presented one is last frame's under double buffering and the most recently
drawn one may have just been cleared by a fill. Diagnostic only.

**This did not fix the picture, and the cause turned out to be the paragraph at
the end of this section.** See "The clip W was the Z-buffer's second cause" under
"PGXP" below: `vDepth` was interpolated screen-linearly on every triangle whose
texture was not being corrected, which is most of the architecture. What follows
is the state of the investigation before that was found, kept because it is what
ruled the other explanations out.

Checked by eye after `0016`: the sky still
draws over walls a few metres ahead. So the clear timing was a real defect —
the buffer measurably did not survive its own frame, and now does — but it is
not the cause of the reported symptom, or not the only one. A second cause
remains; that unresolved symptom, together with DuckStation showing the same
class of problem is unbridgeable even with a proper PGXP depth buffer, is why the
user-facing switch was retired and the mechanism kept for diagnosis only.

That rules out a whole class of explanation, which is worth keeping: **the
depth the world is being tested against is now known to be this frame's**. Any
remaining theory has to work with a correctly cleared buffer, correct per-frame
depths, a table position that agrees with the recovered SZ everywhere measured,
and no primitive standing entirely in front of one the table put nearer. What
has *not* been measured is the geometry the depth is interpolated across
between the vertices — every census number above is per polygon, taken from its
corners, and the map is a 32×16 minimum-per-cell reduction. Screen-linear
interpolation of a view depth is wrong (it is 1/z that is linear in screen
space), which biases a polygon's interior; whether that bias is large enough to
lose a wall in front of the sky is the next thing to measure, not to assume.

### The assemblers write the depth

**Mechanism measured; the picture has not been looked at.**

Closing this as unbridgeable rested on one assumption: that a depth buffer only
ever sees GP0 packets, which is all DuckStation sees. It stopped being true when
the map tiles, the models, their vertex transforms and the view-space clipper
moved into C# (`PolyAssembler*.cs`). The port now *builds* nearly every packet
that should occlude, so it knows two things the address map could only guess:
every corner's depth, and which routine asked for the packet.

`patches/recompone/0050` adds `GtePacketDepth`, a side table keyed by packet
address in the same shape as `GteLightMap` (`0048`): four corner depths, checked
against the command word and the first and last vertex words before they are
believed. The assemblers fill it and `DrawPolygon` reads it. **While it is active
the depth buffer takes a packet's record or nothing**; perspective correction and
the sub-pixel fraction still come from the address map.

- **The transforms** (`func_8002E650`, `func_8002E7CC`) note each cache slot's SZ3
  beside the two cache words they wrote. An assembler takes a corner's depth only
  while the slot still holds those exact words, so a slot something else rewrote
  gives no depth rather than an old one.
- **The tile assemblers** (`func_80030540`, `func_8002FECC`) and **the lit model
  assembler** (`func_8002F214`, `func_8002EAEC`) record every packet they
  finish. A packet with a corner at SZ 0 is not recorded.
- **A clipped fan** (`func_800302E8`'s output, packet k being records 0, k+1 and
  k+2) takes each record's view-space Z at `+0x10`. The clipper projects that same
  number, and it is there whichever clipper ran. A packet whose vertex words are not
  its records' screen words is not recorded, and is counted.
- **Not recorded**: the HUD builder's icons (`InHud`, set by `PerPixelLighting`'s
  hook) and the first-person arm (`InArm`, a pre/post on `func_80032400` in
  `ZBuffer.cs`). Their addresses' old records are dropped so a stale one cannot
  match. With no depth they keep painter's order, which is what they had on the
  console.

**The skybox needs no special case under this source.** It never passes through
these assemblers, so it records nothing and draws first in painter's order, and
every wall after it passes the test. `GteDepth.OtSlot != 0` is still applied. The
same goes for anything else these routines do not draw — a miss is painter's
order, as it always was.

**Coplanar surfaces are the next section.** Reported from play at once: at the seam
between two wall panels the two quads visibly fight.

Measured in area 1 at slot 2's restore point, standing, 144 fps: 17,136 packet
depths recorded a second and 17,136 polygons finding theirs, 23,616 triangles
tested. The ~9,500 a second with no record are the HUD and the other 2D, and 0
clipped packets were unmatched. The address map tested the same 23.6k from the same
spot. It tested more only while the area was loading and the camera turning, and
the extra triangles were all GT4s linked in the far end of the table, which is where
the sky is. Frame rate unchanged, 144.0 fps drawn at 20.0 ticks/s.

`KF2_ZBUFFER_SOURCE=map` is the comparison. The source needs Fast geometry: with
the assemblers recompiled (Video ▸ Fast geometry off, `KF2_POLYASM=0`,
`KF2_POLYASM_TRANSFORM=0`, or PGXP's CPU tracking forcing them back) nothing
records, so `ZBuffer.SyncSource` falls back to the address map rather than to
testing nothing. The source also feeds ambient occlusion, which shares the depth
attachment, so the AO far-plane mask now covers everything that records nothing.
The `KF2_ZBUFFER_PROBE=1` line reads `packet depths recorded/s, polygons found
theirs/s, had none/s, clipped packets unmatched/s`.

### Coplanar panels fought at the seam, and the tolerance that gives it back to the table

**Mechanism measured; the picture has not been looked at.**

Reported with a screenshot: in the seam between two wall panels, a strip where the
two quads take turns pixel by pixel. They overlap there and lie in one plane, so the
test is comparing two depths that should be equal. `GL_LEQUAL` gives the later table
entry an exact tie, and an interpolated float never produces one, so it came down to
noise. On the console the table decided it, because the later quad simply drew over.

Two causes, and `0051` plus the port's half deal with both:

- **The corners were whole SZ units.** RotTrans drops the low twelve bits of the
  view Z, and two tile meshes under different translations round the same plane's
  corners differently, tilting each panel's depth plane by up to half a unit.
  `PolyAssembler.Unrounded` recomputes the view Z from the SVECTOR, the rotation's
  third row and `TRZ`, `(TRZ·4096 + R3·V) / 4096` in double, for the transforms'
  vertices and the clipper's records. A result more than 1.5 from the game's own Z
  means the matrix moved since the projection, and the whole Z is kept instead.
  Measured standing in area 1: 37,872 corners a second unrounded, **0 left whole**,
  clipped fans included.
- **The test had no tolerance.** That is what fixes it, because even exactly
  coplanar surfaces interpolate differently across different triangles. A tested
  fragment now compares a depth pulled towards the camera, by `GteDepth.DepthBias`
  SZ units (1) plus `DepthSlope` (0.5) times the depth's change across a pixel
  (`dFdx`/`dFdy`, so a grazing wall gets more). A surface within that distance of
  what is stored draws, and the later table entry wins, as on the console.

**The bias is on the test, never on the write.** If both surfaces wrote biased
depths they would tie again. GL cannot test one value and write another, so an
opaque tested batch is drawn twice. **The depth goes first**: true depth, colour
masked, `GL_LEQUAL`, written. Colour follows with the bias and no depth write.
Colour-first would have broken crossings *inside* one batch, which is most of a
frame's tiles, since nothing would be written until the batch ended. Depth-first is
a per-batch prepass, so a rock behind another in the same batch still fails against
the nearer one's already-written depth. A semi-transparent batch tests with the bias
and writes nothing, one pass. The AO opaque-texel pass writes, so it runs unbiased.
The software rasterizer takes the constant only, and keeps the nearer depth when a
fragment passes on the tolerance.

The cost: 1,584 prepasses a second at 144 fps in area 1, about 11 batches a frame.
Uncapped, 840-854 fps against 859-864 with `KF2_ZBUFFER_BIAS=0 KF2_ZBUFFER_SLOPE=0`,
two runs each, about 1.5%.

**The trade is the tolerance itself.** Anything genuinely behind by less than it
draws over the nearer surface when it comes later in the table, so a crossing
resolves up to about one world unit short of the true line. If the seams persist,
raise the *Angled seam tolerance* slider (`KF2_ZBUFFER_SLOPE`) first. If crossings
look shifted, lower *Seam tolerance* (`KF2_ZBUFFER_BIAS`).
Whether these two panels are separate tiles' faces, as assumed, or share vertices was
not established. If they share vertices, neither change should have been needed, and
a seam still fighting at a generous tolerance means the cause is something else.

### Water was painted over by what lay under it

**Mechanism measured; the picture has not been looked at.**

Reported from play, and present since the start: fish and bones lying in the water
drawn on top of it, and now and then a triangle or two of water missing. Both are one
defect. **A blended polygon is depth-tested and writes no depth** (the bullet at the
head of this section), so wherever the water is, the depth buffer holds whatever lies
under it. Anything opaque that the ordering table puts *after* the water passes the
test there and draws over it, as it did on the console, which had no test at all.
The table does put such things after the water, for two reasons:

- **Models are pulled forward.** The tile walk links every map tile 240 entries back
  (`func_80031950` passes `0xF0` as the slot bias); the model submitter adds 240 only
  when the model sits at or above the camera, and otherwise uses the caller's own
  small bias (a creature's `rec+0x15`, 20 or -60 for the effects). A fish on the sea
  floor below the camera therefore sorts a few hundred SZ in front of the water tile
  above it. That is the fish drawn over the water.
- **Tiles sort by their average.** A piece of sea floor whose four corners average
  nearer than the water quad over it draws after it. That is the missing triangle:
  the floor, untinted, where the water should be.

The fix is the usual rule for a depth buffer: opaque first, then translucent.
`0079`'s `BlendOrder` does it in the ordering table's walk (`LibGpu.WalkOTag`, which
`Widescreen`'s replacement of `DrawOTag` now calls too, telling it each packet's
entry): a blended polygon with a depth record is held, the opaque polygons with a
record that follow it are sent, and the held ones go out in table order at the next
packet that is anything else. **Moving a translucent packet past an opaque one is
exact whichever is nearer**: the test decides what the opaque one covers, and the
translucent one then blends over what is behind it and is rejected by what is in
front. Everything the depth buffer does not order is a barrier: the HUD, the arm,
the fades, a non-polygon command, a packet without a record, the table's first slot
(the skybox's, which is never tested), and a blended model the port calls solid (the secret door, which writes depth). So nothing is ever moved
past something it was not already ordered against by the test, and two blended
surfaces keep the order the table gave them. It runs only while the Z-buffer tests
and the assemblers' records are the depth source (Fast geometry); with either off
the table is drawn as it always was. A held packet is sent under its own
`GteDepth.OtEntry` and `OtSlot`, so the HUD anchoring, the census and the frame
viewer see where it was linked.

`KF2_BLENDORDER_PROBE=1` measures the defect itself rather than the mechanism:
every recorded packet, in the order it is sent, is sampled on a 4-pixel grid
(perspective-correct depth, and a sample already behind opaque geometry is
skipped), and it counts opaque samples drawn behind a nearer translucent one, by
source. Swept over the New Game beach in `fdat02` with the shell's `view` (a 7x7
grid of camera positions four tiles apart around the spawn, four headings each, 196
poses), fix off: tiles painted over nearer water at 106 of the poses, 7,228 samples
in all and up to 887 at one (about 14,000 game pixels); models at 10, up to 205
(`view 80896 -14408 82944 200 1024 0`). Fix on: **0 and 0 at every pose.** A frame
there held 250-720 packets and sent them in one run, just before the HUD. Uncapped at
a sea view, 317-321 fps either way; 144.0 fps drawn at 20.0 ticks/s, `[present] wide
288, plain 0, vram fallback 0`, with the water enhancements on and with the HUD
anchored, no exceptions. `KF2_BLENDORDER=0` is the comparison.

**The trade** is at a waterline. A blended surface within the coplanar tolerance
of an opaque one in front of it still draws over it (the previous section), and now
does so whatever the table said, so the shore's edge in the water may move by that
tolerance. Whether that shows is for the eye.

**A corner at the camera made its polygon a barrier.** Up close, looking down at
something under the water, it could still show above it. The GTE saturates a
corner at or behind the camera plane to SZ 0, and a record with a zero corner was
dropped, so a nearby model's polygon was a barrier in the middle of the table:
it sent the held water there, and every underwater packet linked after it drew
over the water. Measured with a census of the barriers that sent held packets (a
local probe, not kept) at four poses over the `fdat02` beach: textured Gouraud
polygons (`0x3C`, `0x34`) with no record at slots 7494-7678, and the water sent in
1.9-2.5 runs. Such a corner is now recorded at depth 1, the nearest there is:
after, the only barriers were the table's last two slots and the water went in
1.0 runs at every pose, no exceptions. Judged by eye: fixed.

### The world lost its textures on NVIDIA

Issue #34 (Windows 10, RTX 3080 Ti): every world surface was drawn in its shaded
vertex colour with no texture, while the HUD and the compass were intact. It
reproduced on a Linux GTX 1050 Ti (driver 580.126.18) with every release up to
v0.3.1, on GL 4.5, 3.3 and 2.1. So it was never Windows, the CI build or the
first-run compile: v0.3.1's launcher built locally was broken and HEAD's was not.
**It was hidden by accident, not fixed.** The temporary driver-diagnostics commit
(`00f4861`) rewrote the prim vertex shader's position line as an if/else chain on
a uniform, and that alone was enough: HEAD with only that line put back was broken
again, and HEAD with only the fragment half put back was fine.

What was measured, on the broken build:

- `KF2_ZBUFFER=0`: textures back. `KF2_PERSPECTIVE=0`: still broken, because a
  depth-tested triangle is given a real clip W anyway (`persp = z || …` in
  `HleTri`).
- The texture coordinate drawn as colour: the whole scene blue (a real W reached
  the fragment), with no red or green gradient. So the texture coordinate was
  constant across each polygon, and every pixel read one texel.

With perspective interpolation, a texture coordinate goes constant only when one
corner's `1/W` swamps the rest, which is a corner at W 1 among real view depths in
the hundreds. The only source of a W of 1 was the select
`inW == 1.0 ? vec4(p, 0, 1) : vec4(p * inW, 0, inW)`. The CPU side never mixes the
two within a triangle (`HleTri` is all or none, and `GlCore` writes
`W = HasPersp && Z > 0 ? Z : 1`), so the select was being evaluated wrongly on
the GPU. The Z-buffer is what brings it out: `0051`'s colour-masked pre-pass is a
different compiled variant of the same program, and the driver says so
(`KF2_GLDEBUG=2`: `Vertex shader in program 3 is being recompiled based on GL
state`). Exactly which variant goes wrong, and why, was not established. It is
the driver's compiler, and the port cannot see into it.

**The fix is not to have a select at all:** `gl_Position = vec4(p * inW, 0.0,
inW)` in both prim vertex shaders. `p * 1.0` is `p` exactly in IEEE, so a
primitive with no recovered depth still lands on the same pixels to the last bit,
which was the only reason the select was there. The occlusion normal pass
(`NormalVs`) already wrote its position that way. Checked by eye on the GTX 1050
Ti: GL 4.5 and GL 2.1, every enhancement on, textures correct. The reporter's
RTX 3080 Ti on Windows has not been checked.

Two changes that close the neighbouring holes; neither one was the cause:

- **`invariant gl_Position`** in both prim vertex shaders. `0051` draws an opaque
  tested batch twice with this program and needs the two passes to agree, and GLSL
  promises that only for an invariant output, whatever variants the driver builds.
- **Every disabled `0048`/`0060` attribute reads a defined "no record".** `inLight`
  is a `uint`, and its generic value was never set, so it read GL's initial float
  `(0,0,0,1)`, which is undefined for an integer input. `inTex` was set once at
  init. Both are now given in their own type (`LightDefaults`, `TexDefaults` in
  `GlCore`), again whenever the arrays go off, since an attribute's current value
  is not guaranteed to survive a draw with its array enabled.

Measured on the GTX 1050 Ti, area 1 at `KF2_FPS=144`: 70.5-70.7 fps drawn before
and 71.6-71.8 after (GPU-bound), 19.9 ticks/s both. GL 4.5 and 2.1 both compile,
link and reach an area with `KF2_GLDEBUG=1` reporting no errors, and the picture
was checked by eye on it after both.

## PGXP: upstream's own recovery, and what taking it actually bought

**Mechanism confirmed and measured; the picture has not been looked at.**

Everything above this point recovers the same two numbers — a vertex's true
screen position and the view depth the GTE divided by — and carries them from
`Gte.Rtp` to the GP0 packet by watching `PSMemory`'s words go past
(`GteVertexMap`, "Following the value through memory"). RecompOne grew a second
answer to that after our pin: a full **PGXP**, in `39fb337a`, `91c20fcf`,
`95f0585b` and `6aae910a` (2026-08-31 to 09-07). It is backported here as
`patches/recompone/0034`-`0036`, and both mechanisms ship, chosen between by
`KF2_PGXP` — **and by nothing in the settings window**. See "PGXP has no control
in the window" below.

The difference is where the following happens. `GteVertexMap` sees only that a
word left one address and arrived at another, and pairs the two by value; a
coordinate the game *computes* rather than copies is invisible to it. PGXP is
told what every register holds, by hooks the recompiler emits beside every load,
store, move, shift, add, multiply and divide (`0035`), and keeps a `PgxpValue`
per word of RAM. Nothing is inferred.

**Three things about the backport are ours rather than upstream's.**

- **The tolerance is spent.** Upstream defines `pgxp.tolerance`, draws a slider
  for it, and never reads the value. It is exactly the guard this wants: a
  recovered position more than a couple of pixels from the one in the packet is
  not a more precise version of this vertex, it is a different vertex, and
  believing it moves geometry. See "Picking the tolerance" below.
- **`PgxpStats`** counts where each answer came from — the RAM shadow, the
  screen-position cache, or the per-primitive ambiguity pass — because the whole
  question about a second mechanism is how often it answers and with what.
- **A depth-tested triangle is given a real clip W** whether or not its texture is
  being corrected, and that one is a genuine fix. See below.

### The clip W was the Z-buffer's second cause

The section above left this open: with a correctly cleared buffer and correct
per-vertex depths, the sky still drew through nearby walls, and the untested
suspect was *"screen-linear interpolation of a view depth is wrong (it is 1/z
that is linear in screen space), which biases a polygon's interior"*.

That is exactly what was happening, and only on the GL path. `vDepth` is an
ordinary varying, so OpenGL interpolates it in `1/w` — which is exact when `w` is
the view depth and **screen-linear when `w` is 1**. `HleTri` set the clip W only
for triangles whose *texture* was being corrected:

```csharp
bool persp = tex && a.HasW && b.HasW && c.HasW;   // before
```

An untextured wall never asked for texture correction, so it arrived with `w = 1`
and every depth between its corners came out linear in screen space. The software
rasterizer never had the bug — it interpolates the reciprocals and takes one back
(`useZ` in `DrawPolygon`) — so the two renderers disagreed about the interior of
every flat-shaded surface in the game, which is most of the architecture.

```csharp
bool persp = z || (tex && a.HasW && b.HasW && c.HasW);   // after
```

The cost is that a textured triangle drawn with the depth buffer on and
perspective correction *off* is now corrected anyway. That pair is a comparison
rather than a picture anyone ships, and a depth buffer fed wrong depths is not a
comparison of anything. **This fix applies to both sources**, so it is not a
reason to prefer PGXP — it is the reason the Z-buffer was worth revisiting at all.

### The depth-clear threshold, and why it is off

A frame is not one scene. `GteDepth.DepthClearThreshold` (DuckStation's
`pgxp_depth_clear_threshold`) watches the mean view depth of consecutive
primitives and starts the buffer again when it falls by more than that, which is a
scene the game began afresh under the same projection. It reaches the existing
per-frame clear by bumping `Generation` rather than adding a clear path of its
own, and it flushes the GL batch first so the clear cannot take this frame's
earlier triangles with it. `KF2_ZBUFFER_THRESHOLD`; zero turns it off, **and zero
is the default**.

DuckStation ships 300 and this port shipped 300 with it, and that was wrong. The
value is pickable the same way any threshold in this project is: look for two
populations and put the cut in the gap. `KF2_ZBUFFER_PROBE=1` censuses **every**
forward step between consecutive primitives, fired on or not, so the histogram is
of the game rather than of the setting. Measured over a walk through area 2:

```
forward steps 15696/s: 39.4% under 10, 47.7% under 50, 9.2% under 150,
                       2.8% under 300, 0.9% under 1000, 0.0% beyond; widest 318
```

**One population, decaying smoothly, with no gap anywhere in it** — which is what
ordinary depth sorting inside a single scene looks like, and there is nothing
else. King's Field draws one world and a 2D HUD, and 2D never recovers a depth so
it never enters the mean; there is no second scene to detect. The widest step in
the whole run is 318, barely past DuckStation's 300, so any threshold that fires
at all is firing on the game's own geometry.

Confirmed from the other side by turning it on:

```
2575.2 depth clear(s)/s at threshold 300
```

Some twenty clears a frame — the depth buffer thrown away and rebuilt over and
over inside one picture, which is worse than not having one — and the frame rate
went from 144 to 34-76 fps with it, because each clear flushes the GL batch. The
mechanism is kept because it is the right one for a game that needs it. This game
does not.

**Zero was the default and still was not what ran.** The threshold had a slider
until `dfe73e2` took it out, and `ZBuffer.cs` went on reading
`kf2.zbuffer.threshold` from the saved config, so a value set back then (783 here)
stayed live with no control to show it. It was reported from play as **ambient
occlusion intermittently failing with the Z-buffer on**, and that is how it looks:
AO reads the same attachment, and a clear takes the frame's depth with it. Standing
still after `warp 0`, `2`, `3` and `4` with `KF2_AO_PROBE=2 KF2_ZBUFFER_PROBE=1`, a
forced 783 fired 143-19583 clears a second and the AO census read `0.0% of it
carrying a surface` in almost every window. With the threshold off, the same spots read
11.3%, 73.8%, 0.2% and 10.7%. It fires anywhere near geometry is drawn after far,
which is why it came and went. Even one clear a frame (143/s at 144 fps) was
enough to empty the census. The saved key is no longer read, and
`KF2_ZBUFFER_THRESHOLD` is the only way to turn the clear on.

### Picking the tolerance

Same method, and the same answer shape. `KF2_PGXP_PROBE=1` buckets the
disagreement between every recovered position and the coordinate in the packet,
taken **before** the tolerance test so both populations would show if both
existed. Two windows, area 2, `KF2_PGXP_TOLERANCE=-1` so nothing was filtered:

```
disagreement with the packet, 409642/s: 29.2% under 0.5px, 68.2% under 1,
                              2.6% under 2, 0.0% under 4, 0.0% under 8,
                              0.0% beyond; widest 1.27px
disagreement with the packet,  57888/s: 24.1% under 0.5px, 68.2% under 1,
                              7.7% under 2, 0.0% under 4, 0.0% under 8,
                              0.0% beyond; widest 1.87px
```

**Nothing at all past 2 px, in either window, and the widest ever seen is 1.87.**
There is one population again: PGXP has never once answered with a different
vertex in anything measured here, so the guard has nothing to catch and any value
of 2 or above is inert.

The interesting part is the 2.6-7.7% *between* 1 and 2 pixels, because a genuinely
more precise version of the same vertex can only differ by the fraction the GTE
truncated, which is under one pixel by construction. That excess is not a wrong
vertex, it is **the divider**: `PushPrecise` recomputes the projection in doubles
as `OFX/65536 + IR1·(H/w)`, while the GTE used its own reciprocal lookup
(`Divide(H, SZ3)`, the `Unr` table). The two disagree by up to about two pixels on
a far vertex, and that is a real difference between what PGXP believes and what
the packet says, not noise.

So: **2 is the right number and it is right by luck rather than by argument** —
below 2 it starts refusing correct answers (7.7% of them at 1 px), above 2 it
refuses nothing anyone has ever seen. It stays at 2 as a tripwire: if a scene ever
does produce a wrong-vertex population, the refusal counter is what will say so.
`-1` turns it off and costs nothing measurable today.

### Measured: the coverage was already there, and PGXP costs a fifth of the frame

Both sources, autostart into slot 2, `warp 2` over the shell once the loader had
finished, then walked around for 45 s at `KF2_FPS=144`. Eight probe windows each:

| | address map (`0012`) | PGXP, full | PGXP, `KF2_PGXP_CPU=0` |
|---|---|---|---|
| vertices answered | 92.2 - 97.1% | 93.4 - 96.7% | 79.2 - 85.7% |
| of those, from the RAM shadow | — | 93 - 97% | **0** |
| triangles depth-tested | 90.2 - 96.2% | 91.4 - 95.6% | 77.4 - 79.3% |
| frames a second | **144.0** | **106.7 - 114.9** | **143.8 - 144.1** |

That third column is the cost isolated, and it also shows that **upstream's two
tracking options are not independent**: `PgxpMemory.Store` is only ever reached
from `PgxpCpu`, so turning CPU tracking off empties the RAM shadow whatever the
memory tick says, and PGXP degrades to the screen-position cache — the same class
of guess the address map was written to replace, and it measures like one.

**The honest reading is that PGXP did not buy coverage in this game.** The address
map was already answering for 95% of vertices, because King's Field assembles its
packets with whole-word `lw`/`sw` out of a transform cache — the one shape a
value-matching ring follows perfectly. What PGXP costs is real and measured: a
fifth of the frame rate under load, and the third column above places all of it in
the emitted hooks rather than in the lookup. With PGXP *off* the recompiled binary
still reads 144.0 fps at 20.0 ticks/s, so `0035`'s emitted
`if (Pgxp.CpuTracking)` branch is free when nothing is using it.

What PGXP does have that the address map cannot:

- **Backface culling on precise positions** (`Nclip`). A sliver polygon whose true
  area is a fraction of a pixel can come out the wrong sign from three truncated
  screen positions and drop out of the frame entirely. Nothing in this port
  measures how often that happens, and nobody has looked.
- **Positions rather than fractions.** PGXP produces the projection in floats;
  `GteVertexMap` produces the fraction the GTE truncated, which is the same number
  only where the packet coordinate is the one the GTE wrote.
- **Correctness by construction.** The address map's 95% is a rate that happens to
  be high for this game's packet assembly. PGXP's is what a tracked value does.

So it stays off, and it stays. The thing to keep in mind before reaching for it
is that **the number that fixed the Z-buffer here was the clip W, not the source
of the depth.**

### PGXP has no control in the window

It had one — a *Vertex source* combo and six checkboxes under Video ▸ Geometry
precision, plus upstream's own PGXP block, which the vendoring merge brought back
into `DisplaySettingsSection` alongside it. Both are gone, and so is the depth
buffer's checkbox that shared that heading.

The test is the one the map's style, the widescreen ticks and the two shading
checkboxes were each measured against: **is this a choice the player owns?** PGXP
is not. It buys no coverage in this game (92.2-97.1% against 93.4-96.7%), it costs
a fifth of the frame rate, and its picture has never been judged by eye — which
makes it a comparison between two mechanisms, and every comparison in this port
lives on the console. Nine controls asking a player to arbitrate between two
implementations of a number the console discarded is the pane describing the
implementation rather than the game.

Two consequences worth stating:

- **The saved key is no longer read.** `Pgxp.Reload` forces `pgxp.enable` false
  unless `KF2_PGXP` says otherwise, so a config that ticked it while the combo
  was drawn is not left running a fifth slower with nothing in the window to
  explain it. That is the `kf2.framepacing.logichz` rule applied again.
- **Upstream's frame-rate slider went with it.** It is the *interpolated* rate —
  it writes `Interp`'s key and disables itself unless PGXP is on — and this port
  never enters `PresentLoop`, so `Interp.Backend` stays null and the slider
  changes nothing it claims to. The port's own rate is `FramePacingPage`, under
  Video, and two frame-rate sliders in one pane is one of them lying. The comment
  left in `DisplaySettingsSection.Draw` says so, for the next merge.

## Ambient occlusion: painter's order is the G-buffer

**Mechanism confirmed and measured; the picture has not been looked at. Off by
default.**

Contact shading — the corners going dark, the join between a wall and the floor,
the underside of a doorframe — is the one thing a dungeon this dark could use and
the one thing a PlayStation could not compute. It is a screen-space pass over a
depth buffer, and the whole of this file is about the fact that there is no depth
buffer and never was.

The depth is the same recovered SZ perspective correction runs on. What is new is
how it is turned into a *G-buffer*, and that is the part worth keeping.

### There is no prepass to be had, and painter's order supplies one anyway

A screen-space pass needs the depth of the **nearest visible surface at every
pixel**, complete, before any shading happens. The usual way to get one is a depth
prepass: submit the frame's geometry once with colour writes off, then again for
real. Nothing here can do that. The geometry arrives incrementally through GP0 as
the game walks its ordering table, the port does not hold it, and — this is the
part that has no workaround — **nothing knows the frame is finished until it is**.
There is no point at which the geometry could be replayed that is not after the
frame has already been drawn.

What supplies the G-buffer instead is the console's own occlusion algorithm.
`DrawOTag` walks the ordering table **back to front**, so the last thing drawn at
a pixel is the nearest thing at that pixel. Give every 3D triangle a depth write
and leave the test at `GL_ALWAYS`, and the attachment ends the frame holding
exactly the visible-surface depth — arrived at with no test, no prepass and no
second pass over anything, because painter's algorithm has already done the
sorting that a depth prepass exists to do.

**That is also what makes this safe where the Z-buffer is not.** The Z-buffer
*rejects* fragments on the recovered depth, so a wrong depth loses geometry, and
that picture has never come out right (see "Z-buffer" above). Occlusion shading
reads the same numbers and lets them decide nothing: the ordering table remains
in sole charge of what is visible, and a wrong depth costs a wrong shade of grey.
The two share the whole write path and differ in one statement —

```csharp
_gl.DepthFunc(GteDepth.ZBuffer ? DepthFunction.Lequal : DepthFunction.Always);
```

— which is why `GteDepth.DepthWanted` (`ZBuffer || AmbientOcclusion`) replaced
`GteDepth.ZBuffer` at every site that decides whether a depth is recovered at all.

**There are more of those sites than there look to be, and missing them reads as
the feature simply not existing.** The first run reported `0 tris/s carrying a
depth, 0.0% of 3D` while every other number was healthy — the projection was being
read forty thousand times a second, the passes were running at the frame rate —
because `GpuRaster` still had `bool wantZ = GteDepth.ZBuffer`. Fixing that alone
would have been worse than not fixing it: the gate above it reads

```csharp
if ((GteDepth.Active || pgxp) && (tex || GteDepth.Subpixel || GteDepth.DepthWanted))
```

and without the last term the only geometry reaching the recovery block at all is
the *textured* geometry, which in this game is a minority of the architecture —
most of a King's Field wall is flat-shaded. The result would have been a depth
buffer with the walls missing and an occlusion pass shading around holes, which is
a picture nobody here can see.

### The far plane is the mask, and it is not an optimisation

A screen-space pass over the finished frame would shade the HUD, because the HUD
is drawn over geometry and the depth under it is that geometry's. Every version of
this that keeps a separate "is this 3D" mask — a stencil, a second attachment —
costs an attachment and a bind. It is free instead: **anything with no recovered
depth writes `1.0`**.

```glsl
gl_FragDepth = vDepth > 0.0 ? vDepth : 1.0;   // was gl_FragCoord.z
```

so a fourth batch mode joins the three the Z-buffer had — 0 painter's, 1 opaque 3D
test-and-write, 2 semi-transparent 3D test-only, **3 opaque with no depth, write
the far plane**. The pass then treats a far texel as the absence of a surface in
both directions: it is neither shaded nor allowed to occlude anything, so a
silhouette against the HUD draws no dark halo. Three things fall out of it at once:

- **The HUD, the menus and the item pictures are untouched**, by the same argument
  that keeps them out of perspective correction — the CPU computed those
  coordinates and they were never in the vertex map.
- **A hole in the depth buffer costs occlusion rather than inventing it.** The
  vertex map answers for 61-63% of the *triangles* in the areas measured (all three
  corners, which is the cube of a per-vertex rate), and the 37-39% it misses go to
  mode 3 and are left flat. That is the same "a miss is the old behaviour" guarantee
  the rest of this file rests on.
- **Semi-transparent primitives write nothing at all**, which is mode 0 and is the
  case that would otherwise be a visible bug: the death fade, the damage flash and
  the wash on an area load are full-screen quads you see the world *through*, and
  stamping the far plane under them would switch the shading off for exactly the
  frames they cover.

**Two surfaces broke the mask, both fixed by measurement.** The skybox is linked
in ordering-table slot 0 and projects near, so it carried a depth and was shaded
like a wall: `GteDepth.OtSlot` counts the empty tags a walk has passed, and a
triangle in slot 0 recovers no depth. Measured, slot 0 holds the sky and nothing
else (areas 0 and 2), and no world geometry sits below slot 1840 in any area; at
one spot in area 0 the shaded share went 17.1% to 2.1% with area 1 unchanged.
And a semi-transparent textured primitive only blends the texels with the STP bit
set, so a surface drawn semi-transparent can be wholly opaque and still wrote no
depth, leaving the room behind it for the pass to shade (a secret door showed
through). Such batches now take a second, colour-masked draw that discards the
blended texels and writes the rest. Neither picture has been looked at.

**A secret door is solid all the way through, and only the model submitter can
say so.** Area 2's door (save 3's start) carries the STP bit on every texel, so
that draw discarded all of it and the pass shaded the door with the room behind.
The first fix (`c16d477`) kept every texel of every blended world batch, and that
was wrong for everything else that blends: the torch flames wrote a square of
depth and were shaded round (`526a8df` patched that by discarding black texels),
and the water wrote a depth the Z-buffer then rejected the shore against, cutting
it out below the waterline (`36121ac` added a stencil for that). All three are
reverted. **Blending does not say whether a surface is solid; the table it came
from does.** Measured with the frame viewer and `KF2_ZBUFFER_PROBE=1`: the door's
blended packets are object-table models (about 35 a frame), the flames are
sprite-table models (10 a frame, op `0x26`), and the water is map tiles, which
never pass through `func_80032588`. So `ZBuffer` hooks `func_80032588`, the
assemblers set `GtePacketDepth.Rec.Solid` on a blended packet built there for an
object-table model (`ModelWalk.SubmitKind`), and `GlCore` gives such a triangle
zMode 4: tested like any blended triangle, kept for the normal buffer, and drawn
depth-only with every texel (`uOpaqueDepth` 2). Everything else keeps the
non-STP rule above.

Measured at save 3's start, `KF2_AO_PROBE=2`: 25.2% of the picture shaded and the
occlusion map within one step in two cells of the `c16d477` build; 0.2% with
`KF2_AO_SOLID=0`, which is the comparison. Area 1's water flags nothing, and area 1 at
`37890, -12800, 54670` flags only sprite packets, which stay unsolid. 144.0 fps
drawn at 20.0 ticks/s there and at the door. The flag needs the C# assemblers and
`ModelWalk`'s walk (both on by default); without them the door shows through
again. **Confirmed by eye: the door hides the room behind it, the flames have no
square halo, and the shore is whole at the waterline.**

**The object table was too wide a key, and area 0's water showed it.** Reported
from play: in the flooded cave (area 0, `158821, -11520, 149415`) the walls could
be seen through where they met the water. The near water there is not map tiles
but two object-table models (kind `5F`, model 204; kind `A0`, model 185), so every
blended packet of it was solid and wrote its surface's depth; an opaque wall
triangle drawn after it in the table (the frame viewer showed one at slot 7474
after water at 7223-7378) failed the test wherever it sat below the waterline,
leaving the water blended over whatever had been drawn before. `KF2_AO_SOLID=0`
cured it by eye. A sweep of every object in areas 0-7 found about thirty models
the old rule called solid, additive glows (`abr 1`, `abr 3`) among them; the door
is model 230 in areas 2 and 4, and its definition's first byte (`0x80175914 +
model * 24`) is `0E`. That byte equals the record's type byte at `+4` for every
object sampled except the door, whose type reads `FF`, so it is the object's
kind. Keying on `0E` alone made the ordinary doors see-through (reported by eye
at `65634, -19968, 7173` in area 2): they are kind `02`, models 130 and 205, and
they blend too. Solid is now kinds `02` and `0E` (`ModelWalk.SolidKind`).
Measured: at the water, 0 of about 2,000 blended object packets a second solid;
facing the area 2 door, 960 of 960; 144 fps drawn at 20.0 ticks/s. The probe line
reads `object N (M solid)`. **Confirmed by eye: the cave's waterline is whole and the area 2
door hides what is behind it.** The list is kept by hand; the principled key is
whether an object blocks the player, which the movement code already decides
(see "Solid should mean blocks the player" in `docs/TODO.md`).

### Undoing the game's own projection, and the number that caught the error

A depth texel is a view depth and nothing else; the pass needs a view *position*.
The GTE's divide is `screen = centre + IR * H / z`, so the inverse is
`view.xy = (screen - centre) * z / H` — with `H` and the `OFX`/`OFY` centre read
out of `Gte.Rtp` as it projects rather than assumed. `GteDepth.ProjH` is 200 in
this game, not the 320 a reasonable guess would have used, and a wrong H tilts
every reconstructed normal.

Placing that centre on the presented picture is where this went wrong, and the
failure is worth recording because it was **silent, intermittent and invisible to
every other counter**. The first version added the GP0 drawing offset of the last
depth-writing triangle, on the reasoning that the GTE's centre is in packet
coordinates while the render target holds VRAM ones. The offset it recorded
belongs to the buffer being *drawn*; the pass runs against the buffer being
*presented*; with two display buffers those differ by a screen on alternate
frames. So half the frames reconstructed correctly and half put the projection
centre a whole screen above or below the picture.

The right answer is that a display target's drawing offset **is** its own origin —
if it were not, the prim shader's `uPosBias` of `-rt.X`/`-rt.Y` would already be
putting every polygon in the wrong place — so the two cancel and nothing
per-triangle needs recording at all.

**The probe prints the reconstructed centre for this reason**: it should read
`0.500,0.500` on every frame, and it read `0.500,1.500` and `0.500,-0.500` on
alternating frames while the bug was in. Nothing else moved. The pass still ran at
the full rate, the depth still arrived, the coverage was unchanged, and the census
still reported occlusion — just less of it, in the wrong places.

### The pass

Two full-screen draws between the finished target and the present blit, in
`patches/recompone/0040`:

- **Occlusion.** Reconstruct the view position, take the normal from the *nearer*
  neighbour on each axis (so a pixel on a silhouette takes the surface it belongs
  to rather than straddling the edge), then a golden-angle spiral of samples inside
  the world radius as it projects at this depth. A sample occludes by
  `max(0, dot(normalize(S - P), N) - bias)`, falling off past the radius rather
  than stopping at it. The per-pixel rotation is a **4x4 interleaved pattern rather
  than a hash**, because the blur below is a 4x4 box and a pattern with that period
  cancels exactly where noise leaves noise.
- **Blur.** That 4x4 box, refusing any tap whose depth differs from the centre's by
  more than a twentieth — relative rather than absolute, since a step that is a
  crease at arm's length is a rounding error across a room.

It runs at **present**, and writes only its own `R8` texture; the present shader
multiplies. Nothing the game can read back carries the shading — not VRAM, not
either display buffer, and not the frame a modal loop stores and restores — so a
menu cannot bake it in and then re-shade it on every iteration. `uAoOn` skips the
multiply rather than multiplying by one, so a run with the pass off is
bit-identical to a build without it.

GL backend only. The software rasterizer keeps its own float-per-pixel depth and
has no full-screen pass to run this in.

### The normal was the guess, and the port stopped having to guess (`0058`)

**Mechanism measured; on by default.** `KF2_AO_NORMALS=0` is the comparison.

Everything above is about the *depth*, and the depth was never the weak part. The
**normal** was. A screen-space pass has to have one at every pixel, and the only
thing the pass had to make one out of was the depth buffer itself — four
neighbouring texels, two differences, a cross product:

```glsl
vec3 n = cross(nearer(p, r, l), nearer(p, b, u));   // AoFs, before 0058
```

That is wrong in two ways at once and both of them show as *dirt* rather than as
contact shading. It straddles every silhouette, which is what the `nearer()` pick
exists to paper over — a pixel on the edge of a doorframe has two surfaces in its
four neighbours and no way to tell which one it belongs to. And on a flat wall it
is differencing a quantised depth against itself, so a surface with one true
normal gets a slightly different one at every pixel. King's Field is made of large
flat polygons, which is the case that suffers most.

**The geometry is a thing the port holds now.** `PolyAssembler` builds the
polygons, `TileWalk` enumerates the map's cells and `ModelWalk` enumerates the
creatures, objects, effects and billboards — so "the geometry arrives incrementally
through GP0 and nothing holds it", which is the sentence the whole of this section
rests on, has stopped being true of the world. `AoGeometry` keeps each
depth-carrying triangle as it is submitted, and after the frame is finished
`GlCore.RenderNormals` (`RenderSurfaces` since `0067`) draws the list again into a normal buffer on the target.
A normal is then the polygon's own plane: exact, constant across the face, and
incapable of straddling anything.

**Three things decided the shape of it.**

- **It cannot be an MRT off the colour pass.** The obvious implementation is a
  second output on the prim shader. `PrimFs` already has one — `layout(location =
  0, index = 1) out vec4 BlendColor`, the dual-source factor the console's blend
  modes need — and a program with a dual-source output may not render to more than
  one draw buffer at all. So the normal has to be its own draw, which is only
  affordable because the port has the triangle list to draw.
- **Order is the correctness argument, not a depth test.** The list is kept in
  submission order and redrawn with **no depth test and no depth write**, so the
  last normal written at a pixel belongs to the last triangle drawn there — which
  is the same rule that put the depth there. A test would be a second opinion, and
  a second opinion is a way to disagree.
- **The list is per render target, not per frame.** With two display buffers the
  target being presented was drawn a frame ago, which is exactly why the depth
  attachment lives on the target; the geometry lives there for the same reason and
  is dropped on the same condition the depth is cleared on. Hanging it off the
  frame would have shaded each frame with the *other* buffer's geometry, which is
  the same class of error as the projection centre being a screen out (above) and
  would have read as plausible shading in the wrong places.

The pass keeps its old reconstruction as the fallback, per texel: the normal
buffer's **alpha** says whether this pixel was reached, and a pixel it missed —
a polygon the port did not assemble, a semi-transparent surface's opaque texels —
gets exactly what it got before. So this is additive in the way everything else
here is.

**What it measures.** `KF2_AO_PROBE=1` gains a line for the buffer itself, and the
census gains a third channel, because every number the pass already printed stays
identical if the normal buffer is empty — the fallback produces a completely
healthy-looking frame. Measured in area 1 at 144 fps, slot 2:

```
[KF2] ao: normals geometry, 18309 tris/s kept, 142.4 normal passes/s
[KF2] ao: darkest 0.69, mean 0.983, 17.8% of the picture shaded,
          98.7% of it carrying a surface, 100.0% of that lit from a geometry normal
```

against `KF2_AO_NORMALS=0` on the same view:

```
[KF2] ao: normals depth, 0 tris/s kept, 0.0 normal passes/s
[KF2] ao: darkest 0.67, mean 0.983, 18.0% of the picture shaded,
          98.7% of it carrying a surface, 0.0% of that lit from a geometry normal
```

- **Every shaded surface got a real normal.** 100.0% of the covered picture, which
  is what it should be — the geometry that writes depth *is* the geometry the port
  assembles, and the two coverages are the same set.
- **It costs nothing measurable.** 144.0 fps drawn at 20.0 ticks/s either way, one
  extra draw of ~18k triangles a second. The frame's geometry is already resident;
  this redraws it with a three-line fragment shader and no texturing.
- **The picture moved, and only slightly, which is the expected amount.** The same
  surfaces are shaded to within two tenths of a percent, and the darkest pixel goes
  0.67 to 0.69. That is the signature of *the same occlusion with better normals*
  rather than of a different effect: a wrong normal on a flat wall mostly
  self-cancels over a 16-sample spiral, and where it does not is the silhouettes,
  which are a small share of the pixels and the whole of the visible artefact.
**And then the census was asked how much the normals actually moved, which is the
question that should have been asked first.** The pass computes the old
depth-differenced normal as well on the frame it reads back (only on that frame)
and reports the angle between the two:

```
[KF2] ao: old normal off by 0.4 deg mean, 0.3% over 30 deg, worst 90 deg
```

Measured over 55 census frames across six areas and four headings each, the mean
disagreement is **0.0-0.8 degrees** and the share of surfaces more than 30 degrees
out is **under 1%**. One view read 3.6 degrees and is not a counter-example: it had
`0.2% of it carrying a surface`, so the mean was taken over a handful of pixels in
one corner, which is what a tiny denominator does.

**So this change is correct and very nearly invisible, and saying so is the point
of having the counter.** The disagreement is confined to silhouette pixels — a
doorframe's edge, a pillar against a far corridor, a creature's outline — because
away from an edge the four depth taps sit on the same flat plane and the old answer
was already that plane's. King's Field is made of large flat surfaces, so those
edges are well under one percent of the picture. The honest claim for `0058` is that
it removes a whole class of error for nothing, and makes a tighter bias possible,
not that it is a visible upgrade on its own. The visible one is `0059` below.

- **Never looked at by eye:** whether that sub-1% of pixels — the edges — reads
  cleaner. The numbers now say how little of the picture is in question.

### Occluders the camera cannot see (`0059`)

**Mechanism measured; off by default.** `KF2_AO_WORLD=1` turns it on.

A screen-space pass can only be occluded by what is on screen. Stand in a corner
facing one wall and the wall *behind* the camera contributes nothing, so the corner
is only as dark as the part of it in shot — and the shading changes as the view
turns, which reads as the effect being unstable rather than as the room being dark.
No radius and no sample count fixes it; it is what a depth buffer *is*.

The port is not limited to the picture. `TileWalk` enumerates the map's cells and
the game keeps the whole area as an 80×80 tile grid at `0x801C8484`, so the geometry
that is not on screen is available. The pass marches that grid in world space, where
being off screen means nothing.

**The transform is exact, and that is the part that could have been a guess.**
`func_80031950` loads a view matrix from `0x80192E18`, and `TileWalk.Place` shows
what the game feeds it: `(tx << 11) - CamWorldX`, a position *already relative to
the camera* in world axes. So that matrix is the camera's rotation alone — no
translation, no per-object rotation composed in — and inverting it turns a view
position straight back into a world one. Nothing has to be assumed about how the
game composes pitch, yaw and roll. It is uploaded untransposed on purpose: GLSL
reads a `mat3` column-major, so the row-major world-to-view matrix arrives already
inverted.

Checked before it was used, because a wrong transform here shades the wrong room
and still looks like shading: the third row of a world-to-view rotation is the
camera's own forward axis, and it measured `0.786, 0.040, -0.615`, length `0.999`,
putting a point 2048 units ahead in tile 36,35 from the camera's own 35,36.

**What a wall is in that grid took three tries, and each wrong answer measured as
exactly nothing.** The census is what caught all three — `17.8% of the picture
shaded` never moved by a digit.

1. **A step in the floor heights.** The obvious reading, and wrong: the floor either
   side of a wall is the *same height*, so the march found a horizon nowhere.
2. **`+4 & 0x80`, the bit `CullGrid` floods visibility against.** Plausible — a tile
   that stops sight should stop light — and it exists, 279 of them in area 1. But
   **0 of them within three tiles of the camera**, so it marks something else.
3. **A tile with no floor at all.** Right. The grid's tiles are drawn when their
   model byte is under 240, and the row north of the player read `-1/-1`: nowhere to
   stand, which is what the edge of a room *is* here. With undrawn tiles treated as
   solid rock standing their own height, the term finally had something to occlude
   with.

A fourth error was in the encoding rather than in the reading, and is worth keeping
because it is a general one: **a height byte of 0 is a real floor**, so using 0 as
"no tile" emptied the grid under the player's feet. Being drawn is its own bit now.

The reading that confirmed the whole chain: the player's raw Y is `-14848` and the
tile they stand on has an upper-half floor at `14848` — the game's own
`(0 - height) << 7`, matching exactly — with the camera 1600 above it.

**Measured**, area 1 at 144 fps, one fixed view:

```
                     shaded   darkest
screen space only     17.8%      0.69
with the floor plan   34.4%      0.64
```

and the picture's left third, which the screen-space pass correctly found nothing
for — it is one flat wall, and a flat plane occludes itself nowhere — is now shaded
by the room around it, which is the whole claim. 144.0 fps drawn at 20.0 ticks/s
with it on, `[present] wide 288`.

**Three ways it can step the whole picture at once, and a step is seen as the
screen flashing.** Every input to the term is shared by every pixel, so anything
that moves between one frame and the next moves all of them together — which is why
each of these is guarded rather than left to be noticed:

- **A transform read that is not the camera's.** The matrix belongs to the tile
  walk, so a frame that draws no map tiles can read it stale or zeroed, and the
  composed camera position can splice a stale `u16` onto the player's high bits as
  a position a world away. A read whose rows are not unit vectors, or whose camera
  is more than two tiles from the player, is refused and **the last good transform
  stands**; switching the term off for a frame and on again for the next is the
  thing being avoided. `stale reads/s` counts them.
- **A floor plan caught mid-rewrite.** The grid is re-read on a clock while the
  game is rewriting tile records — a door, a lift, a whole area loading — and a
  tile that is missing reads as *solid rock*, so a half-written grid darkens
  everything until the next read puts it back: one step down and one step up a few
  tenths of a second apart. Nothing can tell a half-written grid from a real change
  by looking at it, but a half-written one does not survive a second read, so **a
  change is held until two reads agree**. A door costs two read periods to appear,
  well inside how long a door takes to open. `changed` and `held` are the rates.
- **Every pixel marching in lockstep.** This one was the port's own doing: the
  march used the *same* eight directions at the *same* three distances for every
  pixel, so the moment the camera crossed a tile boundary every pixel changed its
  answer together and the whole picture stepped — about once per tile walked. The
  directions and the ring radii now carry the pixel's own 4x4 interleaved offset,
  the one the screen-space spiral already uses, so the crossings are spread across
  the 4x4 cell and the blur that follows averages them into a gradient. Measured
  identical where it should be: same view, 34.4% shaded, darkest 0.64, 144.0 fps.

**Now looked at by eye, and the verdict is the reason it ships off.** It does what
it claims — a cave read as lit by its own room rather than by what was in shot —
but at the default strength of 0.6 some rooms come out much too dark: a cave with
a low ceiling is surrounded on every side by blocking tiles, so nearly every march
direction finds a horizon and the whole floor sinks towards the term's floor
instead of only its corners. The screen-space pass cannot do that, because a
direction that leaves the screen contributes nothing to it. Not diagnosed further:
the likely candidates are the flat `AoWorldStrength` (no falloff with how much of
the hemisphere is blocked), the ring radii being tile-sized so a small room is
entirely inside the first ring, and undrawn tiles standing as rock of unbounded
height rather than to the ceiling. Until that is settled it stays off, and the
default strength is not a number anyone should read as chosen.

### What is measured, and what is not

`KF2_AO_PROBE=1` reports the coverage, the projection recovered from the GTE, the
reconstructed centre, and the passes actually run. **`KF2_AO_PROBE=2` is the one
that matters**, and it exists because of the rule at the top of this file: nobody
here looks at the picture, and *every number the first probe prints stays exactly
the same if the shader returns 1.0 on every pixel*. The depth still arrives, the
projection is still read, both draws are still issued, the present still
multiplies. So the occlusion texture is read back once a window and reduced to a
darkest value, a mean, a shaded share and a 32x16 map.

Beside it is a **surface** map, and that is the pair that makes a blank patch
arguable rather than a matter of opinion. The pass carries a second channel saying
whether it found a surface at all — nothing draws with it — because a cell at 1.00
occlusion means two quite different things: no surface there (the far-plane mask
doing exactly its job over the HUD), or a surface the pass looked at and found
nothing near enough to shade, which is a radius too small or a reconstruction that
is wrong. Without the second map every blank column is an argument, and the first
version of this had one: a third of the picture read unshaded and it took a
turn-on-the-spot to establish that the geometry there was a flat wall.

Measured in area 1 at `KF2_FPS=144`:

```
[KF2] ao: 23616 tris/s carrying a depth, 61.2% of 3D, H 200 centre 160.0,120.0
          -> 0.500,0.500 of the picture (42336 reads/s), 144.0 passes/s,
          0.0 no target/s, over 144 frames/s
[KF2] ao: darkest 0.67, mean 0.983, 18.0% of the picture shaded,
          98.7% of it carrying a surface
[KF2] ao:  occlusion (9 = faint, 0 = black)      surface (# full, + partial, . none)
[KF2] ao:  8899999..99899..................   ################################
[KF2] ao:  8877888999899...................   ################################
[KF2] ao:  999988887889....................   ################################
[KF2] ao:  ....9999879.....................   ################################
[KF2] ao:  ........989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .......9989.....................   ################################
[KF2] ao:  .....9.9999.....................   #####+++++######################
[KF2] ao:  .....999999.....................   #####++++++#############+++#####
[KF2] ao:  ..999988889.....................   #####++++++#############+++#####
[KF2] ao:  .99988899999....................   ################################
```

- **It costs nothing measurable.** 144.0 fps drawn at 20.0 ticks/s with the pass
  on, against 144.0-144.1 at 20.0 with it off. The 141.8 that turns up alongside
  `KF2_AO_PROBE=2` is the census's own readback stalling the pipeline once every
  two seconds, not the two draws.
- **The mask lands exactly on the HUD.** The only cells the surface map marks as
  anything but full are the HP/MP gauges (rows 12-15, columns 5-10) and the
  equipment icons (columns 24-26), which is 1.3% of the picture and is the whole
  of what the far-plane write excluded.
- **The reconstruction is right.** `0.500,0.500` on every frame at 4:3 and at
  16:9, where the present still reports `wide` and the frame rate is still 144.0.
- **It coexists with the Z-buffer.** With `KF2_ZBUFFER=1` as well the occlusion
  census is identical to the digit — they read one buffer and only one of them
  rejects anything with it.
- **The radius is the whole of the look.** The share of the picture the pass
  darkens goes **9.2% / 18.0% / 33.9% / 39.6%** at a radius of 192 / 512 / 1024 /
  2048 world units, over one fixed view.
- **The shading follows the camera, which is what rules out a screen-space
  artefact.** Turning on the spot through four headings at radius 1024 reads
  33.9% / 9.3% / 59.5% / 52.3%, and the map resolves doorframes as rectangles
  that move with the view. The first view has a large flat wall filling its right
  third and the pass correctly finds nothing there — a flat plane occludes itself
  nowhere, whatever the radius.
- **At the shipped 20 fps default**: 20.0 fps, 20.0 ticks/s, same census.

**What has not been looked at is the picture** — whether the shading lands where a
person would expect it, whether the radius is the right one, and whether it reads
as contact shading rather than as dirt. It ships on anyway: that is now a
first-run picture rather than a comparison, and the radius, the strength, the bias
and the sample count stay console settings rather than sliders. It is also
deliberately not authentic — the console could not
have drawn this — which is the same footing true color is on.

### What the pass costs, and the quality setting

**Mechanism measured; Medium and Low have not been looked at.**

Reported from an integrated GPU: about 14 ms a frame for the pass. The pass ran at
the present framebuffer's size, the render scale times the display area, so at
scale 4-6 it shaded 16-36 pixels per game pixel, and its 4x4 blur covered a single
game pixel. Each pixel reads 16 depth texels and the blur 16 more plus 16
occlusion texels, so the cost is memory traffic and follows the pixel count; an
integrated GPU shares system memory and pays for it most.

The *SSAO* slider under Video ▸ Enhancements (Off, Low, Medium,
High) replaced the checkbox. Off writes the old `kf2.ao.on` key, so a saved
config reads the same. A quality (`kf2.ao.quality`,
`KF2_AO_QUALITY=low|medium|high`) sets `GteDepth.AoResolution`, a cap on the
pass's scale in multiples of the game's pixels, and the sample count:

| quality | resolution | samples |
|---|---|---|
| High | the render scale, as before | 16 |
| Medium (default) | at most 2x | 16 |
| Low | at most 1x | 8 |

The pass, the blur and the normal buffer all run at the capped size. The blur's
step is one occlusion texel, so the 4x4 box still spans the 4x4 rotation it
cancels. The normal buffer stays Nearest, since a filtered normal is a direction
neither surface faces. The present was already reading the blurred texture by
its own uv through a linear filter, so it upsamples with no change. Without the
probe the two occlusion textures are R8, not RGBA8: only red is drawn, and the
other three channels are the census's. `KF2_AO_SAMPLES` still pins the count over
the setting.

Measured on the development machine at the water in area 0, uncapped, two frame
captures each: the AO step (normals, pass and blur) took 0.58-0.82 ms of GPU at
High, 0.14-0.22 at Medium and 0.06-0.08 at Low. `KF2_AO_PROBE=2` read the same
shading at all three: 52.4 / 52.7 / 53.3% of the picture shaded, mean 0.976,
darkest 0.68 / 0.72 / 0.74. Low with the probe off (R8) held 144.0 fps drawn at
19.9 ticks/s with `[present] wide 277`. Two things were considered and turned
down. Running the pass every second present would leave the shading a present
behind a camera the smoothing moves every present, which shows when the camera
turns. Shader micro-optimisations do little for a pass that is bound on memory
fetches. The integrated GPU itself has not been measured.

## Screen-space reflections: the water is the one surface the depth buffer does not have

**Mechanism measured; the picture has not been judged. Off by default**
(`KF2_SSR=1`, or Video ▸ Experimental ▸ *Screen-space reflections*). The runtime
half is `0067`; the port half is `patches/Reflections.cs`. The pass described here
also composites the planar and retained reflections and the murk, and runs for any
of them with the march off; see "The reflection pass runs for each term on its own".

The pass is the occlusion pass's shape: at present, between the finished target and
the blit, reading the target's depth attachment with the GTE's own H and centre, and
writing only its own texture, which the present composites (premultiplied, after the
occlusion multiply). For each pixel whose material reflects, the view ray is
reflected about the surface's plane and marched through the depth buffer — 32 steps
out to the depth where the game's fog turns black (see below), spaced quadratically
so the near field is fine,
started a fraction of a step along by the same 4x4 interleaved pattern the occlusion
pass rotates by. A step that lands behind a depth by less than a thickness (256
units plus the step's own run in Z) has hit; five halvings find the crossing, and
the colour there is the reflection, fogged for the length of the path it took (see
below). The weight is Schlick's, from `F0` looking straight down to the material's
reflectivity at a grazing angle, faded at the picture's edge and at the end of the
march.

### Why a surface buffer, and not the depth buffer

**The game draws water semi-transparent**, sorted among the opaque tiles (see
"Water on screen cost 5 ms a frame" in `docs/DEVELOPMENT.md`). A blended triangle
writes no depth, so at a water pixel the depth attachment holds the floor of the
pool and the normal buffer (`0058`) holds that floor's normal. A reflection pass
reading those would reflect off the pool floor.

So the normal pass grew a second attachment. `AoGeometry` now keeps a material per
triangle, and with reflections on it also keeps a blended triangle that has a
material. `NormalFs` writes two outputs:

- **The normal buffer, blended** (`ONE, ONE_MINUS_SRC_ALPHA`): an opaque triangle
  writes alpha 1 and replaces, a translucent one writes alpha 0 and leaves the
  opaque surface under it. The occlusion pass therefore still sees the surface
  whose depth it reads. An edge-on opaque polygon used to write zero to clear the
  texel; it now writes a zero *vector* with alpha 1, and `AoFs` treats a vector
  shorter than half as no normal.
- **The surface buffer, not blended** (RGBA16F): the last surface drawn at each
  pixel, water included — an octahedral normal, the view depth over 65536 and the
  material id. Order is the correctness argument, as it is for the normals.

With the Z-buffer on, visibility is the depth test's and not the order's, so the
pass also refuses a surface-buffer depth that is behind the picture's own depth by
more than 1% — the picture is the authority.

**The ray cannot hit the pool floor**, for the same reason: it leaves the water
upwards, and every depth under the water is further along the view ray than the
ray is. A ray that lands on another water pixel is above that pixel's floor too, so
water never reflects water.

**A miss takes the background it crossed.** Anything opaque with no depth record
stamps the far plane (`zMode 3`). A ray that finds nothing takes the colour of the
last far-plane pixel it crossed that is not under the HUD (`KF2_SSR_SKY`, 1 by
default). In `fdat02` that background is not a skybox. It is the void past the
draw distance: the colour the fallback took averaged 0,0,3 to 0,0,5.

### The HUD was reflected, because it is at the top and it stamps the far plane

Reported from play: the HUD showed in the water. King's Field's HUD is at the
**top** of the picture, and that is where water's reflected rays go: they climb the
screen. The HUD is opaque 2D, so it stamps the far plane exactly as the void does,
and the first version took it as sky. A hit could also land where the HUD covered a
surface and take the HUD's colour.

The fix is to put the HUD into the surface buffer. Each vertex now carries a
**`Projected`** bit (`HleVertex.Projected`): set when the vertex map or PGXP
answered for it, which means the GTE projected it. A polygon with no projected
corner is 2D, and so is every sprite (`DrawRect`). With reflections on, those are
kept in the surface list, in draw order, as material **`Overlay`**. They write
nothing to the normal buffer and only their material to the surface buffer. The pass
refuses an overlay pixel as a sky sample and refuses a hit under one. A 2D
primitive covering at least 90% of the target in both directions is a fade or a
damage flash the world is seen through, so it is not an overlay. Otherwise
reflections would blink out for every flash.

`KF2_SSR_PROBE=1` now prints a 48x16 material map of the readback (`.` opaque, `~`
water, `H` overlay, `*` far plane with nothing on it). In `fdat02` it shows the
HUD as two `H` blocks at the top, one on the left over the opaque wall and one on
the right inside the `*` void: exactly the samples the fallback had been taking.
Rays refused under the HUD: 0.0-0.4% of reflective pixels, depending on the yaw.
**Not covered:** a HUD piece the GTE projects (a 3D compass or item model) reads
as scene, and the first-person arm stamps the far plane without being 2D. Neither
has been seen in a reflection, but nothing would stop either. (The arm has since
been seen, the other way round: see the next section.)

### The arm showed the water through it

Reported from play with the murk on: swinging over water, the first-person arm was
drawn dark and carried the water's reflection. The arm's corners come out of the
GTE (`func_8002E650`), so they are projected, but its packets are deliberately not
recorded (`InArm`), so it draws in painter's order with no depth: zMode 3, the far
plane stamped for the occlusion pass. The surface list kept such a triangle only
as an `Overlay`, and only when no corner was projected, so the arm was not in it at
all and the surface buffer still held the water drawn under it. The pass then
composited the murk and the reflection at those pixels, over the arm.

Every opaque zMode 3 triangle is now kept as `Overlay`, except in the table's slot 0
(the skybox, which projects near and must read as no surface). Its pixels are what
is on screen and are not water; the depth buffer already treated them as no
surface. Measured in the `fdat02` New Game, facing the sea, `KF2_MURK=1
KF2_SSR_PROBE=1`, 40 presses of Square: before, the material map's lower rows were
all `~` and the 2D overlay count stayed at 16,170/s; after, the arm is an `H` block
in the lower right over the `~` and the count rises from 17,160 to about 22,300/s
while it swings. Judged by eye: fixed.

### Reflections popped in, because the path is longer than the direct distance

Also reported from play: pop-in in the reflection, of things the fog should have
hidden. The colour at a hit was fogged for the hit's own distance from the camera.
The reflected light has come further: out to the water and back up to the
surface. So a wall two tiles from a pool that is six tiles away reflected at
two-tile brightness when it is eight tiles down the mirrored ray. Anything near
the draw distance that pops in on screen as black therefore popped in visibly in
the water.

The game's fog is a **darkening**, not a blend to a colour: `shade8` in the prim
shader multiplies by `1 - w / 4096`, where `w` is one of four curves of the GTE depth
cue `IR0 = (DQA * H/SZ + DQB) >> 12`. `Gte.Rtp` now publishes DQA and DQB beside H
(`GteDepth.NoteDepthCue`). The pass evaluates the same curve at the image's depth,
`p.z * (|p| + t) / |p|`, and at the hit's own depth, and scales the hit's colour by
the ratio. That is exact for one curve, since the colour already carries the second
factor. It darkens and does **not** fade the weight, so a surface lost in the fog
reflects black, which is what the void past the draw distance reflects. The two
cannot pop against each other. The curve is per light record in the game and the
pass uses one: the knee (`KF2_SSR_FOGCURVE=2`), which is the near map tiles'.

The march stopped at 8 tiles, where `fdat02`'s fog still leaves 36% of a colour, so
a wall crossing the march's end could still flip. `KF2_SSR_DISTANCE` now defaults
to **the depth where the curve reaches black** (`ScreenReflections.March`), 21,695
units in `fdat02` (DQA -12800, DQB 20971520, H 200: black at about 10.6 tiles).
Past the end of the march everything would have reflected black anyway.

Measured, turning in place at the `fdat02` spawn (`goto` with a yaw): the share of
hits the path fog more than halved went 0.7% / 3.5% / 8.7% / 58% across four
yaws, and the longer march found more surfaces: 54.2% of reflective pixels against
51.1%, and 88.5% against 80.9%. `KF2_SSR_FOGCURVE=0` is the comparison.

### A floating object trailed down the water

Reported from play, with a picture: the gem floating over a pool reflected, and
beneath its reflection a long streak ran down the water towards the camera. A ray
from nearer water passes *behind* the gem. The hit test accepted any sample behind
a depth by less than the thickness plus the step's own run in Z, and the
quadratic spacing makes that run about 1,350 units on the far steps, so passing
behind counted as hitting.

A candidate is still halved back to where the "behind" test flips, but the ray has
to be at the surface there: within `KF2_SSR_THICKNESS` (256) of the depth. Where
it crossed a real surface, halving lands on it. Where it passed behind a thin
object, halving stops at the silhouette with the ray still far behind, and the
march goes on. The readback counts rays that passed behind something (bit 8 of its
code). At the `fdat02` spawn that was 0.7-0.8% of reflective pixels at two yaws and
0 at the others. Hits moved 54.2 -> 53.5% and 20.7 -> 19.9%, and were unchanged at
the other two yaws, so the walls still reflect. **The gem's room was not measured.**
Nothing floats at that spawn, and whether the streak is gone is for the eye.

### What a surface is made of

`SurfaceMaterial` is a small id per pixel (`None` 0, `Opaque` 1, `Water` 2; eight
slots) and a table of what each id means (`Reflectivity`, `F0`). It is the surface
buffer's alpha rather than something reflection-shaped, because **it is what
lighting will need too**: a deferred light pass wants a normal, a position and a
material at each pixel, and the first two are already exact here. Two sources, in
order:

1. **The packet.** `GtePacketDepth.Rec.Material`, carried to `HleVertex.Material`
   beside `Solid`. The port sets it where it seals a depth record, the one place
   that knows which routine, model or tile built the packet. Nothing sets it yet;
   it is where an authored material belongs.
2. **The texture.** A triangle whose texels fall in a VRAM rectangle the port has
   published takes that rectangle's material. That is how water is found. The game
   keeps its scrolling textures in eight slots at `0x80192D58`, each re-uploaded
   every tick into a fixed dest rect (see "The water still steps at the tick" in
   `docs/PATCHES_AND_MODS.md`). `Reflections` publishes every live slot as water on
   each `DrawOTag`, whether or not the fluid smoothing is on, because the smoothing
   publishes nothing at or below the tick rate.

The same slots hold the main-hall fire and the creatures' skins, so a slot rect is
**translucent-only**: it applies only to a semi-transparent polygon in an averaging
blend (modes 0 and 3). An opaque skin and an additive fire are refused, and the
probe counts refusals by blend mode. **The fire has not been measured**; the census
below is `fdat02`'s water alone. The slime skins, if any are blended at mode 0,
would reflect.

### What is measured

`KF2_SSR_PROBE=1` prints a line every two seconds: passes, presents with no target,
the published rects, water triangles against all blended depth-carrying triangles,
refusals by blend, and a readback of what each reflective pixel found. The probe
build attaches a second target to the pass and writes a code into it per reflective
pixel (no hit, surface, sky). **The readback is the only number that separates "the
pass ran" from "the pass reflected something"**: every other number reads the same
if the shader returns nothing.

`KF2_AUTOSTART=new` stands in `fdat02` facing the water:

- 5 water rects; 67,800-75,500 water triangles/s, which was **every** blended
  depth-carrying triangle in view; 0 refused by blend.
- 36.3% of the picture reflective; 51.2% of those hit a surface and 46.7% took the
  sky; mean weight 0.18 (strength 0.6, F0 0.12).
- 144.0 fps drawn at 19.9-20.0 ticks/s, `[present] wide 287-288`, no GL errors
  under `KF2_GLDEBUG=1`.
- **The occlusion census is identical with reflections on and off** (darkest 0.76,
  mean 0.994, 9.9% shaded, 71.6% surface, 100.0% geometry normals). That is the
  measurement behind the blended normal buffer: 490k triangles/s kept with water
  against 366k without, and the normals the occlusion pass reads did not move.
- GPU, from frame captures at render scale 5: the reflection pass 0.028-0.044 ms at
  2x (the default `KF2_SSR_RESOLUTION`) and 0.092-0.146 ms at the render scale;
  after the HUD and fog fixes and the longer march, 0.042-0.052 and 0.104-0.137. The
  occlusion step, which now also draws the surface attachment, read 0.367-0.369 ms
  against 0.346 without it. Uncapped, the frame went from 261-264 fps to 254-261.
- AO off, reflections on: the surface buffer is drawn by the reflection pass
  instead, and the readback is the same.
- Slot 2 (`KF2_AUTOSTART=2`): `fdat05`, hp 46/86 in area 1, 144.0 fps at 20.0
  ticks/s, `[present] wide 288`, the vertex map binding. The five rects are live
  there too, but no water was in view: 0 water triangles, 0.0% reflective.

**What still has to be judged by eye**: whether the reflections read as water or as
a smear; the strength and F0; the edge fade; whether any HUD is still reflected
(after the fix above); whether the pop-in is gone; how a flat mirror looks on this game's scrolling water (there is no
ripple yet); and how the reflection holds up while the camera moves. It is
screen-space, so anything off the screen or behind something else cannot be
reflected, and the reflection fades as its source nears the edge.

## Planar reflections: the world walked twice, from under the water

**Mechanism measured; the picture has not been judged. Off by default**
(`KF2_PLANAR=1`, or Video ▸ Experimental ▸ *Planar reflections*; the screen march
need not be on). The runtime half is `0068`; the port half is
`patches/PlanarWalk.cs`, with an arena from `patches/PrimBuffer.cs`.

The screen-space pass can reflect only what is on screen and in front of everything
else. A wall above the top of the view, or a creature behind a pillar, is simply
absent from the water. At the `fdat02` spawn, looking down at the pool, **79.5% of
the pixels the planar texture answered were ones the march could not find at all**.
This extends that pass rather than replacing it. A reflective pixel whose surface
lies on the mirrored plane takes the planar texture; any other reflective pixel,
and any whose planar texel is empty, marches as before.

### Moving the camera, not the world

A mirror turns every winding round, and the game's culling, clipper and backface
tests all assume they do not. So the port does not mirror the world. With `M` the
flip of world Y about the plane, the mirrored view `R·M` equals `M·(M·R·M)`. `M·R·M`
is an ordinary rotation: `func_80015048` builds the view as `Rx(pitch)·Rz(roll)·
Ry(-yaw)`, and conjugating by `M` negates the X and Z rotations and leaves the Y
one. The camera is therefore mirrored by position (`Y' = 2h - Y`) with the pitch
and roll negated, and the remaining `M` is a flip of the camera's own Y. The game
renders the first part as the ordinary camera it is. The pass does the flip when
it samples: the mirrored image of display row `y` is row `2·OFY - y`, with OFY read
off the GTE like everything else.

### The walks, twice

After the game's own object walk (`func_800331B4`) returns, inside stage 13:

1. The camera block at `0x80192E18` (the view matrix, the pitch-only matrix
   `0x80192E38`, position, angles, tile index) is saved. `func_8002E22C`, which
   stage 13 builds that block with, is called again with the mirrored position
   and angles.
2. The frame's arena descriptor (`0x8017E0A4`) and ordering-table pointer
   (`0x8018E0A8`) are pointed at an arena of the port's own. `PrimBuffer` now
   keeps it past its two buffers: an arena as large as one of them, a
   `0x2000`-entry table and a page of scratch, still inside 4 MB. The tile walk
   `func_80031C94` then runs over **the same 24x24 grid** the frame used. The grid
   is a plan-view footprint, and a mirror in a horizontal plane moves nothing in
   plan.
3. The object walk is **not** run again. It plays ambient sounds, uploads texture
   pages and steps the billboard clock, all once per walk. Instead every call it
   made to the submitter `func_80032588` was recorded: four registers, nine stack
   words, and the position and rotation it built in its own frame. Each call is
   made again from the same stack pointer, so every hook on the submitter sees the
   call it saw the first time. `AnimSmoothing` keys its slots on the position
   pointer, and it gets the same pointer. A matrix of 0 is a model placed in view
   space and belongs to the real camera, so it is skipped.
4. Everything is put back: the camera block, the two pointers, the model table
   (`0x8018E19C`) and vertex base (`0x8018EAA0`) the submitter moves, the fog
   word (`0x80192EA8`), the GTE and the registers.

At the frame's own `DrawOTag`, a pre-hook hands the mirrored table to the runtime's
`LibGpu.DrawOTag` directly (so no hook on the game's `DrawOTag` fires twice) with
`PlanarReflections.Capturing` set. `GlCore.Classify` then swaps every primitive's
target for that target's **planar texture**: a `GlDisplayRt` of the same size and
margin, never written back to VRAM, cleared on the first primitive of each
capture. Nothing else about a primitive changes. Perspective, sub-pixel, per-pixel
lighting, even fog and the mip atlas all come from the same records as for the
real frame, because the mirrored packets were built by the same C# assemblers. The
depth cue is computed from the mirrored camera's own SZ, so a reflection is fogged
for the length of the path through the mirror with no correction in the pass. That
is exact, where the screen-space pass's path fog is an estimate.

The pool's own floor and walls lie between the mirrored camera and the water, so
the prim shader discards any fragment on the camera's side of the plane:
`uClipPlane` is the plane in the mirrored view, and the view position is rebuilt
from the fragment's depth exactly as the normal pass rebuilds it. It keeps what is
more than `KF2_PLANAR_BIAS` (8 units) above the water, which also drops the water
itself.

### The plane comes from the picture

Nothing in the map says which tiles are water: water is found by the fluid slots'
VRAM rects when the backend classifies a triangle (see "What a surface is made of"
above). So the plane is found at that same point. Every triangle classified as
water is taken back to world Y with the camera the port published before the
frame's `DrawOTag`. A triangle whose corners differ by more than 64 units (a
waterfall) is refused. The rest are binned in 16-unit bands by the area they cover
**on screen**: the walks submit water past the picture's edge too, and at first that
kept the mirrored walk running while looking straight up. The next frame mirrors in
the heaviest band. The previous plane is kept while its band holds at least 60% of
the heaviest, so two pools cannot trade places frame by frame. No water on screen
means no mirrored walk. Neither does a camera at or below the water.

### What a pixel takes

In `SsrFs`, a reflective pixel takes the planar texture when its surface lies
within `KF2_PLANAR_TOLERANCE` (48 units) of the plane, in the view the target was
drawn with, and the mirrored texel is drawn. The capture clears to black at the far
plane, and an opaque surface writes a depth whatever its colour, so a texel is
drawn if its depth is nearer than the far plane or its colour is not black. The
weight is the same Schlick term. The lookup is bent by the water's own brightness
gradient (`KF2_PLANAR_RIPPLE`, 4 game pixels per unit of brightness change), so
the reflection moves with the scrolling texture instead of lying on it like glass.
An empty texel is looking into the void over the water, and falls through to the
march and its sky fallback.

The planar texture belongs to the target, like the depth and the surface buffer,
because with two display buffers the target presented was drawn a frame ago. It
carries the two planes it was drawn with, and it is read only while the target's
last draw is the same frame as its capture. A menu drawn over an old world
therefore gets the march alone.

### What is measured

`KF2_PLANAR_PROBE=1` prints two lines every two seconds (the plane, the walk, the
arena, the capture, the binning) and turns on the reflection pass's readback, which
now counts a fifth outcome: the planar texture. **The readback also checks the
mirror.** On the probe's frame, a planar pixel is marched as well. Where the march
found an on-screen surface too, the two colours' brightness difference is written
out, against the planar texture read *unmirrored* as the control. A mirror sampled
in the right place reads far below its control, and one sampled in the wrong place
reads like it.

`KF2_AUTOSTART=new`, the `fdat02` spawn, facing the pool:

- Plane at world Y -12160: 2240 below the eye, 640 below the floor the player
  stands on. 144 mirrored walks a second at 1.09-1.12 ms each, 2 submits replayed
  a frame, arena peak 33,940 of 409,600 bytes, no overflow, no table mismatch.
- 55.6% of reflective pixels took the planar texture; the other 44.4% took the sky
  through the march, all of it in the void to the right of the pool (the march
  alone gave 46.7% sky and 51.2% hits here). 86.0% of the planar pixels were found by the
  march too, and there the two are **3.5 apart in brightness (of 255) against
  24.2 unmirrored**.
- Turning in place with `goto`: yaw 0 is 0.2% planar and 99.8% sky (facing the
  void); 1024 is 1.4%; 2048 is 25.9%, 3.6 against 25.8; 3072 is 73.6%, 3.1
  against 23.2. Pitched down 300, 41.4% planar, and only 20.5% of those were found
  by the march. Pitched up 700 with no water on screen: 0 mirrored walks, 144
  frames a second counted as having no water.
- The same readback with `KF2_ZBUFFER=0`, with `KF2_AO=0` and with
  `KF2_POLYASM=0` (depth from the address map): 55.5-55.6% planar, 3.3-3.5 against
  24.2. No GL errors or warnings under `KF2_GLDEBUG=1`.
- **The occlusion census is identical with planar on and off** (darkest 0.76, mean
  0.994, 9.9% shaded, 71.6% surface, 100.0% geometry normals): a capture adds
  nothing to the target's surface list.
- Cost, frame profiler at 144 fps: frame work 3.56-3.76 ms without, 5.19-5.38 ms
  with; `LibGpu.DrawOTag` 0.87-0.93 to 1.24-1.29 ms; buffer swap and driver
  0.15-0.17 ms either way. 144.0 fps drawn at 19.9-20.0 ticks/s. **GPU time is not
  measured**: the frame viewer's capture skips the mirrored draw, since its
  software replay would draw the reflection into the display buffer.
- Slot 2 (`KF2_AUTOSTART=2`): `fdat05`, hp 46/86 in area 1, 144.0 fps at 20.0
  ticks/s, `[present] wide 288`, the vertex map at 63.0% hit, identical to planar
  off. No water in view, so no mirrored walk.

**Not reflected**: the skybox and anything else drawn outside the two walks (the
march still finds them on screen); the player's arm; models in view space; the
object kind `0xF0`, whose submitter is handed a translation the first walk already
projected. Only one plane is mirrored. A second pool at another height marches.

**What still has to be judged by eye**: whether it reads as water; the ripple
strength; the seam between the planar reflection and the march's sky where the
mirrored texel is empty; the rim of the pool at an 8-unit clip bias; creatures and
billboards in the reflection; and all of it while the camera moves.

## The retained scene: the world kept on the GPU, so a reflection can draw it again

**Mechanism measured; the picture has not been judged. Off by default**
(`KF2_RETAINED=1`, or Video ▸ Experimental ▸ *World reflections*; the screen march
need not be on). The runtime half is `0072` (`Gpu/RetainedScene.cs`,
`Gpu/Backends/Common/GlRetained.cs`); the port half is `patches/RetainedMap.cs`,
`patches/RetainedPlanes.cs` and `patches/RetainedModels.cs`.

The reflections above were screen-space (the march can only borrow what is on
screen) or paid for a second pass of the game's own code on the CPU (the planar
walk re-ran the tile walk and replayed every model submit, 1.8 ms a frame at the
`fdat02` spawn). Every way of drawing the world again from another camera — a
second plane, a cubemap — cost another such pass. **That is what this removes:**
the area's geometry is kept in world space on the GPU, and any extra view is a
draw with a different camera matrix. The game's code never runs twice.

### The map is data

80x80 tiles of two halves, each naming a mesh of the map's model bank
(`0x8018E18C[0]`), a height, a quarter-turn and a light record (see `TileWalk`).
`RetainedMap.Build` places each mesh's corners as `func_80031950` does — the tile's
centre `(x*2048+1024, -height*128, z*2048+1024)` plus the corner turned by the
inverse of what `func_80014B88` does to the view matrix's columns — and lights each
face once, flat, through the GTE with the record's light matrix (by the turn),
colour matrix and back colour, exactly as the assembler's `NormalColorCol` does.
None of that depends on the camera. The depth cue does, so each corner carries the
record's DQA and DQB (from the game's own `SetFogNear`, as `func_8002DDDC` calls it)
and its curve, and the vertex shader fogs it at whatever camera draws it.

The table's `+4` is not a model count: it is `TileWalk.Beyond`'s far-model limit,
and gating on it dropped 5,623 of 6,313 halves in `fdat02`. A mesh is refused only
when its header points outside RAM.

**It is the game's geometry, measured.** With `KF2_RETAINED_PROBE=1`, after the
assembler draws a half, every corner of its mesh is projected through the frame's
camera and compared with the screen word the GTE left in the vertex cache: in
`fdat02` 423,072 corners every two seconds, **100.00% within 1 px, mean 0.051 px**
against the GTE's truncation (0.67 px against the unrounded position, which is
just the truncation). A subdivided half's cache holds the subdivider's corners and
is left out.

`fdat02` is 48,837 triangles, area 1 67,501. A build takes 5.5-7.5 ms and runs
only when what it reads changes: the bytes of the map a mesh is built from (model,
height, the turn's low two bits and the light record's low six — the game writes
other bits of the map as it runs, and hashing the whole block rebuilt it every
couple of seconds), the light records, the bank pointer, the remaster's materials
and the number of water rects.

### Models, every frame

Everything the object walk hands an assembler is captured before a single face
is culled — a reflection sees the side the camera does not — from the lit assembler
(`func_8002F214`, `func_8002EAEC`) and from the tile assemblers when the object walk
calls them (`func_80030540`, `func_8002FECC`; area 1's objects are all drawn that
way). Corners come from the vertex base the transform just read, so an animated
pose is the pose drawn. The colour is `NormalColorCol` again,
without the cue. Checked the same way: 553,800 corners in `fdat02` and 84,672 in
area 1, **100.00% within 1 px**. Cost: 0.16 ms a frame of `ReplaceLit`'s time at the
`fdat02` spawn (0.194 to 0.351 ms).

**A model is placed from its record, not back through the camera.** The first
version took the GTE's view transform back to the world with the frame's camera,
whose rotation is only good to 1/4096: a static object's corners moved by a unit or
two whenever the camera turned. Nothing a reflection shows, but the lights' shadows
(`0077`) redraw a cubemap when a caster moves, and turning in place redrew one on
357 of about 475 frames. The submitter (`ModelWalk`) now publishes the model's own
rotation times its scale (the matrix before it is multiplied by the view) and its
record's world position whenever it places the model through the view matrix, and
a corner is that rotation about that position; only a model placed any other way
still goes through the camera. Measured in area 1: every model in view placed from
its record (864 of 864 in two seconds at the spawn), creatures included; corners
**100.00% within 1 px** of the GTE's (347,697, worst 2 px); and 0 redraws over 60
turns in 3.4 s. With reflections off and only shadows asking, a model is captured
without its colour, since a shadow wants only the corners.

### Drawing it: the game's own fragment shader

`WorldVs` turns a world corner into exactly the outputs `PrimVs` gives `PrimFs`, and
`PrimFs` is used unchanged, so a reflected texel is decoded through the CLUT,
filtered and blended by the same code as a drawn one. The projection is the GTE's
(centre plus H times x/z) written as a clip-space position with W the view depth, so
UV is perspective-correct and the rasterizer clips at a near plane, which packets of
projected corners never needed. Opaque triangles test and write depth; each blend
mode is drawn tested without writing, with the same dual-source factors
`FlushCore` uses. Subtractive blending needs a second pass against a copy and is
left out.

**Everything is drawn at present, for the target being presented, with that
frame's camera and models.** With two display buffers the presented target was
drawn a frame ago, so `RetainedScene` keeps the last four frames by serial and a
target records the serial it was drawn under (`GlDisplayRt.RetainedSerial`). Nothing
reflects a camera or a model from another frame.

One trap, for anything that draws between the surface pass and the reflection
pass: **dual-source blend factors left set are an error for any draw into more than
one buffer, with blending disabled** (`GL_INVALID_OPERATION`, silently skipping the
draw). The reflection pass draws into two when its probe is on, so the first build
reflected nothing only while being measured. `DrawRetained` puts the factors back.

### The planes, from the mesh

`RetainedPlanes` keeps every level face that is water (translucent, textured from a
fluid slot's VRAM rect) or carries an authored material, grouped by height, chunk
and kind when the map is built (73 groups in `fdat02`). Each frame the groups a
reflecting material covers are ranked by their area on screen from **that frame's
camera**, and the best four the camera stands above are drawn. The old finder
binned the water the backend had drawn, so its plane was the previous frame's.

All the planes share one planar texture, drawn **straight into the picture's
pixels**: a corner is drawn where its mirror image stands, seen from the real
camera, with `gl_ClipDistance` removing what lies below the plane first. No row
flip, no mirrored camera to build. Each plane's draw keeps a fragment only where the
frame's own surface buffer says the surface at that pixel lies on that plane
(`uMaskOn` in `PrimFs`; zero, the default, is the shader as it was), so a pixel of
the texture holds the reflection in the plane its own surface lies on.

### The cubemap, in place of the march

Six 256-pixel faces from the frame's camera, with a depth cube beside the colour,
drawn unfogged. A reflective pixel not on a plane marches its reflected ray **in
world axes about the camera** against the depth cube: a step whose point stands
further from the camera than the surface the camera sees in that direction has gone
behind it; halved back to the crossing, it is a hit only where the ray is at that
surface, so a ray passing behind an object marches on. The hit is fogged for the
whole path, and a rough material reads a mip of the colour cube. There is **no
per-pixel jitter** — 48 quadratic steps and six halvings, and a miss reflects
nothing: nothing on screen is borrowed, including the old sky fallback.

The ripple still bends the planar lookup by the water's own brightness gradient,
which reads the water's texture, not the scene.

### Culling

The static map is kept in 8x8-tile chunks, sorted by range and chunk. Each view —
each plane, each cube face — draws only the chunks whose box is in front, inside
its picture and not wholly past the fog's black (by view depth and the chunk's own
fog; see the next section), and for a plane, the part above it. At the `fdat02` spawn 5% of chunk tests pass and
the triangles submitted fall 17-fold (98M to 5.7M every two seconds); the coverage
the readback measures was identical before and after (61.9% planar, 58.1% cubemap).

### What the mirror showed that it should not, and the fog it dropped

Reported from play at the `fdat02` pier, 2026-09-26: the reflections did not look
geometrically right, and distant geometry popped into them with no fog on it. Three
defects, all in how the retained scene was drawn rather than in its geometry (the
corners were already checked against the GTE's):

- **Faces seen from behind were drawn.** The world program ran with culling off,
  and every face the game draws is one-sided: the assemblers cull on `NormalClip`.
  A mirror sees the scene from below, so the top of every floor, ledge and deck
  above the water is seen from behind, and with no earth under it modelled it
  showed as a floor lying under the water -- a textured slab in the reflection
  beside the thing it belonged to. Both a mirror and a cube face (all six
  rotations have determinant -1) turn the winding over, so the faces the game keeps
  are clockwise there; `DrawWorldRanges` culls counter-clockwise ones
  (`RetainedScene.CullBack`, `KF2_RETAINED_CULL=0` is the comparison). The probe
  counts it on the planar pass: pixels whose nearest opaque face is a front face,
  and pixels a back face drawn against that depth would still have taken. Over 100
  views around the spawn (a 5x5 grid of 8192-unit steps, four headings, through
  `view`), **mean 2.7% and up to 14.7% of the planes' opaque pixels** showed a face
  from behind; 1.2% at the spawn itself.
- **The distance cull was on the wrong measure.** A chunk was dropped once its
  nearest point was further than the frame's fog black plus a tile, in straight-line
  distance, with the one fog the GTE last held. The game's depth cue goes by view
  depth, and depth is distance times the cosine of the ray: at the picture's corners
  about 0.63 of it, so a chunk at the side was dropped while still lit. And each
  light record has its own fog. Now each chunk carries its latest fog (the quotient
  at which its last corner goes black, `RetainedScene.ChunkFogQ`), a mirrored chunk
  is dropped only when its nearest corner's depth is past that, and a cube face,
  drawn unfogged and fogged at the march for the whole path, drops a chunk past the
  frame's black depth over the cosine of the picture's widest ray. The probe counts
  the mirrored chunks the old rule dropped and what they kept: **every frame at the
  spawn one, keeping up to 34% of its colour; over the sweep up to 97%** -- that is
  the pop-in. More is drawn now (chunks 4320 to 6360 per two seconds at the spawn).
- **The fog was per corner.** `WorldVs` darkened each corner's colour and let the
  rasterizer interpolate it, where the game's faces, under per-pixel lighting, take
  the curve at every pixel (`0048`). The raw IR0 goes as 1/z, which is affine on
  screen, so it is now handed to `PrimFs` as `vFog` with the curve in `vLight`,
  exactly as a recorded packet is, and `shade8` fogs it per pixel. A cube face is
  unfogged, as before.

**Reflecting halves the game never drew there.** Reported next, with two pictures a
small camera move apart: a stone slab in the water beside the ledge, reaching well
past the ledge's end, with nothing above the water to be its source -- and it came
and went as the camera moved. The retained map is every half on the map, and the
game draws far fewer: the tile walk draws only what `CullGrid`'s visibility grid
lights, which is the view cone flooded out from the eye's own tile *on the eye's
level* (the lower or the upper half of a stacked cell, `func_8002B6B4`), stopped by
blocked cells and walls, and lights the other level only beside walls. So the mirror
showed the other level's halves and cells the flood never reaches, and the 8x8-tile
chunk culling swung them in and out a chunk at a time. The mirror's grid is the
camera's: the cone depends on pitch only through `rcos(pitch)`, which a mirrored
(negated) pitch leaves alone, the flood is two-dimensional, and its seed is the same
tile. So each frame now records the halves its own walk drew (`TileWalk.RunTile`,
after the far-model gate, into `RetainedScene.Frame.Halves`); every static corner
carries its half (`Flags` bits 13-26); and `WorldVs` drops any half the frame did
not draw, from a 160x80 byte texture (`uHalves`, unit 16) uploaded at present.
Models carry no half and are what the object walk drew anyway. The lights' shadow
cubemaps are not gated -- a light needs the walls the camera cannot see.
`RetainedScene.HalfGate`, `KF2_RETAINED_GATE=0` to compare. The probe draws the
undrawn halves against the gated depth and counts what they would have taken: at
the spawn **11.9%** of the planar pixels (planar coverage 61.9% to 55.6% of the
reflective pixels); over the same 100-view sweep, on average **2.2 times** as many
pixels as everything kept, and in a view out to sea (camera at 63488, 114688,
heading 3072) **99.0%** of what the mirror drew was halves the game did not draw.
Everything reflected now is something the game shows from that eye, and appears in
the reflection in the same frame it appears in the picture. **Judged by eye, 2026-09-26: worse than without the gate**, and kept for now at
the user's word; `KF2_RETAINED_GATE=0` is the comparison. Why it reads worse is not
yet known.

And one change of rule: a pixel whose surface lies on a plane but whose planar texel
is empty (open sky in the mirror) no longer marches the cubemap. The plane's answer
is exact; the cube's has the camera's parallax, and could only put something there
that is not. It reflects nothing, as a cube miss does. The probe's compare frame
still marches it, so the readback's figures do not move.

Measured after, `fdat02`, 144 fps, 16:9: 144.0 fps drawn at 19.9-20.0 ticks/s,
`[present] wide 288`, no GL errors under `KF2_GLDEBUG=1`; the planes and the
cubemap still agree (93.4% of planar pixels found by both, 3.7 apart against a
control of 24.4 at the spawn; 95.1%, 12.6 against 84.6 in the 14.7% view); the
planar pass 0.29 to 0.58 ms GPU with the probe's two counting draws in it.
With the gate: 143.9-144.5 fps drawn at 19.9-20.0 ticks/s, planar pass 0.30 ms GPU,
no GL errors. **Not judged by eye**: whether the pier and the rock now reflect as they should,
and whether a thin deck, whose underside the game never modelled, now reads as
missing from its reflection -- the price of drawing what the game would.

### What is measured

Render scale 5, 16:9, 144 fps, this machine (RX 9070 XT), `KF2_RETAINED_PROBE=1`:

- **GPU**, timer queries read a present late: at the `fdat02` spawn the planes
  0.29 ms and the cubemap 0.55 ms; in area 1 the cubemap 0.65 ms and an authored
  floor's plane 0.12 ms.
- **CPU**, frame profiler at the `fdat02` spawn: frame work 3.8-3.9 ms with the
  screen march alone, 5.6 ms with the planar walk, **4.2 ms with the retained
  scene**.
- **Coverage**, the reflection pass's readback: at the `fdat02` spawn 61.9% of the
  reflective pixels take the planar texture (the walk took 55.5% there), and with
  planes off the cubemap reaches a surface for 58.1% where the screen march found
  none.
- **The two agree.** Where a planar pixel's cubemap march also finds a surface,
  the probe writes the brightness difference, against the planar texture read the
  wrong way up as the control: `fdat02` water 93.5% of planar pixels also found,
  **3.8 apart (control 24.3)**; an authored mirror floor in area 1
  (`tile:1:37:36:upper` face 3, reflectivity 1) 100% of its pixels planar, 100%
  found by the cubemap too, **3.0 apart (control 61.7)**.
- **Off is the picture it was**: the pinned area-1 view hashes to
  `210d55698c875fb8` with it off, and with it on (nothing reflects there).
- The acceptance run with it on: slot 2 at hp 46/86 in area 1, 144.0 fps at
  20.0 ticks/s, `[present] wide 288`, the vertex map at 100.0% hit, no GL errors
  under `KF2_GLDEBUG=1`.

**Not reflected**: billboards and effects (their assembler is not captured), the
arm and anything else drawn outside the two walks, subtractive faces; authored
lights and glows (the world program runs with neither); `EvenFog`'s and
`EvenLight`'s blends between records (the retained colours are the assembler's own,
flat per face); mipmaps (the atlas entry is per batch; the anisotropic taps are
kept). The cubemap sees from the camera: something the reflecting point would see
and the camera cannot is missing from it, where a plane has it.

**What still has to be judged by eye**: all of it — whether the planar water and a
mirror floor read right and hold still while turning, whether the cubemap's
reflections on walls and props read as reflections or as a pasted image, the seam
where a plane meets the cubemap, what a miss reflecting nothing looks like against
the old sky fallback, and roughness through the cube's mips.

## Per-pixel lighting: the corner colours are the end of a chain, and the chain is known

**Mechanism measured; on by default.** One
checkbox under Video ▸ Enhancements (`kf2.perpixel.on`), `KF2_PERPIXEL=0` off on the
console. GL core backend only. The runtime half is `patches/recompone/0048`; the
port half is `patches/PerPixelLighting.cs` and `patches/PolyAssemblerLight.cs`.

A packet's vertex colour is not a free number. The GTE made it in two steps, and
the GPU then interpolates the *result* across the polygon, which is right only
where neither step bends inside it:

1. **A lit colour.** A map tile takes one `NormalColorCol` per face, so its lit
   colour is constant. A model face takes `NormalColorDpq` (flat, one normal) or
   `NormalColorDpq3` (gouraud, a normal per corner): `RGBC * (BK + LCM * max(0, LLM
   * N))`, clamped, and not yet saturated to 255.
2. **The depth cue.** `DpqColor`'s weight `w` pulls the colour towards the far
   colour: `colour * (1 - w / 4096)`, floored and clamped. The weight is `IR0` from
   the vertex's own `RotTransPers`, clamped to `[0, 4096]` and then bent by the
   game's own curve.

**The depth cue is the whole of a map tile's shading, and it is the jolt.** Measured
in play (a temporary probe, areas 1, 2, 3, 5 and 7): `H 200`, `DQB 20971520` (5120
in IR0 units), `DQA -12800` or `-11600` by area, far colour 0, `BK 1920` or `1760`,
and the three light columns `2662, 2662, 3328`. So `IR0` is 0 nearer than about
8,000 units, saturates past about 40,000, and in between is `5120 - 3.125 * div`
— affine in `1/z`, which is exact under screen-space interpolation. What is not
affine is everything done to it after:

- `func_8002E650` (the far tiles and the models): zero when `FogMode >= 32000`,
  `max(IR0 - 800, 0) * 2` when bit `0x8000` is set, and otherwise `IR0` below 2800
  and `3 * IR0 - 5600` above it — a knee, reaching black at 3232. `FogMode` read
  14500 and 16000 in play, so the knee is what is drawn.
- `func_8002E7CC` (the near tiles): the knee, or zero.
- `func_800302E8` (anything the view-space clipper cut): `IR0 >> 1`, no knee.
- The models: **one weight for the whole face** — the first corner's for a gouraud
  face, the mean of the corners' for a flat one.

A two-tile floor with one corner at `IR0` 2000 and the next past the knee is
interpolated as a straight ramp between two colours the knee put in different
places, and the clamps at 0 and black land wherever the corners happen to be — both
of which move as the camera does. A creature's face steps whole as it walks.

### What is recorded, and how the GPU finds it

`PolyAssembler` records each packet's inputs as it builds it, into
`GteLightMap`, keyed by the packet's address in the two primitive buffers
(`0x800FC99C`, `0x32000` bytes). Per corner: the lit colour, or for a gouraud model
face the three `LLM * N` dots before their clamp; and the raw depth cue, `MAC0 /
4096` read straight after the transform's `RotTransPers`. Per packet: the curve,
the light colour, and the command and first vertex words once the packet is
finished. `DrawPolygon` looks the packet up by the address `DrawOTag` read the
command word from and believes it only if both words still match. `BK` and `LCM`
are uniforms, by generation; a directional batch flushes if the generation moves
(`FlushReason.StateLight`), which is an area load.

**Nothing in the recording writes guest memory or the GTE** — reads go straight to
the RAM array, and the lighting products come from `Gte.LightProducts`/`LightDots`,
which read `LLM`, `LCM` and `BK` without touching a register — so
`KF2_POLYASM=verify` compares the same routine it always did: over areas 1, 2, 5, 6
and 7 with the feature on, **0 RAM, 0 register and 0 GTE mismatches** in every
routine.

Three details that the counters found:

- **A vertex-cache slot has to be the transform's own.** The tile assemblers bump
  a serial before calling their transform, and a slot the transform stamped under
  that serial is fresh. The models' cache is checked against RAM instead (the
  screen word and the fog word), because `func_8002EA60` writes the same cache with
  `RotTransPers` and a fog word of 0 — about 190 corners a frame in area 1, the
  first person arm among them — and a stale raw value from an older mesh would put
  a clamp in the wrong place. Such a face falls back to the fog words as they are
  (`CurveWord`), which for `func_8002EA60`'s zeros is exact.
- **The clipped emitter is recompiled, and does not need porting.** Its packets are
  found after the call by the buffer cursor: packet `k` is records 0, `k + 1` and
  `k + 2` of the output list, lit by `NormalColorCol` (recomputed from the normal)
  and fogged at half each record's raw value, which `Survivors` stores beside the
  record.
- **A face that comes out the same interpolated is not recorded**: every corner
  unfogged, or fogged to black. That is most of the near geometry in a small room,
  and it keeps the light buffer out of batches that do not need it.

### The shader

`shade8()` in the core-profile `PrimFs` makes the colour again at each pixel, from
the corner values interpolated `noperspective` (the raw depth cue is affine on
screen, as the colour was): the lit colour or `RGBC * clamp(BK + LCM * max(0, dots)
/ 4096) / 4096`, then the curve on `clamp(raw, 0, 4096)`, then `floor(lit * (1 - w /
4096))` clamped to 255. With no record it returns the interpolated vertex colour,
bit for bit as before. The dots are interpolated, not the normal, so this is the
GTE's diffuse model evaluated per pixel — the terminator and the colour clamp land
where they belong — rather than a renormalised Phong.

The attributes are in **a vertex buffer of their own**, uploaded only for a batch
that carries a record, with attributes 7-9 disabled otherwise (their generic value
reads as no record). That is not tidiness: the first version widened `GlVertex`
from 44 to 64 bytes, which took its `0x40000`-vertex buffer to exactly 16 MiB, and
**that alone took area 1 from 861 fps to 265-305 with the feature switched off** —
`GlCore.Flush` 0.04 ms to 2.7 ms a frame, and every GPU timer in a frame capture
1.7x slower, the AO pass included. Reverting the shader and disabling the attributes
changed nothing; restoring the stride did. Split: 856 against 861.

### What is measured

- **The formula against the GTE, in play.** `KF2_PERPIXEL_PROBE=2` evaluates the
  shader's formula at every recorded corner of a gouraud packet and compares it with
  the colour the GTE wrote there (a gouraud model face at its first corner's weight,
  since that is what the GTE used). Areas 1, 2, 5, 6 and 7: 1,491,187 corners exact,
  21,280 off by 1 (the GTE's fixed point against float), **none off by 2 or more**,
  over the knee, the half curve, the fallback and the directional model light.
- **The shader against the formula, headless.** `scripts/light_probe.c` drives the
  real `PrimFs` over seven strips sweeping the lit colour, the dots and the raw
  depth cue through every curve, untextured and true colour: **worst difference 0**
  in all seven, 120-167 distinct colours across a 256-pixel strip where the curve
  bends.
- **Coverage.** 96-99.9% of polygons found their record before the uniform faces
  were dropped (the rest are the HUD and sprites); the probe line now also counts
  the dropped ones as drawn from their corner colours.
- **Cost**, `KF2_FPS=1000`, one session per run with a temporary build cycling off /
  recording only / on every second: area 1 standing +0.03 to +0.09 ms a frame, areas
  2 and 7 +0.05 to +0.19 ms, about half of it recording and half the draw side.
  At 144 fps it is 144.0 drawn at 20.0 ticks/s.

**Nobody has looked at it.** Whether the tiles read as smoother rather than as
different, and whether a creature lit per pixel still looks like itself, is the
question. Leave everything else at its default while judging it.

### A clipped tile is fogged at half, and that is the block on the floor

Reported from play near save 3 (area 2): floor polygons "darker or brighter
depending on distance from the camera", unchanged with AO off. Not the lit colour:
every floor face in view shares one normal and one `NormalColorCol` result.

**The cause is the emitter after the view-space clipper.** `func_800302E8` fogs
each surviving record at **`IR0 >> 1`**, where both tile transforms (`func_8002E7CC`
for the near tiles, `func_8002E650` for the far ones) fog at the knee, `IR0` below
2800 and `3·IR0 − 5600` above it. So a tile the clipper cuts gets half its
neighbours' fog.

**Which neighbours** took a second probe to see. The near transform writes `otz`
as 0xFFFF unless RotTransPers's flag is exactly 0x1000, which is IR0 clamped at 0.
So **any near tile with a fogged corner fails the `z == -1` test and is clipped**,
and no unclipped near corner carries fog at all. Its neighbours at a fogged
distance are the far tiles, `func_8002FECC`. The first diagnosis's "unclipped"
column was those far tiles.

**Fixed as an enhancement, on by default** (`EvenFog`, `KF2_EVENFOG=0` off, Video ▸
Enhancements ▸ *Even fog and lighting*). After `func_800302E8` returns,
`PolyAssembler.Clipped` rewrites each emitted `POLY_GT3`'s three corner colours from
the lit colour and the near curve of each record's `IR0` (`+0x14`), or no fog when
`FogMode >= 32000`. The arithmetic is DPCS's own, done in C# with no GTE register
written; checked against `Gte.Dpcs` itself on every clipped corner, 0 of about 50k
a window differed. A packet past the buffer's end is left alone, as
`LightClipped` does. It only runs from the C# assembler, so it needs Fast geometry,
and it stands down under `KF2_POLYASM=verify`: with `KF2_EVENFOG=1` verify reads 0
RAM, register and GTE mismatches in every routine.

Measured at save 3 during the autostart load, with the probe spinning the base
yaw a full turn every ten seconds so every direction is sampled. Brightness is
corner R over the face's lit R (x1000), then the fog weight. Only far tiles drawn
under the same DQA are compared (below):

| view Z | far tiles | clipped, the game's | clipped, with the fix |
|---|---|---|---|
| 8.0k | 987.3, 32 | 990.0, 16 | 986.5, 32 |
| 8.2k | 958.1, 154 | 977.2, 76 | 958.6, 154 |
| 8.6k | 902.5, 383 | 947.3, 193 | 900.5, 385 |
| 9.2k | 826.9, 692 | 908.8, 346 | 826.5, 692 |
| 9.5k | 792.8, 830 | 897.3, 414 | 795.5, 828 |

Every 100-unit bin from 8.0k to 9.5k agrees within about 2 on that scale. Below
8k both paths are unfogged. **Per-pixel lighting** records a refogged packet with
the same curve as a tile (`CurveKnee`, or `CurveNone` in far mode), not `CurveHalf`.
A face with every corner unfogged is no longer recorded, as a tile's is not.
`KF2_PERPIXEL_PROBE=2` over the same run: 2.39M corners, **0 off by 2 or more**,
and no `curve3` left.

### Fog changes at a tile edge

**Fog is not global: every map tile names its own light record.** In the same runs
as above, some of `func_8002FECC`'s corners were drawn with DQA −10000 instead of
−12800, and at the same view Z they were much darker (8.2k: 693, fog 1239).
`func_80031950(half, &position, flags)` draws one half of one tile. It reads the
half's byte `+4 & 0x3F` as an index into the `0x68`-stride records at
`0x801930F0`. It loads that record's light matrix for the half's quarter turns
(`+2 & 3`, 0x14 bytes each), its colour matrix at `+0x50` and back colour at `+0x62`.
It passes the `+0x66` word to `func_8002DDDC`, which stores it as `FogMode` and calls
`SetFogNear((word & 0x7FFF) >> 1, 200)`. So DQA is `-(word/2)·320/200` and DQB is
always 1.25. Only then do the tile assemblers run. Near save 3, the room carries
16000 (DQA −12800, fog from about 8k) and the corridor under the arch 12500 (DQA
−10000, fog from about 6.25k). The step between them follows the tile grid.

Reported from play with only the clipped fix in: "the area under the arch is
noticeably darker than the surrounding area in an unnatural way". Removing the half
fog had made it plainer, because the corridor's clipped tiles were no longer drawn
lighter than authored.

**Blended as the second half of `EvenFog`** (`KF2_EVENFOG_BLEND=0` keeps the hard
edge). A pre and post hook on `func_80031950` hold the half being drawn, but only
while one of its eight neighbours' records carries a different fog word, so most
tiles pay one lookup. Inside that window, both tile transforms and the clipped
refog compute each vertex's fog weight bilinearly between the four tile centres
around it: the tile's own at its centre, half and half at an edge, a quarter each
at a corner. A neighbour's weight is its own word's curve (near or far, as that
transform picks) at its own IR0. That IR0 comes from the vertex's SZ3 through the
GTE's own divide: `Gte.DepthQuotient` is `0049`, the depth-cue half of `Rtp`'s
`Divide` with no flag raised, and the port applies the neighbour's DQA to it.
Things the geometry forced:

- **The mesh is centred on the tile and turned by the half's quarter turns**
  (`func_80014B88`: turn 1 maps local `(x, z)` to map `(z, −x)`, 2 to `(−x, −z)`,
  3 to `(−z, x)`). Its edge vertices sit at **±1034**, ten units past the tile's
  own ±1024, so neighbouring meshes overlap. Both sides clamp at 1024 to get the
  same half-and-half weight.
- **An empty half (model ≥ 240) or off the map has no record, so it drops out** and
  the remaining tiles share its weight. Treating it as "the drawing tile's own"
  looked harmless and was not. Two tiles meeting at a corner beside the same empty
  half gave it their two different words, and the corner still stepped by up to 278.
- **Per-pixel lighting records a blended vertex as `CurveWord`**, the weight
  itself, since no single curve describes a mix of four.

Measured at save 3 during the autostart load, holding the view at eight headings
for three seconds each. The probe matched world points that two different tiles
both transform (edges clamped to ±1024, view Z within 2) and compared their fog
weights:

| | points compared | differ by 0-1 | 2-4 | 5-50 | 51+ | worst |
|---|---|---|---|---|---|---|
| hard edge (`KF2_EVENFOG_BLEND=0`) | 73.1k | 66.8k | 1.1k | 0 | 5.2k | 1482 |
| blended, empty halves as own | 86.6k | 82.9k | 1.1k | 0 | 2.6k | 278 |
| blended, empty halves dropped | 74.1k | 73.0k | 1.1k | 2 | **0** | 9 |

What is left is one unit of SZ3: each tile rounds its own translation, so the same
point projects a unit apart on two tiles. The recomputed IR0 under the tile's own
word equalled the GTE's on every vertex checked (about 1.2M in one run), which is what
confirms the DQA formula. `KF2_PERPIXEL_PROBE=2`: 2.16M corners, 0 off by 2.
Verify with `KF2_EVENFOG=1`: 0 RAM, register and GTE mismatches in all seven
routines. Uncapped at save 3's resting view: 327-328 fps off, 329-331 on. No
record edge is in view there, so that only shows the idle cost is nothing.

Looked at after the blend: "looks good".

### The light colour changes at the same edge

The same probe dumped both records. Room record #16 (fog 16000) and corridor #0
(12500) have identical light matrices and back colour (`0x78,0x78,0x78`). Their
colour matrices differ: `0x0933`/`0x0B80` against `0x0CCB`/`0x0FFE`, the
corridor's about 39% brighter. The tile assemblers light a whole face once, with the
drawing tile's record, so the light steps at the tile edge just as the fog did.

**The game blends lighting itself, for objects.** `func_80032588` takes two records
and a weight. It mixes their colour matrices with `func_80015930` into a stack matrix
for `SetColorMatrix`, and their fog word and each back-colour byte with
`func_800158C8`, which is `a + ((b − a)·t >> 12)`. It does not mix the light
matrix: it multiplies one record's by the object's rotation. So a blend across tile
edges follows the game's own recipe rather than inventing one.

**Blended as the third part of `EvenFog`** (Video ▸ Enhancements ▸ *Even fog and
lighting*, on by default; `KF2_EVENLIGHT=0` keeps the hard light edge). It was a
separate *Even lighting* checkbox at first; the two fix the same step for fog and
for light, share the hook and the Fast geometry requirement, and neither reads
right without the other, so they are one control, and the dimmed checkbox says
Fast geometry is needed. The same
`func_80031950` window marks a tile whose neighbours' colour matrix or back colour
differ (`+0x50` to `+0x64`). Then each face corner is lit with the four records'
colour matrix and back colour (`byte << 4`, as `SetBackColor` loads it) mixed on
the same bilinear weights and empty-half rule as the fog. The tile's own light
matrix is used for its quarter turns, followed by NormalColorCol's arithmetic
(`LightStage`, then `(RGB·IR << 4) >> 12 >> 4`) with no register touched. Such a
face gets a colour per corner instead of one. The fill writes each corner's colour
before its depth cue. The clipped path lights each record by its interpolated local
position and rewrites the packet, fogged on the tiles' curve. (Its branch at the
emitter's own `IR0 >> 1`, light without the fog fix, is no longer reachable from a
switch.) Per-pixel lighting records the per-corner colours.

Measured at save 3 over the same eight-heading sweep, with both switches on and
with the light blend alone:

- recomputing a corner with only the tile's own record matched the GTE's colour on
  every corner checked (about 150k a window, 0 differ), which is what confirms the
  matrix layouts and the arithmetic;
- world points two tiles share with the same world normal: up to 28k a window,
  **0** differ by 2 or more in any channel, worst 0;
- about 73% of the corners on marked tiles came out blended at yaw 0 and 512;
- `KF2_PERPIXEL_PROBE=2`: 1.9M corners, 0 off by 2;
- verify with both on: 0 RAM, register and GTE mismatches in all seven routines, and
  nothing blended.

Its cost was not measured with an edge in view.

Looked at, both switches on, at the arch near save 3: "looks good". Both still ship
off, as enhancements of the game's own behaviour.

## Dithering: one flag, and it lives in the draw environment

The 4x4 crosshatch over every shaded surface is the GPU's ordered dither, and the
port reproduces it faithfully on both render paths — the software rasterizer adds
the table entry in `GpuRaster.Plot`, the hardware backend packs it into the vertex
texpage word and applies it in `quant5` in the fragment shader. Both read the same
single bit of GPU state, `Gpu._dither`, and `SetDrawMode` sets that from bit 9 of a
GP0(E1) word and from nowhere else.

So removing the dithering is not a rendering change at all. It is one question:
**can an E1 word with bit 9 set still reach the GPU?** There are three routes, and
they are worth separating because only two of them are hookable:

1. **`PutDrawEnv`**, from `DRAWENV.dtd` at `env+0x16` — `LibGpu.GetMode` turns that
   byte straight into bit 9. **This is the only route this game uses**: exactly one
   dithered draw env per frame, 30 a second at 30 fps, on the title screen and in
   `fdat02` alike.
2. **A `DR_MODE` or `DR_TPAGE` packet linked into the ordering table.** Measured
   over the same two, **zero** — the game sets the mode once a frame and never
   mid-frame. (`SetDrawTPage` would build one with the bit already clear;
   `SetDrawMode` is the one that could set it.)
3. **Recompiled MIPS writing GP0 through the trapped register**, from the parts of
   libgpu that are not mapped to the runtime's HLE. **Nothing here can hook this**,
   so it is checked instead of intercepted: `KF2_NODITHER_PROBE=1` samples GPUSTAT
   bit 9 after every frame, and it read 0 for whole sessions of title screen and
   play. That register
   read is what closes the loop — without it, "the two hooks fire" would only be
   evidence about the two routes that were already known.

`patches/NoDither.cs` covers routes 1 and 2 and reports on all three under
`KF2_NODITHER_PROBE=1`; the switch itself is in System ▸ Settings ▸ Video. It
**restores what it clears** — the `dtd` byte and any E1 word are cleared in the
pre-hook and put back in the post-hook — so game memory is identical either side
of the call and nothing survives switching it off. That matters most for the
packet buffer: some of it is built once and re-sent for the rest of the run, so a
bit cleared in place would stay cleared long after the setting was turned off.

It was `mods/nodither` first, and the conversion left the measurement exactly
where it was: **the counters and the GPUSTAT sample stay behind
`KF2_NODITHER_PROBE=1`, on the console.** The settings page is one checkbox and a
tooltip. A per-frame report is what you want while establishing which of the three
routes a game uses; it is not what belongs in a graphics settings window next to
vsync, and putting it there would have been the conversion quietly promoting a
diagnostic into UI.

The ordering-table scan steps **command by command** using the GP0 command lengths
rather than searching for bytes that look like E1: a colour or a vertex word can
perfectly well carry 0xE1 in its top byte, and clearing bit 9 of a coordinate moves
geometry. It stops at the two commands whose length depends on data that follows (a
polyline, and an image load); neither occurs in this game's tables.

**It is a pre/post hook, not a replacement, and that is deliberate.**
`HookManager` allows exactly one `Replace` owner per function — a second owner's
replacement is refused with `replace conflict on …` — and the widescreen patch owns
`DrawOTag`. Pre- and post-hooks compose with a replacement and with each other, so
the patch (`dither: off, 12 hook(s)` — pre and post on both entry points in all
three overlays), the widescreen patch and the frame pacing's own `DrawOTag`
post-hook all coexist.

Steady state with the patch in, probe on: `30 draw envs/s and 0 ordering-table
words/s asked for dither, over 30 frames/s; GPUSTAT dither bit 0` — one dithered
draw env per frame intercepted, route 2 unused, route 3 never taken.

## True color: the other answer to 15-bit banding

**Confirmed mechanism; picture is the point of the switch.**

The console renders into 15-bit VRAM (RGB5A1), so every shaded pixel is quantised
to five bits per channel — 32 levels. A wall that darkens with distance is a smooth
per-vertex Gouraud shade interpolated across the polygon; where that gradient
crosses each 1/32 step the framebuffer bands. The dither above is the hardware's
own answer: it adds the 4×4 table before the truncation and spreads the step, at
the cost of the crosshatch. With the dither off — this port's default — the bands
show raw. That is the banding a player sees on a fog wall.

True color is the second answer, and it removes the banding *without* the
crosshatch. Two things enforce the five-bit truncation, and both have to give:

- **The render-target format.** Geometry rasterises into a `GlDisplayRt` whose
  colour attachment is an `Rgb5A1` texture, so even a full-precision fragment is
  crushed to five bits on write. `patches/recompone/0021` makes that attachment
  `Rgba8` when `GteDepth.TrueColor` is set. The mask/STP bit rides the alpha either
  way — one bit in 1555, the top of an 8-bit alpha in RGBA8 — and reads back
  `>= 0.5` in both.
- **The fragment shader.** `quant5` (both the GLSL 330 and the 120 path) ends in
  `min(c8 >> 3, 31) / 31.0`. Under `uTrueColor` it returns `c8 / 255.0` instead and
  skips the dither, which has nothing to dither into on an 8-bit target.

**Only the shaded gradient gains precision, not the texture.** Textures live in
15-bit VRAM and are sampled at five bits (`t8 = floor(texel*31+0.5)*8`, and the
CLUT/`fetch16` paths through `u5`), so the texture palette stays exactly the
console's — the picture is authentic where its precision *was* the texture, and
stops banding only where its precision was the framebuffer. The
writeback/​sync blits between an `Rgba8` target and 15-bit VRAM convert
automatically (`BlitFramebuffer` down/up-samples), so a true-colour target still
leaves 15-bit content in VRAM for anything that later samples it. Presentation
reads the target texture directly (`PresentFs` is a plain `texture()` fetch, no
re-quantise), so an `Rgba8` target presents smooth.

**GL backend only.** The software rasterizer keeps a 15-bit VRAM and is always the
console's precision; the switch does nothing there. Toggling at run time is safe:
`GlCore` compares `GteDepth.TrueColor` against the format its live targets were
built with and, when they differ, writes each target back to VRAM and drops it at
the next present — the next draw recreates it in the new format and re-syncs from
VRAM, costing one transition frame that presents from VRAM.

**Off by default**, so the default picture is the console's 15-bit output. Unlike
sub-pixel and the Z-buffer this is not off because the picture is unchecked — it is
off because 24-bit shading is deliberately *not* what the hardware did, and a
player who wants the authentic look should get it without a package to load.
(`KF2_TRUECOLOR=1` forces it on for the run.)

**It is not a checkbox of its own any more, and neither is the dither.** The two
are answers to the one question this section is named for, so they are asked once,
as the three-entry `Shading` combo under Video ▸ Enhancements — `Dither
(original)` / `None` / `Smooth (24-bit)`. `None` is what both switches' defaults
already were, so nothing anybody had saved changed meaning. See "Two shading
checkboxes were one question asked twice" in `docs/PATCHES_AND_MODS.md`.

## Anisotropic filtering: a pixel covers an area, and the console read a point

**Confirmed mechanism; picture never checked by eye since the taps were held
inside the texture and mipmaps added (`0060`, below).**

This is the *minification* half of the story perspective correction tells about
interpolation. A screen pixel does not cover a point of a texture, it covers an
area, and the shape of that area is the parallelogram spanned by the two screen
derivatives of the texture coordinate. Square-on to a wall it is about a square.
On a floor running away to the horizon, or along a corridor wall seen edge-on, it
is a long thin sliver — many texels along one axis and barely one across the
other. The console read a single texel out of that sliver, and *which* texel it
read changes completely for a sub-pixel movement of the camera. That is the
crawling, sparkling floor, and it is the artefact the port's own render scale
makes more visible rather than less: more output pixels means more independently
sparkling samples of the same sliver.

`patches/recompone/0041` samples along the sliver instead, in both prim fragment
shaders, driven by `GteDepth.Anisotropy`, with `patches/Anisotropic.cs` as the
switch (`KF2_ANISO=<1..16>`, `KF2_ANISO_PROBE=1`, and a combo under
Video ▸ Enhancements).

### Why it is a shader change and not a sampler setting

The obvious implementation — `GL_TEXTURE_MAX_ANISOTROPY_EXT` on the VRAM sampler
— does nothing at all here, and both reasons are what make this a shader change.
They are the same two the port records against any filter placed on this
geometry:

- **The VRAM texture is not a texture.** It is a 1024×512 (×`GlVram.Scale`) sheet
  holding every texture page, every CLUT and both display buffers at once. A
  filter running across it bleeds one texture page into the next, and bleeds a
  *palette* into the palette beside it.
- **A paletted texel is an index, not a colour.** In the 4-bit and 8-bit modes the
  value read from the page is a CLUT index, and the average of index 3 and index 4
  is index 3.5 — a different colour entirely, with no relation to either. Any
  filter has to run *after* the palette lookup, which is inside the shader by
  construction.

So `decode(raw)` does the whole per-texel job — texture window, the 8-bit page
wrap, the page fetch, the nibble/byte extract, the CLUT lookup — and the kernel
calls it once per tap. The single-sample path calls the same function, so with
the filter off the fragment is the one the port drew before, to the bit.

### There is no mip chain, and there cannot be one

*In VRAM.* `0060` builds one where the texture is decoded instead — see "Mipmaps
where the texture is decoded" below. What follows is still how the kernel works
wherever no atlas entry answers.

A modern GPU does anisotropy as *N bilinear taps at a LOD chosen from the minor
axis*, and the LOD is what stops the major axis needing a tap per texel. The
first reason above forbids a mip chain outright: one page's lower level would
average in the pages beside it and a CLUT's would average in the palette next to
it. So this is supersampling — the taps run along the axis the footprint is
longest on, **one texel apart**, `min(ceil(len), uAniso)` of them, centred on the
pixel.

**The spacing is the part that had to be got right, and the first version got it
wrong**; see "The taps were spread over the whole footprint" below. Spreading
`uAniso` taps across the *whole* major axis is what a mipmapped filter does, and
it is only correct there because each tap is a pre-filtered sample covering the
gap to the next one. Here a tap is a point sample, so the honest choice is
between estimating the whole footprint from a strided handful and filtering the
part of it nearest the pixel exactly. This does the second.

Three consequences are worth recording rather than glossed:

- **Past `uAniso` texels of footprint it filters a part rather than the whole.**
  A distant floor may read fifty texels per pixel; at 16 taps the kernel averages
  the middle sixteen and degrades toward nearest beyond that, rather than toward
  a full box filter. That is the price of the reach cap, and it is the right one:
  the alternative reads other textures (below).
- **A footprint large on *both* axes is still averaged along one of them only.**
  The short axis keeps the console's single sample. That is the right trade in
  this game — it magnifies far more often than it minifies, and the artefact being
  chased is the anisotropic one — but it is a limit, not a completeness.
- **The cost is paid only where the footprint is actually long.** `taps` is
  `ceil(length(majorAxis))` clamped to the setting, so a surface facing the camera
  takes one tap at 16× exactly as it does at Off. That is why the measurement
  below shows no frame-rate difference rather than a small one.

### The taps were spread over the whole footprint

Reported from play, at oblique angles, against the first version: *it barely looks
different, there are white artefacts on the floor, and the fire billboards show a
bit of texture overlap at the edge of the texture area.* All three are one line —

```glsl
vec2 t = vUV + axis * ((float(i) + 0.5) / float(taps) - 0.5);   // the defect
```

— which places the taps across the **whole** major axis whatever its length, so
the spacing is `len / taps` texels and the reach is `±len/2`. At the very angles
the filter exists for, `len` is not 8 or 16 texels but tens or hundreds.

**A derivative is in unclamped texture-space units, and a texture is not.** A
floor tile is 64 texels wide inside a 256×256 page that holds three others beside
it; the page itself sits in a 1024×512 sheet holding every page, every CLUT and
both display buffers. Once the reach passes the texture's own width the taps wrap
— by the texture window, or by the page's `& 0xff` — onto **other art**. In the 4-
and 8-bit modes that art is a table of *indices*, and they are read through *this*
primitive's CLUT, so the colour that comes back is not a blurred neighbour but an
arbitrary entry of an unrelated palette. Bright entries are the **white speckle on
the floor**. On a billboard, whose sprite is one small rectangle among many in its
page, it is the **neighbouring sprite arriving at this one's edge**.

And it barely looked different because a strided undersample of a two-hundred-
texel span is not a low-pass of anything: it is sixteen more chances to sparkle.
The probe shows that directly at a 64-texel footprint, where the old kernel's
spread *rises* from 2 taps to 4 —

| `uAniso` | spread (sd), taps over the whole span | spread (sd), taps one texel apart |
|---|---|---|
| 1 | 59.42 | 59.42 |
| 2 | **21.18** | 48.44 |
| 4 | **25.06** | 33.90 |
| 8 | 19.55 | 16.86 |
| 16 | 12.97 | **7.09** |

— a filter whose output gets *noisier* as you give it more taps is not filtering.
The one-texel spacing is monotonic. (At a 16-texel footprint the two are the same
kernel at 16 taps by construction, which is why the table further down is
unchanged there.)

**What the probe cannot see is the reach itself**, and that is worth stating
plainly: its texture is white noise, where every texel is independent, so reading
a texel a hundred away and reading the one next door are statistically the same
draw. The reach is arithmetic rather than a measurement — `±len/2` before,
`±taps/2` after — and the picture it produces is the user's to judge.

**The second defect was the silhouette.** The colour came from a coverage vote —
average the solid taps, discard below half — while `texel.a` came from the centre
tap, so the two disagreed about what the pixel was. A fragment whose own texel is
transparent, which the console did not draw at all, was drawn whenever half its
taps came back solid: the sprite grows outward by up to half a footprint, into
exactly the neighbouring art the reach was already reading. The centre tap decides
the silhouette now, alone, so it is bit-for-bit where truncation put it, and the
kernel does not run at all on a transparent fragment.

### Two things the hardware's encoding forces

These are shared with any filter placed after the CLUT and neither is optional.

**A transparent texel is black.** The PlayStation marks a texel transparent by
storing it as all zero — RGB 0 with the STP bit clear — so a plain average next to
a punch-through edge averages *black* in and draws a dark fringe round every
grate, torch and bush in the game. Each tap is therefore weighed by whether it is
solid, and the colour renormalised by the surviving weight.

**The silhouette is not the filter's to decide.** Weighing the transparent taps
out is a statement about colour only: whether the fragment is drawn at all is the
centre tap's answer and nothing else's, which is where truncation put it. An
earlier version discarded below half coverage instead, on the argument that half
is where the edge sits — see "The taps were spread over the whole footprint" — and
that both grows and shrinks a punch-through edge by up to half a footprint. An
even tap count puts no tap at the centre, so every tap can come back transparent
on a fragment that is drawn; the average is then simply not applied.

**The semi-transparency bit is a mode, not a colour.** `texel.a` selects whether
the fragment goes through the blend equation at all — it is what picks between
`uBlend` and `uBlendOpaque` on the dual-source output. Interpolating it would ask
the GPU for a state halfway between two blend equations. It is taken whole from
the centre tap, which is the texel the unfiltered path would have read.

### It needs no "is this 3D" test, and that is the part worth keeping

Any filter that softens texels has to be kept off the HUD, the menus, the 2D
screens and the billboard sprites, which are drawn at or near 1:1 where there is
no detail to recover and only text and icons to blur. That normally costs a test,
and the only test available here is whether the exact GTE vertex map answered for
the primitive's vertices — which **exists only while perspective correction is
on**, so it has to fold in `!Enabled` or the whole picture silently reverts with
the combo still claiming otherwise.

Anisotropy needs none of it. A 2D primitive is axis-aligned and unminified, so
both derivatives are about one texel, the major axis spans one texel, `taps` comes
out 1, and the fragment takes the single-sample path — the same `decode()` call,
bit for bit. **The kernel is self-gating on exactly the geometry it should be**,
with no varying to carry, no dependence on the vertex map, and nothing to go wrong
when perspective correction is switched off. It is the one place in this file
where the cheap answer is also the complete one.

**A billboard is not in that list, and the first version of this section said it
was.** The HUD, the menus and the 2D screens are drawn at 1:1 and do gate
themselves out. A billboard sprite is a *world-space quad*: it minifies with
distance like anything else, so its footprint is genuinely several texels and the
kernel genuinely runs on it. That is correct — a distant torch flame should be
filtered — and it is why the two defects above showed up on the fire before they
showed up anywhere else: a sprite is a small rectangle in a shared page, so it is
the geometry with the least room either side of it for a filter to reach.

### Scope

**GL backend only**, on both the core-profile and the GLSL 120 paths; the software
rasterizer is always a single sample. **The native VRAM paths only** (`texMode` 0,
1, 2/3, and only while `vRepClut` is clear): an asset pack's replacement textures
and replacement CLUTs already sample real GL textures through their own sampler
state, which is where their filtering belongs. Nothing is rebuilt when the setting
changes — it is a plain uniform the next batch reads, unlike true color, which has
to rebuild its render targets.

### What it measures

The shader loop is bounded at 16 and `Anisotropic.Level` is clamped to 1..16, so
raising the ceiling needs both changed together.

`GteDepth.AnisotropyLive` is the counter that matters, and it exists because of
what this port has already been bitten by twice: the RAM fast path went round the
vertex map's hooks and left `[KF2] perspective: on` printing at boot over a dead
mechanism, and a hook summary counted registrations rather than detours. A picture
switch that cannot reach the shader should say so. It is set from the one place
that uploads the uniform, and `KF2_ANISO_PROBE=1` reports it on the first frame
that has actually presented — the prim program is built on the first present, so
it cannot be asked at `RuntimeReadyEvent`:

```
[KF2] aniso: on, up to 8 taps
[KF2] aniso: level 8, uniform bound and uploading
```

Both shader pairs were compiled and linked through Mesa directly — headless EGL,
the same driver the runtime uses — because `glslangValidator` cannot parse GLSL
120 at all and its SPIR-V mode rejects the dual-source outputs the prim shader
needs. Both link, and `uAniso` survives optimisation in both (locations 16 and
18), which is what says the kernel is reachable rather than folded away. That was
the check the GLSL 120 path actually needed: its loop bound is a constant with a
`break` inside precisely because 1.20 does not promise dynamic loop bounds.

The port's own acceptance run at `KF2_ANISO=8`: `open` → `game` → `fdat02` →
`fdat05`, slot 2 restored at HP 46/86 in area 1, **144.0 fps drawn at 20.0
ticks/s**, no exceptions and no `compile failed` or `link failed`. The same at
`KF2_ANISO=16`, with **no frame-boundary warning** — worth stating because a
short run killed during the area load does print one, and that reads as a
regression when it is only a load that never reached steady state.

### The frame rate cannot answer this one, and what can

Uncapped in area 1, the port draws **861.6 fps at `KF2_ANISO=1` and 860.7 at
`KF2_ANISO=16`** — no difference at all. That number is worth nothing on its own,
and it is the exact shape of mistake this file keeps recording: the port is
CPU-bound at ~860 fps, so the whole GPU cost of the kernel hides under the
recompiled MIPS, and "costs nothing" and "never runs" produce the same reading.
`AnisotropyLive` says the uniform arrives; it does not say a pixel changed.

`scripts/shader_probe.c` answers that directly — it drives the **real** fragment
shader headless, with a substitute vertex shader supplying its varyings, over a
noise texture at a chosen footprint, and reads the pixels back. Extract `PrimFs`
from `GlShaders.cs` into `PrimFs.frag` (strip the eight leading spaces of the raw
string literal), then:

```bash
gcc -O0 -o /tmp/shader_probe scripts/shader_probe.c -lEGL -lGL -lm
FOOT=16 /tmp/shader_probe PrimFs.frag 1 2 4 8 16
```

**The statistic is the spread across neighbouring pixels, not the mean.** Pixels
covering nearly the same texels returning wildly different colours *is* the
sparkle; a filter that works collapses that spread. Over a 16-texel footprint:

| `uAniso` | min | max | mean | spread (sd) | range |
|---|---|---|---|---|---|
| 1 | 99 | 247 | 173.75 | **51.23** | 148 |
| 2 | 107 | 238 | 167.00 | 35.57 | 131 |
| 4 | 123 | 222 | 169.62 | 24.87 | 99 |
| 8 | 132 | 206 | 170.69 | 24.38 | 74 |
| 16 | 140 | 181 | 162.88 | **11.41** | 41 |

A monotonic 4.5x collapse, which is the mechanism working and is the number to
re-take after any change to either shader. (Re-taken after the tap spacing was
fixed; the 1 and 16 rows are unchanged, since at a 16-texel footprint sixteen taps
one texel apart *are* sixteen taps over the whole span. The rows between moved
because they no longer stride.)

**The self-gating claim is measured rather than argued.** At a footprint of 1.0
texel per pixel — a HUD sprite, a menu box, a font glyph — `uAniso=1` and
`uAniso=16` return *identical* pixels (min 66, max 247, mean 170.98, sd 53.86,
both), and the same at 0.5 texels. Filtering begins at 2 texels (sd 61.24 ->
41.33) and reaches the table above at 16. So "a 2D primitive takes the unfiltered
path by construction" is a reading rather than a hope.

**One honest limitation the probe found.** Tap spacing is uniform, so it can alias
against periodic texture content when the taps are fewer than the texels spanned.
The first version of this test used one-texel stripes, and a period-2 pattern
against an even tap spacing puts *every* tap on one parity — which read as the
filter doing nothing at 2 and 4 taps and as a wildly dark result at 8. That is a
property of mip-free supersampling rather than of this kernel (a mipmapped
anisotropy prefilters along the minor axis and cannot hit it), it needs texture
content periodic at almost exactly the tap spacing to show, and the noise figures
above are the fair case. It is recorded because that stripe run looks like a bug
report and is not one.

**Off by default.** The mechanism is measured; the picture **has** been looked at
once, and that is where the two defects above came from — the frame rate and the
noise probe both read healthy while the kernel was reading other textures, which
is the same shape of mistake this file keeps recording. What has not been looked
at is the picture since they were fixed: whether a receding floor stops crawling,
whether the white speckle and the billboard's edge are gone, and how the average
reads against the 15-bit quantisation, since `quant5` still crushes the filtered
result to five bits unless true color is also on and the two have never been seen
together.

### The taps still left the texture at its edge

Reported from play after the one-texel spacing: *mipmaps and anisotropic filtering
do not work, and there are artefacts on the floor at an oblique angle.* The spacing
fixed the reach in the middle of a texture and left it at the edge. The kernel
reaches `±taps/2` texels from the pixel's own, so a pixel within eight texels of a
floor tile's edge at 16x still read the art beside the tile in its page, through
this polygon's CLUT.

`scripts/shader_probe.c` measures it directly now. `LEAK=1` paints every texel
outside the polygon's rectangle pure red and keeps the grey noise inside, `UC`
puts the sweep's pixels at the rectangle's edge, and only pixels whose own texel is
inside are counted, so any red at all is a tap that left:

| footprint | shader | 1 tap | 4 taps | 16 taps |
|---|---|---|---|---|
| 4 texels, `UC=65` | before | 0 | **66** | **66** |
| 4 texels | after | 0 | 0 | 0 |
| 16 texels, `UC=90` | before | 0 | 0 | **99** |
| 16 texels | after, and with mipmaps | 0 | 0 | 0 |
| 64 texels, `UC=98` | before | 0 | 0 | **99** |
| 64 texels | after, and with mipmaps | 0 | 0 | 0 |

(red picked up, 0-255, rectangle u 64..127.) A leak of 99 is a pixel two fifths of
the way to an unrelated colour — the speckle.

**Every tap is clamped to the polygon's texture rectangle now** (`0060`): `u0..u1`,
`v0..v1`, inclusive, handed to the shader as a flat per-vertex attribute. It is the
bounding box of the polygon's own UVs, which is exact for a whole face — except for
a clipped fan, which carries only the part of its face's UVs that survived the
clip. For those the port records the face's rectangle by packet address
(`GteTexRect`, filled by `PolyAssembler.TexRectClipped` after `func_800302E8`,
checked by the command word and the first and last vertex words like the depth
records), and the GPU takes the record over the fan's own bounds. So a fan filters,
and keys the atlas below, exactly as its unclipped neighbours do; without the record
its taps would clamp at the clip seam and it would make an atlas entry of its own
for every fragment of a face. Measured in area 1: about 1,600 fan packets recorded a
second, all but a handful found by the GPU.

The centre tap is not clamped: it is the console's texel and stays where
truncation put it. A model triangle's rectangle is its own UV bounds, which may
include skin around it; that is the same texture, so it is a softer edge, not
another texture's colour.

### Mipmaps where the texture is decoded

**On by default since v0.3.0** (`KF2_MIPMAPS=0` to compare), chosen before the
picture was judged by eye; the filtering level stays off.

**One slider since, and 16x by default.** *Texture filtering* is a slider with
fixed positions, Off / Trilinear / 2x / 4x / 8x / 16x, and the *Mipmaps* checkbox
is gone. Every position past Off turns mipmaps on. The fourth combination the two
controls allowed, filtering without mipmaps, was never better: the same cost
(717-722 fps against 720-722 below) and a kernel that cannot average a footprint
past 16 texels. It is left to `KF2_MIPMAPS=0`. A saved level above 1 now implies
mipmaps whatever the saved `kf2.mipmaps.on` says; Off saves both keys off. A first
run starts at 16x. Measured: `[KF2] aniso: on, up to 16 taps, mipmaps on`, the
uniform bound and the atlas built (500 entries, none evicted). **16x as the default
has not been judged by eye.**

A mip chain cannot live in VRAM, for the two reasons at the top of this section,
and it does not need to: it can live where the texture is **decoded**. `0060`'s
`GlTexCache` keeps a 2048x2048 RGBA8 atlas with levels 0-8. A textured polygon
minified on screen asks it for its texture — keyed on page, mode, CLUT and texel
rectangle — and a miss decodes that rectangle through its CLUT into a block of its
own and builds the block's levels by a 2x2 box.

- **A block is a power of two, on a boundary of its own size** (a buddy allocator,
  8 to 256 texels), so every level of it is aligned too, a 2x2 box never reads
  another block, and a bilinear tap held half a level texel inside the block never
  reads one either. Past the texture's own width the block repeats its edge.
- **Premultiplied by solidity.** A transparent texel is black and weighs nothing;
  the shader divides the sum of the taps by their total weight, which is the level
  0 rule of the plain kernel carried down the chain.
- **Decodes run at the start of the batch that asked**, reading the same 1x VRAM
  the prim shader reads. Anything that changes VRAM flushes the open batch first,
  so a decode sees VRAM exactly as the polygons that asked for it did. An entry is
  decoded again when `VramTracker`'s generation over its texels or its CLUT moves
  (`VramTracker.Clock` skips the check while nothing has been written). Eviction
  takes the oldest entries not drawn this frame or last.
- **What asks.** A polygon whose texel area is less than 1/32 of its screen area
  at the render scale is magnified, and so is never mipped and makes no entry.
  Neither does a replacement texture, a texture-window primitive, a sprite (always
  1:1) or a texture under a `0053` fluid rectangle (its sub-tick scroll lives in
  the shader, not in the atlas). They all keep the plain kernel, clamped to the
  rectangle.

The shader takes the footprint's two axes, `n = min(ceil(major/minor), uAniso)`
taps along the long one, and `lod = log2(max(major/n, minor))`. At `lod <= 0` a tap
covers no more than a texel and the plain kernel runs, one texel apart, which is
exact there. Above it, `n` taps are spread across the **whole** long axis, each one
trilinear between `floor(lod)` and the level above — correct here, unlike the first
version of the plain kernel, because each tap is now a prefiltered sample covering
the gap to the next. **Level 0 of that blend is the exact console texel**, not the
atlas's bilinear one, so the picture is continuous where the footprint shrinks to a
texel and mipmapping starts. At `uAniso` 1 it is ordinary isotropic trilinear.

The filtered colour also stops being rounded to five bits before the modulate:
`texel * 248` is exactly `n << 3` for a texel read straight out of VRAM, so the
unfiltered picture is bit-identical, and a filtered one keeps its fraction until
`quant5` (true colour keeps it outright).

**Measured.**

- The atlas's contents, not just its existence: `KF2_ANISO_PROBE=2` reads four
  decoded entries back. In area 1 every one held real colours (64-196 colour runs
  in a block) at 23-66% solid, and its last level matched the mean of its level 0
  to within one step on every channel — 128.0,120.9,83.0 against 128,121,82.
- The probe with `MIP=1` builds the same atlas on the CPU. At a 64-texel footprint
  the plain kernel at 16 taps can only average 16 of the 64 texels (sd 10.2); the
  mip path averages all of them and the spread is below one 5-bit step (sd 0.0).
  At a footprint of 16 taps or fewer the two are the same kernel, as the `lod`
  rule says they should be.
- Area 1, slot 2: 504 entries, decoded once each, none evicted, none refused;
  144.0 fps drawn at 20.0 ticks/s, `[present] wide 288`. Uncapped at a 2000 fps
  ceiling, off 750-757 fps; 16x without mipmaps 717-722; 16x with mipmaps
  720-722. The filters cost about 0.06 ms a frame, and that is the rectangle, not
  the atlas.

**Off by default**, as the plain kernel always was: the mechanism is measured and
the picture has not been looked at. What wants looking at is a long floor at a low
angle with `Texture filtering` 16x and `Mipmaps` on — whether it stops crawling,
whether the white speckle and the billboard's edge are gone, whether the distance
is soft rather than muddy — and the same with anisotropic off, which is isotropic
trilinear and should look blurrier along the floor.

**GL core backend only.** The GLSL 120 path has neither the rectangle nor the
atlas and runs the plain kernel as it was.

## The render scale did not survive a menu

**Measured mechanism; the picture is what the report was.**

Reported from play: *in subscenes like when you open the menu, or talk to an NPC,
the render scale is overwritten in the 4:3 area in the middle of the screen, and
it renders at 1x.* That sentence names the mechanism precisely, because the middle
of the screen is exactly the region that goes through VRAM.

A modal sub-loop keeps the world behind it without redrawing it: it reads the
finished frame out of VRAM into system RAM once (`StoreImage`, GP0 `0xC0`) and
blits that copy back at the head of every iteration (`LoadImage`, GP0 `0xA0`), so
each pass erases the last one's menu boxes and text against a still world. **That
roundtrip is 1x by construction** — VRAM is the console's own 320×240, and
`GlCore.ReadVram` reads it back at that resolution whatever `GlVram.Scale` is — so
the restore stamped a one-sample-per-game-pixel picture over the display area
every single frame the menu was up.

Why the *middle* and not the whole picture: `Writeback` copies a render target's
middle `W` columns into VRAM and nothing else, because the widescreen margin lives
nowhere but in the render target ("The margin's only clear is the game's own" in
`docs/WIDESCREEN.md`). So the game's own 320 columns are the only ones a VRAM
roundtrip can reach, and the margin stayed at full scale beside them — which is
the seam the report describes.

Measured with `KF2_VRAMPROBE` (an ad-hoc census, not committed), at 16:9 and scale
4: in an area, every VRAM upload is at `x = 320` — texture space, outside the
display area — and the display columns are never written. Press Circle and the
census reads `read 1` followed by `write … 0,240 320x240` at 60 a second, forever.

**The fix is to keep a scaled copy of what was read.** `ReadVram` now also blits
the region into a snapshot texture at `GlVram.Scale` (`SnapTake`), and `WriteVram`
compares the 1x pixels it has been handed against that snapshot's; on a match it
blits the scaled copy back into VRAM instead of uploading (`SnapRestore`). The key
is **the content and the size, not the address** — the frame may be restored into
either display buffer, and identical pixels are identical wherever they land.
Anything the game actually built or changed in RAM fails the compare and takes the
upload path exactly as before, so a texture, an MDEC frame or a decoded sprite is
untouched; a readback smaller than 64×64 is not snapshotted at all, being a tile
rather than a frame. Two slots, LRU, invalidated by a scale change.

Measured at `KF2_FPS=144`, 16:9, scale 4, autostart into slot 2, with
`KF2_VRAMSNAP_PROBE=1`: in the menu, **120-121 restores per two seconds and 0
uploads that missed**, with the present census still reading `wide 180, plain 0,
vram fallback 0` and the world still at 20.0 ticks/s. In an area the counters are
`0, 0` — nothing reads the frame back, so the mechanism costs nothing. Boot and
the area transitions read `39 restored, 13 uploaded` and similar: the loading
screen and the fades do the same roundtrip and gain the same way, and the misses
there are the real image loads.

**Counting restores says the path fires, not that it wrote the right pixels**, so
under the probe the first restore of a run is read straight back out of VRAM at 1x
and compared against the upload it replaced — a scaled copy of the same frame must
downsample to the same picture, so any disagreement is the blit's geometry rather
than the resolution. Measured: `verify 320x240 at 0,0: 0 of 76800 pixels differ`.
That it lands at `0,0` and the menu's at `0,240` is also the evidence for the
address-independent key: both display buffers are restored from the one snapshot.

`KF2_VRAMSNAP=0` is the comparison and puts the 1x upload back. There is no
control in the window, because a render scale that survives a menu is not a
choice. **Not looked at by eye** — what is measured is that the restore is served
from the scaled copy on every frame of a menu, not that the menu now looks like
the world behind it.

The mechanism is `patches/recompone/0039`. GL backend only: the software
rasterizer has a 1x VRAM and nothing to preserve.

## A shop overwrote the textures with the atlas's old texels

**Fixed; confirmed by eye in the shop.** Reported from play: after opening and
closing a shop, the room's walls and ceiling showed flat colour with scattered
old texels. `KF2_VRAMSNAP=0` cured it; the primitive buffer move
(`KF2_PRIMBUF=1`) and water-texture smoothing (`KF2_SMOOTH_FLUID=0`) did not.

**`0039` took its scaled copy from the atlas, and `0054` stopped keeping the atlas
current.** A readback of 64×64 or more copied the region out of the scaled atlas,
keyed by the 1x bytes. `0054` sends an upload to 1x sample VRAM and to any display
target it touches, but no longer to the atlas, so outside a display target the
atlas holds whatever was last promoted there. When the shop uploaded a region
byte-identical to an earlier readback of texture space, the restore blitted those
old texels into the atlas and `Publish` copied them down into the 1x textures. The
menu's own frame restore was unaffected, because it reads the display area and a
writeback keeps that part of the atlas current.

**The copy is taken only when a display target covers the whole rectangle, and
from that target**, which draws and uploads both keep current. Any other readback
gets no copy, so its upload takes the 1x path; texture space is 1x anyway.
`KF2_VRAMSNAP_PROBE=1` counts those as `readback(s) outside a display target not
copied`. Measured after: the in-game menu still restores from the scaled copy,
116-120 restores a window, first restore `0 of 76800 pixels differ`.

**Why the VRAM check missed it.** `KF2_VRAMCHECK=1` mirrors every upload, copy and
fill on the CPU and compares 1x sample VRAM after each VRAM operation, naming the
one that changed pixels it should not have. The first version adopted a restore's
rectangle as the GPU's own, which is exactly where this wrote. It now checks
restores against the bytes the game uploaded.

**Open: a menu restore differs from its upload by 2525 pixels.** With the check
on, every restore of the in-game menu after the first leaves 2525 pixels in
`(0,0)-(319,233)` that differ from what the game uploaded (the first restore
verifies at 0). It is inside the display area, so it cannot touch textures, but
the restored frame is not exactly the game's. Not looked at.

## The first intro movie never reached the screen

**Fixed (`0054`, amended); the mechanism is measured, and the picture still needs a look by eye.** Reported
from play: the ASCII Entertainment logo at boot is black, but its jingle plays and
the FromSoftware logo after it shows. The three boot movies are `OP0.S`
(ASCII), `OP1.S` (FromSoftware) and `OP2.S` (the intro). All three decode and upload
the same way, 15-bit, 320x240, twenty 16-pixel slices per frame, flipping
between `(0,0)` and `(0,240)`. The logs cannot tell them apart.

**The pixels were in VRAM and not on the screen.** Reading the display rect
back from 1x VRAM after each `DrawOTag` found the ASCII frames there, blue
content fading to full white, up to 35% of the picture lit. Reading back the
present's own output found `mean 0, max 0` for the whole movie, at 16:9 and at
4:3.

**`0054` sends an upload to the scaled framebuffer only when no display target
exists.** That framebuffer is the present's fallback, and the rule treats "there
is a target" as if it meant "a target will present this". Boot's first
`PutDrawEnv` (`isbg=1`, 640x240 at `(0,240)`) creates one target that nothing
draws into again. `KF2_PRESENT_PROBE=2` shows the two buffers failing for two
different reasons:

- A frame shown from `(0,0)` is `no target covers the display`.
- A frame shown from `(0,240)` is `margin latch refused` (`latch -1000`, idle
  counting up).

Both fall back to a framebuffer that never received a slice. The target is
destroyed after 300 idle presents, which falls right at the switch to `OP1.S`, so
uploads promote again and the second logo shows.

**An upload is now promoted unless a target that the present will take contains
it** (`ServedByTarget`). The present's own latch test is the same
`MarginRefused`, so the two cannot disagree. Measured after: the presented ASCII
frames read `mean 8-48, max 126-255` where they read 0, and FromSoftware reads what
it did before. Uploads outside every target now promote too, in texture space
during play. In `fdat02` facing the water at `KF2_FPS=1000` that cost nothing:
270 fps against 265, `GlCore.Flush` 0.565 ms against 0.569. The acceptance run is
unchanged (144.0 fps drawn at 20.0 ticks/s, `wide 288, vram fallback 0`).

## The display list cannot name a face: why packet-level smoothing failed

The port smooths between logic ticks by carrying *tables* — the camera in
`patches/FrameSmoothing.cs`, four model tables and the entity records in
`patches/ObjectSmoothing.cs`. That set has been found incomplete three times, each
time by a person noticing something step in play, and item 6 in
[TODO.md](TODO.md) is the standing request to stop finding them that way.

The generic alternative tried here was **packet-level interpolation**: one layer
below the tables, at `DrawOTag`, the frame is a finished list of primitives; if the
same primitive can be recognised in the previous tick's list, its screen position
can be carried by the tick phase, and one hook covers doors, tiles, enemies and
animation alike. It does not work for models, for a reason worth writing down
because it is not obvious and because the measurement that appeared to endorse it
was measuring the wrong thing.

`patches/PacketMatch.cs` (`KF2_PACKETMATCH=1`) is the probe, and it is kept. It is
a measurement only — it never writes to game memory.

### Where a primitive's identity comes from

A read-only pre-hook on `DrawOTag` walks the ordering table exactly as
`Widescreen`'s replacement does (`patches/Widescreen.cs:409-450`). Not
`RenderPrimEvent`: that event carries four screen positions, four flags and a CLUT,
and nothing else — no texture coordinates and no source address — so nothing
intrinsic to a face is visible from it.

Which object a packet belongs to is read off the **primitive arena**, the same
mechanism `patches/DrawCensus.cs` attributes bytes with: the game bumps a
`{start, end, current}` descriptor at `0x8017E0A4` once per polygon, so the packets
a call produced are the addresses between its entry and exit `current`. Two calls
are attributed:

* `func_80032588`, the model submitter — `a2` is the position pointer, which names
  the table slot and covers all four model tables at once.
* `func_80031950`, the map-tile submitter — `a0` is the tile record address **plus
  the half offset** (`S0` or `S0+5` in `func_80031B1C`), so it already distinguishes
  a tile's two drawn halves and needs no separate counter.

Three keys were measured, and a fourth number decided it:

| key | definition |
|---|---|
| **K1 ordinal** | `(kind, contextId, index within context)` |
| **K2 lerpable** | K1 matched *and* the same primitive shape |
| **K3 intrinsic** | `(kind, contextId, hash of command byte, UVs, CLUT/texpage)` |
| **correctness** | of the primitives K1 matched, how many matched a face with the **same texture coordinates** |

### The ordinal matches, and names the wrong triangle

K1's hit rate is high and it is **meaningless on its own**. Back-face culling
submits only the faces pointing at the eye, so dropping one polygon shifts every
ordinal after it by one: the key `(slot, 5)` still exists on both sides of the
tick, so it counts as a match, while now naming a different face. Measured, models,
one second a window:

| what the player was doing | K1 matched | …and was the same face |
|---|---|---|
| standing perfectly still | 100% | **100%** |
| walking or turning | 92–100% | **14–40%** |

Map tiles survive it — 76–100% correct under the same motion — because a tile is a
simple static mesh whose visible-face set barely changes. Models do not.

**This is the trap to remember.** A match rate answers "did the key find
something", and the question is "did it find the *right* thing". The first reading
of this experiment reported 96.6–99.3% and concluded the design was viable; the
second key, K3, was disagreeing with it at 56–94% the whole time, and that
disagreement was read as K3 being the weaker key rather than as the two keys
contradicting each other. Applying it looked exactly like what it was: **"every
object becomes super garbled the moment I move"**, because two thirds of the
carried motion belonged to some other triangle.

### The other thing a screen-space carry gets wrong

Even with a correct key, a primitive's whole displacement is the wrong quantity to
carry. The frame is already drawn through a camera `FrameSmoothing` has advanced to
this frame's phase, and through positions `ObjectSmoothing` has already
interpolated; adding the full per-tick delta adds both a second time and runs
objects ahead of the world on every turn. The quantity to carry is the **deviation
from the context's mean** — the mean being the object moving as one, which is
already carried, and the deviation being the pose. The probe reports that split as
`pose split`, gated on the camera having held completely still, because parallax
from a moving *eye* survives the mean and would otherwise be counted as pose.

That measurement is worth keeping whatever happens next, because it is the evidence
that there is anything to fix: with the camera frozen and two enemies attacking,
over twenty-five consecutive windows, the objects' own translation was 0.1–0.8 px a
tick while the motion of their primitives *relative to each other* was **3.4–13 px
a tick on 9–13% of contexts, peaking at 36**. There is real animation the tables
cannot reach — the model pipeline has no skeleton, so a pose is vertex data or a
swapped model index (see "The model pipeline has no skeleton" in
[GAME_INTERNALS.md](GAME_INTERNALS.md)) — it simply cannot be recovered from the
display list.

### Two defects in the probe itself, both fixed and both instructive

* **Vertex colours are not identity.** The intrinsic key hashed them at first and
  read 0–5% on the world against 92–99% for the ordinal. They are the game's own
  per-frame shading, recomputed as the camera moves, so a face never hashed the same
  twice; the HUD, whose colours are constant, was the only thing that matched, which
  is what named the cause.
* **The UV word's upper half is pad on the third and fourth vertices.** Only the
  first two carry the CLUT and the texpage. PSY-Q does not clear the rest, and the
  arena hands out memory two frames stale, so hashing it made the key differ from
  itself with the camera standing still. `Gpu.DrawPolygon` reads exactly the three
  fields that mean anything.

### What would work instead

The identity problem is created by projection and culling, so it does not exist
before them. Inside `func_80032588`, a vertex is identified by its index in the
mesh — exact, not inferred — and a morph applied there comes out through the
game's own transform, so there is no camera to subtract and no mean to take out.
`patches/AnimSmoothing.cs` no longer hooks that fetch. Creature pose is an
MO clip: `func_8003486C` already produces a 12.12 weight from integer time,
and `func_80034A74` already morphs. Driving that clock between ticks lets the
decoder write the in-between mesh; interpolating object-space `SVECTOR` reads
did not, because most submits are rigid architecture (`CurAnim >= 0x80`) and
the first-person arm is posed by the MO clip clock like everything else. Tile
height is a different, smaller
problem and is not this. See "The model pipeline has no skeleton" in
[GAME_INTERNALS.md](GAME_INTERNALS.md).

## "No textures on the other machine": splitting the three layers

Reported from play on a Windows PC with an Nvidia card: the game runs, the HUD is
correct, and every wall, floor and ceiling is one flat colour. The dev machine
(AMD RX 9070 XT, Mesa 26.2, GL 4.6, `Gl45`) draws the same scene textured.

A picture cannot say which layer failed, and there are three, each of which
produces *exactly* the same flat-shaded surface:

1. **The game submitted flat polygons.** Textured-ness is a bit in the GP0
   command the game itself wrote — `tex = (cmd & 0x04)` in `GpuRaster` — decided
   long before any GL call. If the game's own state is wrong, no renderer change
   puts a texture back.
2. **The VRAM page is uniform.** A textured polygon sampling a page that never
   received its upload draws one colour per surface. Indistinguishable from (1)
   by eye.
3. **The fetch is wrong.** Textured prims, populated pages, and still no texture
   leaves `PrimFs`'s `texelFetch` — the only part of this that is vendor-specific.

`patches/TexProbe.cs` (`KF2_TEXPROBE=1`) separates them, writing to
`texprobe.log` beside the saves as well as to the console, because a Windows
release is a `WinExe` and its console output goes nowhere a player can send. It
hooks nothing and writes nothing to game memory: the census is a
`RenderPrimEvent` listener, which **both** renderers pass through and which
carries `Textured`, `Raw`, `Gouraud` and `Clut`; the page count is a
`ReadVram(0,0,1024,512)` from a `VSyncEvent` listener — the backend's own thread,
once a second, since the read stalls the pipeline — reduced to the number of
*distinct* 16-bit words in each of the 32 texture pages. A page holding art holds
hundreds; a page that never received its upload holds one.

Measured on the dev machine, in an area, at `KF2_FPS=60`:

```
gl: AMD | AMD Radeon RX 9070 XT (radeonsi, gfx1201, ACO) | 4.6 (Core Profile) Mesa 26.2.0-devel
backend Gl45  ready True  active True  scale 4  truecolor False  perspective True  subpixel False  zbuffer False
prims/s 70500  textured 70440 (99%)  flat 60  raw 0  gouraud 69600  semitrans 17400  distinct cluts 11
vram y0  :  150  147   98  142  196 1000 1000    2 1000 1000 1000 1000 1000 1000 1000 1000
vram y256:  358  452  446  479  394 1000 1000 1000  428  270  800 1000 1000  423  122   76
```

**That reading is from the shipped launcher, not the developer build**, run
against a fresh data directory (`VERDITE2_DATA`) so every setting is at its
release default — true color off, sub-pixel off, dither on, 4:3. It is the same
99% textured picture the developer build gives, so **the release path is not the
cause and this is not a packaging regression**; the difference is the other
machine. The developer build reads the same census with the dev config
(true color on, sub-pixel on, 16:9).

### What the other machine can be asked without a new build

Three controls in the shipped release already split the remaining space, and all
three are in System ▸ Settings ▸ Video (each needs a restart):

* **Backend → `gl33`.** Same fragment shader, completely different VRAM path: no
  `glTextureBarrier`, no `glCopyImageSubData`, ping-pong FBOs for the destination
  read. Textures returning here indicts `Gl45Vram`.
* **Backend → `gl21`.** A *different shader* (`PrimFs120`, GLSL 120, no
  `texelFetch`, no dual-source blending) and a different VRAM path again.
  Textures returning only here indicts the 330 shader.
* **Render scale → 1.** `uScale` becomes 1, so `fetch()` stops striding
  (`texelFetch(uVram, c * uScale, 0)`) and `WriteRect`'s upload blit stops
  magnifying 4x. Textures returning at 1x indicts the scaled VRAM texture.

And **Debug ▸ VRAM viewer** answers layer (2) on its own, by eye, in one look.

Never checked: any of this on Nvidia hardware. Nothing here owns an Nvidia GPU,
so the vendor half of the question is a report rather than a measurement.

### Murky water

**Mechanism measured; the tuning judged by eye. Off by default**, at the user's
tuning when switched on (depth 2654, the colour unchanged; `KF2_MURK=1`, or Video ▸
Experimental ▸ *Murky water*). Runtime `WaterMurk`
(amending `0067`), port `patches/Murk.cs`.

Water was clear to the bottom. The reflection pass now lays a murk under the
reflection on water (material 2): the depth buffer holds the opaque floor under a
translucent surface and the surface buffer the water itself, so the view ray's run
between the two is the water it crosses, `1 - exp(-run / KF2_MURK_DISTANCE)` of a
dark teal (`WaterMurk.R/G/B`, fogged at the water's depth), sky behind the water
counting as all water. It is per pixel and independent of world height, so a pond
above the player does not darken anything else. Default 2654 units (a tile is 2048;
700 until it was judged).
Under the checkbox, *Murk depth* (100-8000, logarithmic; `kf2.murk.distance`,
which `KF2_MURK_DISTANCE` overrides) and *Murk colour* (`kf2.murk.r/g/b`) set both
live, with a reset back to 2654 and `0.03,0.05,0.06`.

**The tiles' cracks as lines of murk.** Reported: seams between water quads with
the murk on. The likely cause, not measured: the water tiles and the floor under
them meet with hairline cracks the game's own picture hides, and the murk turned
each into a seam: a surface texel no water triangle covered took no murk, and a
floor texel no floor covered read as the sky's full run. `SsrFs` now takes such a
texel as water when the texels either side of it on one axis are, and a missing
floor depth from its nearest neighbour. Judged by eye: the seams are gone.

**A halo round the pier's pillars.** Reported with a screenshot at `fdat02`'s
pier (player `75773,-11520,83101`, yaw 1586, pitch 35; `view 75773 -13026 83101 35
1586 0`), with the murk on and nothing else mattering. Measured at that camera with
`snap`, three faults, each checked by its own before and after:

- **Water with no floor under it.** The cells along the pier have no seabed: with
  the murk off the water there lies over black. The murk takes the sky behind water
  as a full run, so those cells are solid teal, flat patches ending at the cell
  edges beside each pillar. **Kept as the look.** Three fills of that floor from
  the water around it were built and reverted (`084707d`, `577d480`, and murking
  what is drawn under the surface instead, `0b1b034`); the user preferred this
  version's water, and only the two edge fixes below were carried forward.
- **The crack fill borrowed the pillar's depth.** The fill for a one-texel crack in
  the floor took the nearest neighbouring depth, and beside a pillar that is the
  pillar, in front of the water: a run of 0, and a strip of unmurked water about a
  game pixel wide down both sides of every pillar. Only a depth behind the water
  counts now.
- **The pass's resolution.** The pass runs at `KF2_SSR_RESOLUTION` (2x the game's
  pixels) and the present read it bilinear, which put a teal fringe on the pillar's
  own edge. `PresentFs` upsamples it by the surface under each pixel (`ssrAt`): what
  the pass computed from at the pixel and at each of the four texels round it (the
  surface buffer's material and view depth, cracks filled as the pass fills them,
  nothing where the depth buffer has an opaque surface in front), nothing where
  that is nothing, and otherwise only the texels with the same material and a depth
  within 10%, renormalised, or the nearest such. The surface buffer is drawn at the
  render scale while the pass runs, and the depth is bound for the composite on
  unit 4.

Measured, with the fill, at the pier: 330 fps uncapped with the murk against 336
without, and no GL errors. The two edge fixes without the fill have not been
measured or checked by eye.

### The reflection pass runs for each term on its own

The pass at present used to be switched by the screen-space reflections, and the
murk, the planar walk and the retained scene all rode on it: planar and retained
reflections did nothing without `KF2_SSR=1`, and the murk nothing without a
reflection. Each is its own switch now (`ScreenReflections.Enabled`,
`WaterMurk.Enabled`, `PlanarReflections.Enabled`, `RetainedScene.Enabled`), and
`GteDepth.Reflections` -- the pass, the surface buffer, the materials and the water
rectangles -- is on while any of them is (`ScreenReflections.Refresh`). With the
march off (`uMarchOn` 0) a water pixel takes a planar lookup where one answers and
reflects nothing elsewhere; the retained cubemap still marches, being world space.
This is the seam for taking the screen march out: what it still lends the rest is
`ScreenReflections.March()`/`FogBlackDepth()` (the cubemap's reach), the fog curve,
the pass's resolution and the probe's readback.

Measured on `fdat02`'s water (`KF2_AUTOSTART=new`, 144 fps, each alone, the others
forced off): murk 36.3% of the picture reflective, 0% marched, mean weight 0.945;
SSR 53.5% of that hit a surface and 46.3% the sky, as before; planar 55.6% planar
and 0% marched; retained 55.5% planar and 0.2% from the cubemap; none of them, no
pass. 144.0 fps drawn at 19.9-20.0 ticks/s and `[present] wide 288` in each.

### Water waves

**Mechanism measured; the tuning judged by eye. Off by default**, at the user's
tuning when switched on: swell 338 over 6114, ripples 139 over 700, shade 0.51,
speed 1 (first 96 over 12000, 48 over 700 and 0.25; `KF2_WAVES=1`, or Video ▸
Experimental ▸ *Water waves*). Runtime `WaterWaves`
(`0078`), port `patches/Waves.cs` and `patches/WaterSwell.cs`.

**The water is coarse, and that decides what a vertex can do.** A census of
`fdat02`'s bank: open water is model 18, one quad a tile, used by 3,022 halves;
a pool's edge is two to eight triangles a tile; every water face maps the same
64x64 image once per tile, so the picture is that image on a 2048-unit grid. A
vertex is at best a tile corner, so moving vertices can only make a long swell.
Breaking up the grid is the texture's job, per pixel. The feature is both.

**The swell** lifts and lowers the water's own vertices. A half's mesh is read in
three places -- the vertex transforms and the clipper through `VertexBase`, the
subdivider `func_80030C94` straight from the bank's header -- so for the length of
a half with water `WaterSwell` copies its vertices to `PrimBuffer.WaveScratch`
(past the mirrored walk's scratch), moves the copy's Y, and points the bank
header's vertex offset and `VertexBase` at it, then puts the header back. Every
consumer, and everything after them (depth records, sub-pixel, Z, the surface
buffer, the planar walk), sees one surface; a subdivided half's midpoints are made
from moved corners, so near and far tiles agree along each edge. Three waves at
unrelated headings, lengths L, 0.71L and 0.53L, periods as the root of the length.
**A vertex moves only if it is interior to the water**: not on an edge only one
water face has (the rim), and not where any non-water face of the area has a
vertex. Worked out per area from the map, the bank and the water's rects, hashed
once a walk. In `fdat02`: 3,714 of 5,174 water positions free, 1,460 on a rim, 363
shared; built in 9.7 ms; about 97 halves moved a frame. The map bank is
`0x8018E18C`; `0x8018E19C` holds it only while the tile walk runs (it is the
object bank at the walk's start, which is what the first build read: no water).
Skipped under `KF2_TILEWALK=verify`, `KF2_TILEWALK=0` and `KF2_PRIMBUF=1`.
`KF2_POLYASM=verify` with the swell on: 0 RAM, register and GTE mismatches in all
nine routines.

**The ripples** are `PrimFs`: a fragment of water's blend (semi-transparent, blend
0 or 3; `GlCore` sets `uWaveOn` per batch, never into a planar texture) whose texel
lies in a water rect is taken to world space -- view position from its depth, H
and the centre, then `view = R (world - cam) + T` inverted, with the camera the
port reads at the walk's start (the carried one, so the ripples do not swim with
the smoothing). Four directional waves give a slope; the slope times *Ripple
strength* is a push **in the world**, taken into texture space through the
polygon's own mapping (the inverse of the world position's screen derivatives,
then the UV's), so it agrees across tiles however each is turned; the pushed texel
wraps inside the rect, as the upload does, and the aniso taps with it. The slope
also lightens one side and darkens the other (*Ripple shading*). Faded out where a
pixel spans more than 3-9% of the longest ripple, before it can shimmer.

**One clock**, the world's: ticks plus `LogicPhase`, so both stop when the world
does (the map, the editor) and are smooth at any rate; *Wave speed* scales its
steps, not its value, so moving the slider does not jump the water.

Measured in `fdat02` at the New Game's view, 144 fps, 2140x1200, the world paused
by the editor (`waves` is a shell verb): off `5e48ed0a1016d4b4`, on
`0651d4f7072b8a47`, and **off again `5e48ed0a1016d4b4`**; on changes 21.5% of the
pixels, all in rows 694 and below (the water), largest step 61. Ripples alone
20.4%, the swell alone 21.5%. Two snaps paused are identical; unpaused, 22% of the
picture moves between two a second apart. 144.0 fps drawn at 20.0 ticks/s, about
340 rippled batches a frame. Off is compared inside one run, not against a build
without `0078`: with `uWaveOn` 0 the shader's texture path is the old one by
construction (`uv` is `vUV`, the wrap returns its argument, no multiply).

**What still needs an eye**: whether the swell reads as water or as the floor
moving (at tile-corner resolution a short swell is faceted, which is why the
length was first 12000, and the judged default is 6114); whether the rims, held still, look pinned; the ripples'
strength and shading defaults; and that no crack opens anywhere a water tile meets
something else.
