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

**The carry is at the submitters**, Verdite3's seam: a pre hook on each of the
five samples the record it is handed, and a post hook puts the tick's values back,
so what is carried is exactly what the walk draws and nothing is written outside a
submit. Keyed by table and slot, with an identity per table (an interaction
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
the negated yaw of the renderer's own camera copy (`0x80095756`) every frame: a
view-fixed model, likely the compass. It follows the carried camera with nothing
more; its own clip is not carried.
