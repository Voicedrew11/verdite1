# Smoothing: drawing between ticks

With frame pacing on (`docs/DEVELOPMENT.md`), the world ticks 20 times a second
and the renderer draws every frame. Without more, every frame between two ticks
draws the same picture. This document is what is carried between ticks, and how.
Verdite3's `docs/SMOOTHING.md` is the model.

**King's Field's renderer makes this smaller than Verdite3's.** Stage I,
`func_8001FDE4`, builds and draws the whole frame from two pointers, the main
loop's camera blocks, and the stages before it are the world. Verdite3 put its
stage 15 into C# to give it a view override; here the arguments are the override.

## The camera carried

`patches/ViewSmoothing.cs`, **on whenever pacing is** (`KF1_SMOOTH=0` compares).

The main loop hands the renderer `0x800650A0` (the camera's position, a VECTOR)
and `0x80065098` (its rotation, an SVECTOR: pitch, yaw, roll), which stage B fills
from the player each tick. Nothing but the main loop names those two addresses,
and the renderer's own copy of the camera (`0x80095744`, the position, and
`0x80095754`, the rotation, kept by the camera block `func_8001C184`) is read only
by the renderer and the walks under it. So on each main-loop call of the renderer
the hook samples the camera, writes the camera interpolated between the last two
ticks' at the pacing clock's fraction into the two blocks, and writes the tick's
camera back after the renderer returns. The modal loops call the renderer with
null arguments, which draws from its own copy: the last drawn camera.

- **Interpolated, never extrapolated**, so the picture trails the world by up to
  one tick (50 ms), as in Verdite2 and Verdite3.
- Angles take the short way round their 12-bit circle and stay in `0..0xFFF`: the
  map walk picks its visibility stencil by the yaw's high byte (the earlier
  `kf1-port` attempt's finding), and stage A keeps the yaw masked.
- **A step past 4000 units or `0x300` of angle in one tick snaps** (a warp, a
  load), and an overlay load re-primes.

Measured 2026-10-05, `KF1_FPS=144 KF1_SMOOTH_PROBE=1`, a New Game, holding Left
for 3 s then Up for 3 s: **144.0 frames a second, 144.0 with a new camera
turning and 142.0 walking**, 20.0 tick samples a second, 0 snaps; the drawn yaw
trails the handed one by 4 units mid-turn. **Not judged by eye.**

## Creatures, objects and their clips

`patches/ModelSmoothing.cs`, **on whenever pacing is** (`KF1_SMOOTH_MODELS=0`
compares).

The model walk `func_8001F218` (called by the renderer) draws five tables, each
through the visible-cell map at `*0x80095860`, handing each submitter its record
in `a0`:

| table | records | drawn when | position | angles | clip, time, pose | submitter |
|---|---|---|---|---|---|---|
| interaction objects `0x8006EDE0` | 190 x 44 | type `< 0x85` | `+8` | `+0x18`, `+0x1A` | | `func_8001EBB8` |
| objects `0x8006C4B8` | 128 x 72 | `+6 == 1` | `+0x1C` | `+0x2C`, `+0x2E` | `+0xA`, `+0x12`, `+0x34` | `func_8001E9A4` |
| a counted list `0x80095098` | `*(u16)0x80095090` x 24 | | `+4` | | | `func_8001ED90` |
| creatures `0x8009D040` | 48 x 60 | `+0`, `+3 != 0xFF` | `+0xC` | SVECTOR `+0x1C` | `+4`, `+8`, `+0x34` | `func_8001EEDC` |
| people `0x8009DB88` | 8 x 68 | `+0 == 1` | `+0x24` | SVECTOR `+0x34` | `+0xF`, `+0x12`, `+0x3C` | `func_8001F0C4` |

Positions are three `s32`, of which the submitters read the low halves. A
creature's model is `+3`, its scale three halfwords at `+0x24`; an object's
type is `+1`, and its animation model the low nibble of `0x8006BD99 + type*152`; a
person's model is `+2` plus `0xA`, its angles go through `RotMatrix`
(`func_8004E9B8`), and stage H (`func_8003596C`) runs the table. An object with
`+2 != 0` is drawn when within 12 cells of the player instead of by the map.

**The interaction objects move.** Stage F (`func_80031CC8`) runs each one's
state (`+0x28`, a jump table of 0x63 at `0x80012888`): a double door swings its
leaves' `+0x1A` by `0x20` a tick (state 0), a gate rises 60 a tick for 41 ticks
(state 2), and states `0x60`-`0x62` are **things that fall**, a pickup dropped
from a monster among them: `Y += velocity` (`+0x24`, growing by `0x14` a tick),
or 20 a tick spinning, until the floor (the cell's height in the map at
`0x80095900`), then tipping over on `+0x18`. Until 2026-10-05 this table was
taken for fixed and nothing in it was carried.

