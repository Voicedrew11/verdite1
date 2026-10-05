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
