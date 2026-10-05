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
through the visible-cell map at `*0x80095860`:

| table | records | live | position | angles | clip, time | submitter |
|---|---|---|---|---|---|---|
| interaction objects `0x8006EDE0` | 190 x 44 | type `< 0x85` | `+8` (fixed) | | | `func_8001EBB8` |
| objects `0x8006C4B8` | 128 x 72 | `+6 == 1` | `+0x1C` | `+0x2C`, `+0x2E` | `+0xA`, `+0x12` | `func_8001E9A4` |
| a counted list `0x80095098` | `*(u16)0x80095090` x 24 | | `+4` | | | `func_8001ED90` |
| creatures `0x8009D040` | 48 x 60 | `+0`, `+3 != 0xFF` | `+0xC` | SVECTOR `+0x1C` | `+4`, `+8` | `func_8001EEDC` |
| `0x8009DB88` | 8 x 68 | `+0 == 1` | cell `+0x1C` | | | `func_8001F0C4` |

A creature's model is `+3`, its scale three halfwords at `+0x24`; an object's
type is `+1`, and its animation model the low nibble of `0x8006BD99 + type*152`.
The list's submitter writes a byte at `+0x14` every draw (unread). The fixed
interaction objects and the last table are not carried.

Around the renderer's main-loop call, each creature, object and list record live
with the same model at the last two ticks, and not moved more than 2000 units in
one, has its position and angles interpolated at the pacing clock's fraction and
the game's values written back after the renderer. The record must still hold
the tick's values when the frame is drawn, or it is left alone.

**The clip time is carried without writing it.** Both submitters pose with
`func_800205D4(record + 0x34, model, clip, time, verts)`. A model's clips are in
its animation header (`*(0x80090FCC + model*4)`): `header + *(header + 0x10)` is a
table of offsets to clips, a clip a `u16` count and offsets to segments, a
segment a `u16` flag and a `u16` duration. The routine finds the segment the time
lies in and blends that segment's two keys with `gteMIMefunc` at weight
`((time - start) << 12) / duration`, or 4096 less that when the flag is set (its
one blend call, return address `0x8002092C`). **Clips run to a length of 4096**
(the four read: segments of 2048 x 2, 1024 x 4, and uneven ones), and a clip's
time steps about 51 a tick. So the pose call is handed the whole tick the carried
time lies in, and the blend the fraction's share of that segment on top. The
carried time is `t0 + (t1 - t0) * frac`, unwrapped over the clip's length when it
looped; a step of more than half the clip, or a different clip, is drawn at the
game's own time.

Measured 2026-10-05, `KF1_FPS=144 KF1_SMOOTH_MODELS_PROBE=1`, standing by area 1's
two animated objects (type 8, clip 1): **253-274 poses carried a second, each
blend weight moved, 0 repeated and 0 backward poses between consecutive frames**,
and 79-203 moving records carried a second; 0 snaps. The two objects restart
their clip from time to time (1245 back to about 230 in a tick), drawn at the
game's own time. **Not measured on a creature that walks or animates**: the one
near the start is idle. **Not judged by eye.**

### The arm

The first-person arm is `func_8001F798`, called by the renderer after the model
walk: model `0x14`, clip 0, posed into `0x800A07F4` at the `s16` time
`0x800A07F0`, which is -1 while the arm is not swinging. **Triangle swings it**
(Square does nothing in play, Cross opens the menu): the time runs from about
1200 to 3000. Its time is carried like a creature's, with the same pose hook.
Measured at `KF1_FPS=144`: one swing draws **86 carried poses**, 0 backward.

`func_8001F8B0`, called by the renderer earlier, poses model `0x15` (record at
`0x80055D74`: on at `+0`, clip `+1`, time `+2`, pose buffer `+0x18`) turned by
the negated yaw of the renderer's own camera copy (`0x80095756`) every frame: a
view-fixed model, likely the compass. It follows the carried camera with nothing
more; its own clip is not carried.