**The list is flipbook sprites**, 22 in area 1: the cel is `u16[+0] + u8[+0x14]`
into the model table `0x80055B00`, and the submitter steps `+0x14` on every draw,
wrapping at `+2 & 0xF` (`+2 >> 4` is a fixed turn). With pacing on they
animated at the drawn rate, seven times too fast at 144.

**The carry is at the submitters**, Verdite3's seam: before each of the five, the
record it is handed is sampled, and after it the tick's values are put back. Since
2026-10-14 the C# model walk (`patches/Draw/ModelWalk.cs`) makes both calls around
its submitters, and the submitter hooks stand in only when the walk is recompiled.
Either way, what is carried is exactly what the walk draws, and nothing is written
outside a submit. Keyed by table and slot, with an identity per table (an interaction
object's type, an object's type, a sprite's model and `+2`, a creature's model
and `+2`, a person's model and `+1`). Sampled on the first draw of a tick
(`FramePacing.Ticks` moved on a ticked iteration); a slot that missed a tick
(culled), or a new tenant, primes; **a step past 2000 units or `0x300` of angle
snaps**; a value moved outside a tick snaps; an overlay load forgets every slot.
Only the main loop's renderer call is carried; the modal loops draw the record as
it is. **Interpolated, never extrapolated.**

**The list's cel steps once a tick**: a draw of a sprite that is not its first in
the tick puts `+0x14` back after the submitter (whenever pacing is on, carried or
not). The submitter draws the cel it read before stepping, so a held draw shows
the tick's cel.

**The clip time is carried without writing it.** Every submitter that animates
poses with `func_800205D4(pose, model, clip, time, verts)` inside its submit, so
the pose hook carries the clip of the record whose submit is in progress, keyed
by its slot and sampled from the call's own arguments; a changed model or clip
primes. A model's clips are in its animation header (`*(0x80090FCC +
model*4)`): `header + *(header + 0x10)` is a table of offsets to clips, a clip a
`u16` count and offsets to segments, a segment a `u16` flag and a `u16`
duration. A model whose `*(header + 4)` is 0 has no clips, and the routine
returns before reading the table. Reading one anyway follows garbage offsets out
of RAM: casting a spell crashed with `unmapped address 0x8817E686` while the
patch read clips ahead of the draw for every live record, so every address it
follows is kept in RAM, and clips are read only for poses the game makes. The
routine finds the segment the time lies in and blends that segment's two keys
with `gteMIMefunc` at weight `((time - start) << 12) / duration`, or 4096 less
that when the flag is set (its one blend call, return address `0x8002092C`).
**Clips run to a length of 4096** (the four read: segments of 2048 x 2, 1024 x 4,
and uneven ones), and a clip's time steps about 51 a tick. So the pose call is
handed the whole tick the carried time lies in, and the blend the fraction's
share of that segment on top. The carried time is `t0 + (t1 - t0) * frac`,
unwrapped over the clip's length when it looped; a step of more than half the
clip, or a different clip, is drawn at the game's own time.

Measured 2026-10-05, `KF1_FPS=144 KF1_SMOOTH_MODELS_PROBE=1`, a New Game in area
1, the scene set by `poke`:

- **area 1's two animated objects** (type 8, clip 1): 252-288 poses carried a
  second, each blend weight moved, 0 snaps, as before the move to the seam. They are a trap: a
  `poke` to (51000, -11500, 19000) and Up for 300 ms kills a new character
  (HP 30 to 0) in most runs, with or without this patch;
- **a person** (record 2, near (17000, 124000)) idling: 144 poses carried a
  second (this table was not carried before);
- **the double door** at (45000-49000, 70350), opened with Circle: both leaves
  carried on every frame of the swing, 288 a second, 0 snaps;
- **a falling pickup**, a type-81 record copied into a free slot 6000 units up in
  state `0x61`: carried on 144 frames a second as it fell, 0 snaps;
- **the sprites**: 4 drawn a frame by (61000, 78000), 496 of 576 draws a second
  held, so each steps 20 times a second.

A monster's actual drop has not been seen (area 1's one creature, model 5, did
not react to Triangle), nor a walking creature. **Not judged by eye.**

### The arm

The first-person arm is `func_8001F798`, called by the renderer after the model
walk: model `0x14`, clip 0, posed into `0x800A07F4` at the `s16` time
`0x800A07F0`, which is -1 while the arm is not swinging. **Triangle swings it**
(Square does nothing in play, Cross opens the menu): the time runs from about
1200 to 3000. Its time is carried like a creature's, with the same pose hook, the arm's
routine standing in for a submitter. Measured at `KF1_FPS=144`: one swing draws
**86-87 carried poses**.

`func_8001F8B0`, called by the renderer earlier, poses model `0x15` (record at
`0x80055D74`: on at `+0`, clip `+1`, time `+2`, pose buffer `+0x18`) turned by
the negated yaw of the renderer's own camera copy (`0x80095756`) every frame: the
compass needle. It follows the carried camera with nothing more ("The HUD", below);
its own clip is not carried.

## Sprites and the eye

Read 2026-10-15 in `patches/Draw/SubmitSprite.cs`. **The flipbook sprites follow
the carried camera with nothing more.** The submitter `func_8001ED90` transforms a
sprite's position (`+4`, `+8`, `+0xC`, carried) by the view matrix `0x800956A0`
less the eye `0x80095744`, then turns the quad, which the writer `func_8001E230`
builds flat in its own plane (z 0), by one of two rotations, picked by the high
nibble of `+2`:

- **0**: the pitch-only matrix `0x800956C0`. The quad faces the picture plane,
  upright: a billboard.
- **otherwise**: the view matrix times a fixed turn of `(nibble - 0x10) << 6`
  (`func_80014BEC`, then `func_8004D11C`). The quad is turned once, with the
  world.

The three come from the camera block `func_8001C184`, run from the renderer's
copy of the camera, which `ViewSmoothing` hands it. They are written together, so
they agree on every drawn frame. No sprite reads the player's own state. Verdite3's
`BillboardFacing` fixed quads that went edge-on to a carried eye. Here a
billboard is built facing the picture, so this game has nothing like that to fix.
Whether a turned sprite (nonzero nibble) can go edge-on is a question about the game
itself, not about the carry.

**The cel stays held in `ModelSmoothing`.** The plan offered stepping it once a
tick inside the C# submitter instead. That would need the same first-draw-of-the-tick
record, would not cover the recompiled submitter (`KF1_SUBMIT_SPRITE=0`), and would
make the submitter's verify differ from the recompiled body on every held draw. The
hold puts `+0x14` back after the submitter, so all three modes read the same.
Measured 2026-10-15 at 144 fps: 129 of 144 draws a second held, so a sprite steps
on 15 draws, one per tick at that run's 15.1 Hz tick rate.

## The item display's turn

`patches/Draw/ItemDisplay.cs`, **on whenever pacing is** (`KF1_ITEMDISPLAY_CARRY=0`
compares; `KF1_ITEMDISPLAY_PROBE=1` counts).

The caption a pickup or an interaction shows is the item display, `func_8001FAFC`,
called by the renderer. It takes items from the 8-entry ring at `0x8009506E`, with
the index at `u8[0x80095086]` and `0xFF` meaning empty. It shows each one for 15
steps (state 2), then turns it out (state 3): the angle `u16[0x8009508A]` goes
0, `0x80`, `0x100`, `0x180`, and at `0x200` the records are cleared. The renderer's
HUD block reads the angle right after the call (`RotMatrix` of (angle, 0, 0),
`Renderer.HudModels`) and turns the display's models about X by it. Since
2026-10-14 the routine steps only on a tick's first frame (`KF1_ITEMDISPLAY_HOLD`,
"What moved with it" in `GPU_RENDERER.md`). So the turn drew four angles, each
held for a tick.

**The angle is carried.** On a frame where the routine steps, the patch keeps the
angle from before and after the step. On every frame it writes the angle
interpolated between the two at the tick's fraction, and a post hook on the
renderer writes the tick's angle back. The routine's own entry puts it back as well,
since the present's vblank can come before the post hook. Only the routine and the
renderer read the angle (grep of `generated/game.cs`), so the C# renderer and the
recompiled one both draw the carried angle. A step that goes down (a new item starts
at 0) or by more than `0x100` is not carried, and an overlay load forgets the pair.
**Interpolated, never extrapolated.** The last angle drawn is `0x180`, at fraction 1,
as on the console. The step to `0x200` clears the records.

Measured 2026-10-17, `KF1_FPS=144`, a New Game in area 1, item 5 poked into
the ring's slot (`poke 8009506E 05`), the world at 15 Hz: the countdown ran
`0F` to 0, then **39 frames carried with 39 distinct angles** over the four steps of
the turn. Without the carry, those frames draw 4. **Not judged by eye.**

## The HUD

Verdite3's `Stage15.CarryNeedle` and `CarryFollowers` were the models
(`docs/SMOOTHING.md` there, "3e"); only the second is needed here.

**The bars** (`patches/HudSmoothing.cs`, `KF1_SMOOTH_GAUGES=0` compares). The
renderer's gauge block builds the four bars' lengths every frame from HP
`0x800A0792`, MP `0x800A0796` and the two charge gauges `0x800A0798`/`079C`
("The renderer in C#" in `GAME_INTERNALS.md`), which the stages step once a tick,
so under pacing the bars moved 15 times a second. While the view is carried the four
are written between their last two tick samples for the block and put back right
after it; the maxima are not touched. A value that changes off the tick (a poke)
restarts its samples. It needs the renderer in C# (`KF1_RENDERER`), and stays off
under the renderer's `verify` replay.

**The needle is not a spring here**, unlike Verdite2's and Verdite3's: the block
next to the gauges writes it straight from the drawn camera's yaw
(`0x80055D86 = -yaw`), so it turns exactly as the carried view does, on every
frame, and has no position of its own to swing into. Nothing was held or carried
for it.

Measured 2026-10-17, `KF1_FPS=144`, the world at 15 Hz, a New Game in area 1,
`KF1_HUD_PROBE=1` (a line every 2 s), holding Left for 4.5 s then swinging:

- **the needle** moved on 287 of 288 frames while turning, a mean of 2.8-2.9
  angle units a frame (about 420 a second, the turn's 28 a tick), at most 6 in a
  frame; under the compass routines' `verify` a stalled window (281 frames) drew
  one step of 18. Standing, 0 of 288;
- **the bars**, refilling the charge after a swing: carried on 88-202 of 288
  frames, 0 at full charge.

**Not judged by eye**: the bars filling, and the needle turning.

## Loops that draw their own frames

Verdite3's `LoopPacing` runs a loop that calls the renderer once per world tick
and fills the frames between with redraws. Here, every loop that calls the
renderer but one waits the game's own frame gate after it ("What each loop waits
on" in `docs/GPU_RENDERER.md`). So these loops already step 20 times a second
under pacing, as on the console. Redraws between their steps would draw the same
picture again, because only the main loop's renderer call is carried. So they are
left at the gate's 20 frames a second. The two loops that waited for nothing, the
area-5 scene and the event picture, wait a vblank now ("Menus wait for a vblank"
in `docs/DEVELOPMENT.md`).

## The tints between ticks

**This game draws no full-screen tint**, so there is nothing to hold. Verdite2's
`TintHold` fixed a full-screen quad drawn only on tick frames, a strobe.
`KF1_TINTPROBE=1` (`patches/TintProbe.cs`, Verdite3's probe) looks for flat
semi-transparent primitives that span the draw area, and reports the largest one
covering half its width and height. Measured 2026-10-15 at `KF1_FPS=144`,
`KF1_TICKRATE=20`: two warps, so two area fades and loads; the area-1 trap at
(51000, -11500, 19000), HP 30 to 4 and then to 0; and three deaths and respawns.
**Not one tint and not one large primitive.** The probe does see the event
picture's 256x128 quad, so it works.

The fades go through the GTE's colour matrix instead (read in the code):

- **The area fade** `func_80036D3C` lerps the colour block it saved with
  `ReadColorMatrix` (`func_80020364`) and sets it with `SetColorMatrix`, 0 to
  `0x1000` and back by `0x40`, one step per renderer call, then the frame gate:
  130 steps at 20 a second, as on the console.
- **Death** (`func_80018540` in stage A) lerps the colour matrix and the fog's
  near distance (`SetFogNear`) once a tick.

Neither is stepped by the renderer, and nothing carries the colour matrix between
ticks, so a death's dimming steps 20 times a second while the eye's fall is
carried. Whether that shows is for a person to look at. The damage flash was not
found: at 30 to 4 HP no large primitive was drawn.

## Textures

**This game animates no texture or palette through VRAM.** Verdite3's
`TextureScroll` and Verdite2's `FluidSmoothing` step water by moving VRAM
rectangles. Reading for this game's image uploads found no caller of a
`LoadImage` or `MoveImage` that runs during play ("libgpu image routines" are not
bound), so it was measured. `KF1_VRAMCENSUS=<s>` (`patches/VramCensus.cs`) compares
the CPU's VRAM, which holds every upload, tick by tick in 64x16 blocks outside the
two display buffers. Measured 2026-10-15 at `KF1_FPS=144`, `KF1_TICKRATE=20`,
200 tick pairs standing in each of areas 1-5 after the warp: **no block changed in
any area**. The control, a window that spans the warp into area 4, catches the
load: one tick, blocks of 1,024 changed pixels from (320,0). A scroll written into
a mesh's UVs in RAM would not show here, so it was looked for on 2026-10-16: no byte
of the map, the models or the bank records changes over 6 s standing in any area
("Water: none to classify" in `GPU_RENDERER.md`).
