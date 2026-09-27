# King's Field (JP, SLPS-00017)

This repository began as a copy of the King's Field II port (verdite2) and now
builds **the first King's Field** — Japan only, From Software, 1994. This file is
the working log for that move: what the disc is, how every address the port binds
was found, what broke on the way to the first area and why, and which of the King's
Field II port's features are running here and which are still KF2-only.

Everything else under `docs/` was written against King's Field II. Its
*mechanisms* (the vertex map, the Z-buffer, the runtime patches) carry over; its
*addresses* do not, and every one of them names a routine in a different game.

**Status.** Boots, plays the attract sequence and the title, starts a New Game and
runs the first area. **It draws at 60 fps (any rate up to 240) with the world at
the game's own 20**, the view, the creatures and the objects carried between
ticks. Perspective correction, sub-pixel positions, the Z-buffer, ambient
occlusion, true colour, anisotropic filtering, widescreen and the audio quality
settings all engage on KF1's geometry (measured, below). **The picture has not
been looked at by eye** — every number here is a counter.

## The disc

| file | what |
|---|---|
| `PSX.EXE` | 2 KiB boot stub at `0x80010000`; the disc has **no `SYSTEM.CNF`**, so this is the BIOS default |
| `OPEN.EXE` | title, attract and the New Game / Continue menus, text at `0x80012000`, entry `0x8001AA7C` |
| `GAME.EXE` | the whole game, text at `0x80012000`, entry `0x8003AC5C` |
| `KF/B0` … `KF/B5` | one directory per area: `MIX.TIM` (textures), `MIXA.DAT`, `MIXB.DAT`, `SND*.SEQ`; `B0` holds OPEN's own scenes |
| `KF/COM` | `COM.DAT`, `MIX.TIM`, `STAT.DAT` — loaded once, at GAME.EXE's start |
| `KF/ENE*`, `KF/ITEM*`, `KF/WEPON` | enemy TIMs, item TMDs, weapon MIMs |
| `KF/TALK`, `KF/KAN`, `KF/PRSN`, `KF/MAP`, `KF/TIM` | dialogue, sign and portrait TIMs; `KF/TIM/ICO1-3.TIM` look like the memory-card icon |
| `E0.`-`E3.`, `KF/B0/END.`, `ENDG.` | ending data |

**There is no `END.EXE` and there are no code modules.** King's Field II loads
per-area MIPS off `FDAT.T` at run time; this game keeps all of its logic in
`GAME.EXE`, so there are two overlays and no runtime-loaded code at all.

`PSX.EXE` holds a pointer pair at `0x80010224` — `0x80010014` (`cdrom:OPEN.EXE;1`)
and `0x80010000` (`cdrom:GAME.EXE;1`) — and loops: `Load` and `Exec` (as a call)
OPEN, then GAME, then OPEN again. Before that it reads `0x1F802040` (a dev-kit
DIP switch) to pick a stack size; every entry of its table is 8 MB, so the value
does not matter. The PS-X EXE headers' stack fields hold garbage (`"fake"`,
`"94C "`); the code sets its own stack.

## The recompiler config

`config/kf1.json`, function maps under `config/funcmaps/kf1/`. **The KF2 config
(`config/kf2.json`, `config/funcmaps/*.json`) is kept as reference** and nothing
reads it.

- **No `SYSTEM.CNF`.** RecompOne's `SystemCfg.Parse` read it unconditionally; it
  now falls back to the BIOS's own defaults (`PSX.EXE`, TCB 4, EVENT 16, stack
  `0x801FFF00`) when the file is absent (`0082`).
- **Function maps** are upstream's `--autoconfigure -sweep-all`, with PSY-Q names
  from the signature bank: 235 of GAME.EXE's 935 functions and 234 of OPEN.EXE's
  664 are named. As in the KF2 port, **every name `SdkPatches` would bind is put
  back to `func_`** (28 per overlay) so binding happens only through
  `patches[]`, by address. Duplicate names get the address appended
  (`RotMatrixC_8004B9B0`). The recompiled result: 1,639 functions, 42 patches
  applied, 0 reimplementations.
- **The game's name is `KingsField`**, so the generated classes are
  `Recompiled.KingsField_open`, `KingsField_game` and `KingsField_main`.

### The SDK entry points

The signature bank found libcd, libcdstream, and `DrawOTag`, `DrawSync` and
`PutDrawEnv` in both executables. **It holds none of the 1994 library's
`VSync`, `PutDispEnv` or `DMACallback`**, so those three were found by hand:

| entry | open | game | how |
|---|---|---|---|
| `VSync` | `0x800352C4` | `0x800555E0` | the 1994 `vsync.c` (its `$Id` string is `1.11 1994/10/15`): `VSync(n)` loops `n<<8` hblanks through an inner wait at `+0x64` that reads GPUSTAT through the pointer at `0x80057E54` and opens root counter 1's event (`0xF2000001`) the first time, where KF2's reads `RCNT1` directly. 9 `jal` sites in GAME |
| `PutDispEnv` | `0x80030B4C` | `0x80050E68` | `PutDrawEnv` + `0xC0` is `GetDrawEnv` (signature-named) in both games, and `PutDispEnv` follows it, ending exactly where the named `GetDispEnv` begins |
| `DMACallback` | `0x8002FA00` | `0x8004FC2C` | the one routine the named `CdDataCallback` family all call, with `a0` the channel; it indexes a 7-slot table at `0x800642D0` and sets DICR through the pointer at `0x80057D1C`. The 1994 one returns nothing |
| `InterruptCallback` | `0x8002FA80` | `0x8004FCAC` | not bound; it names the IRQ callback table (below). Refuses IRQ 3, sets `1<<irq` in I_MASK |

The addresses the patches hook are in one table, `patches/SdkAddr.cs`; the KF2
port had fourteen private copies of its `DrawOTag`.

**PSY-Q's interrupt-callback table** (patch `0006`) is `0x800642F0` in GAME and
`0x800423C0` in OPEN — the 11 slots `InterruptCallback` indexes and
`ResetCallback` (`0x80050000`) clears. The DMA callback table sits 0x20 bytes
*below* it in this library, where KF2's sat above. IRQ 0 and IRQ 3 report
"dropped: no handler": this library does not put its vblank or DMA dispatch in
those slots, so that is expected.

## What broke on the way in

### The vblank came only from VSync, and this game waits without calling it

After GAME.EXE loaded the first area it drew four frames and stopped. The managed
stack named the loop: `func_800149F4` is **the game's frame gate** —

```
loop: EnterCriticalSection
      cur  = *0x80057B0C          ; bumped by the vblank handler 0x800149D4
      last = *0x80057B10
      if (cur > last + 2 || cur < last) { *0x80057B10 = cur; ExitCriticalSection; return }
      ExitCriticalSection
```

— a spin until three vblanks have passed since the last frame, which makes
King's Field a **20 fps** game. The counter is bumped by an event handler GAME
opens on `RCntCNT3` (`OpenEvent F2000003, EvMdINTR, func 0x800149D4`), and the
port's vblank (`0021`'s wall-clock grid) was advanced **only from inside
`VSync`**. A loop that waits for a vblank without calling `VSync` waits forever.
That is the shape `docs/RUNTIME.md` warns about ("Two general shapes worth
keeping"), met from the other side: not something refreshed at `VSync` going
stale, but time itself only moving there.

The grid is time, so the interrupt poll now advances it too (`0081`):
`Interrupts.PollSlow` calls `LibEtc.PollVBlanks`, which delivers each vblank that
is due once, whoever asks first, and does not re-enter a delivery already under way
(a handler is recompiled code and polls too). Measured: 455 frames in the 15 s
after the area load, where there had been 4.

### Every vblank event fired twice

With the gate open the game drew 30 frames a second, where three vblanks a frame
is 20. `LibEtc.TickVBlank` delivered the `F2000003` event itself **and** raised
IRQ 0, whose service routine (`BiosB.DeliverIrqEvents`, upstream's since the
merge) delivers the same event: every `EvMdINTR` handler on the vblank ran twice a
vblank. KF2 opens no such handler, so it never showed there. In KF1 **both** of
GAME's vblank handlers ran double — the frame counter above, and the sound
sequencer tick below — so the game ran at 30 and the music tick at 120 Hz. The
direct delivery is gone (`0081`). Measured after: 20 frames/s in the area, and 774
sound ticks in about 13 s (60/s).

### setjmp and longjmp are a stack switch, and they happen to work

The runtime's BIOS `longjmp` (A(14h)) restores the registers and returns; it
cannot transfer control, since recompiled code returns through C# frames. King's
Field calls it 120 times a second, so this looked like the first suspect. It is
not one: the only user is `func_8004A55C` (OPEN's is `0x8002A330`), the vblank
handler that runs the sound sequencer, and it uses the pair only to **run on a
private stack**:

```
if (setjmp(jb) == 0) { jb.sp = 0x80063278; longjmp(jb, 1); }   ; now on the private stack
... the sequencer tick ...
jb.sp = saved; longjmp(jb, 2)                                  ; back, returns
```

Each `longjmp` is followed, in the recompiled function, by exactly the code the
`setjmp` return would have branched to — the first falls into the tick, the second
into the epilogue — with `SP` restored by the register copy. So the switch is
correct by the accident of the layout. **A longjmp anywhere else in this game would
not be**; there are none (two `jal` sites to the thunk in each executable, both
here).

## The main loop

`func_800146B8` sets the game up and runs the loop at `0x8001482C`:

| stage | routine | what (measured with `KF2_STAGE_PROBE=1`, standing in `KF/B1`) |
|---|---|---|
| A | `func_80018880` | 0.16 ms; the player and the pad (reads the rotation at `0x800A083C`) |
| B | `func_80017E3C` | copies the player's position (`0x800A0824`, VECTOR) and rotation (`0x800A0838`, SVECTOR) into the camera blocks `0x800650A0`/`0x80065098` |
| C, D | `func_8003303C`, `func_8002CAD4` | copy the camera into two modules' own copies (`0x8009587C`, `0x8006E8B8`) |
| E-H | `func_80030818`, `func_80031CC8`, `func_8003A760`, `func_8003596C` | logic; none projects a vertex or draws |
| I | `func_8001FDE4` | **the renderer**: every projection in the frame and the one `DrawOTag`, 1.5 ms |
| gate | `func_800149F4` | spins about 48 ms until the vblank counter is three past the last frame's |

then a check of the player's cell for an area change (`0x800A078D` + `0xBD`/`0xBE`).
**The renderer is one function, called with the camera as arguments**, which is
what made frame pacing here much smaller than King's Field II's. It is also
called by eight modal loops (doors, menus, the area load) with `a0 = a1 = 0`,
which draws from the camera it last stored, followed by the same gate.

Inside it, in order: `func_8001C184` (the camera block, below), `func_8001BFB8`
(clear the ordering table), `func_8001E83C` (**the map walk**, 1,169 projections a
frame at the start), `func_8001F218` (**the model walk**, below), `func_8001E230`
(three small 3D pieces, 5 projections), `func_8001C050` (**the flip**:
`DrawOTag`, `PutDrawEnv`, `PutDispEnv` and a `VSync`, so every picture presents
through `VSync`).

**The camera block** `func_8001C184` copies a non-null position into its store at
`0x80095744` (x, y, z) and the map cell it lies in (x/2000, z/2000) into
`0x8009575C`/`5E`, a non-null rotation into `0x80095754`, and builds its matrices
(`0x800956A0`, and a pitch-only one at `0x800956C0`) from the store. Flipping
either of the first two rotation halfwords by half a turn for the length of the
renderer call took its projections from 27,161 to 2,740 and 2,540 a second: the
renderer builds its view from that store and nothing else.

**The model walk** `func_8001F218` runs five tables through the visible-cell map at
`*(0x80095860)` (width, height, origin, then a byte per cell):

| base | entries × stride | live when | position | submitter |
|---|---|---|---|---|
| `0x8006EDE0` | 190 × 44 | `+0 < 0x85` | cell at `+2`/`+4` | `func_8001EBB8` |
| `0x8006C4B8` | 128 × 72 (objects) | `+6 == 1` | int32 at `+0x1C`, rotation at `+0x2C` | `func_8001E9A4` |
| `0x8009505A` | `*0x80095090` × 24 | — | int32 at `+0x42`, `+0x4A` | `func_8001ED90` |
| `0x8009D040` | 48 × 60 (creatures) | `+0 != 0xFF`, `+3 != 0xFF` | int32 at `+0x0C`, rotation at `+0x1C`, scale at `+0x24` | `func_8001EEDC` |
| `0x8009DB88` | 8 × 68 | `+0 == 1` | cell at `+0x1C`/`+0x1E` | `func_8001F0C4` |

The submitters take the position relative to the camera store, so a model's
matrix is built from those fields at draw time.

**The map walk** `func_8001E83C` draws the cells of a 14×14 stencil around the
camera, and **picks the stencil by heading**: `0x80065BE8 + (15 − (s8)(yaw >> 8)) ×
0xCC`, one of 16, from the high byte of the stored yaw (`0x80095757`); a pitch
beyond ±511 takes a special one at `0x80055E9C`. Stage A keeps the yaw in 0..4095
(`andi 0xFFF` at `0x80018F94`), and anything writing a yaw the renderer will read
must too: an interpolated 4101 across the wrap indexes past the table. `ViewCarry`,
`ObjectCarry` and `MouseLook` all mask it. **The stencils are generous**
(dumped with `KF2_AGENT_DUMP`): 14 cells (28,000 units) wide, full width from
three cells ahead, and skewed to cover the whole 22.5° sector a heading stands
for, so a 16:9 view (±46.8° against 4:3's ±38.7°, at `H` = 200) stays inside them
except possibly right beside the camera. Nothing widens them; if widescreen shows
missing geometry at the sides, they are the first place to look.

## Frame pacing

King's Field II's frame pacing had to put the world on a tick clock of the port's
own and gate every stage that held per-tick state, because its gate and its world
were tangled together. **King's Field's are not**: the world is stages A–H, the
picture is stage I, and the gate is a wait at the end. So `patches/GateRedraw.cs`
**replaces the gate with the same wait, and redraws while it waits**: it polls the
interrupts (so the vblank handler keeps counting), returns when the counter is
three past the last frame's, exactly as the game would, and in between calls the
renderer again whenever a frame is due and it would land clear of the next tick.
**The logic never runs at any rate but its own**, so there is no tick clock to
keep in step, nothing to gate, and no rate at which the game speeds up.

Only the main loop's gate redraws (return address `0x800148D0`), and only after
the renderer has run since the last gate. The modal loops keep the original
wait; they draw at 20.

Measured (`KF2_FPS_PROBE=1`):

```
KF2_FPS=60   60.0 drawn (40.0 redraws), 20.0 ticks/s, 45.7 ms spun per tick, presents 60
KF2_FPS=144  140.0 drawn (120.0 redraws), 20.0 ticks/s, 41.0 ms spun per tick
KF2_FPS=20   20 frames/s, the game untouched
```

144 draws 140 because a redraw is not placed within half a frame of the next tick:
seven fit in a tick. The picture presents through the renderer's own `VSync` (in
its flip), 60 a second at 60 fps; the host ceiling is set to twice the target so
it never undercuts it. **60 is the default**; Video ▸ Frame pacing has the rate
slider and the smoothing tick (`patches/settings/FrameRatePage.cs`).

**What a redraw changes.** `KF2_FPS_PROBE=2` diffs guest RAM across 200 redraws:
93 words in 40 runs. The double-buffer state (`0x80070E98`, `0x80090EBC`, written
by the frame-begin and flip routines) moves every picture, as it must; the vblank
counter, the sound tick's jmp_buf, private stack and save area, and the exception
stack move because interrupts are delivered during the redraw's `VSync`. Three
words (`0x8009522C`, `0x800597D4`, `0x800C2610`) move with no CPU store at all
(`KF2_RAM_WATCH` saw none), so they are written by a DMA or an HLE path, not by
the renderer. **The renderer steps no game state of its own**, which is the
condition for redrawing it being safe.

### The view is carried between ticks

`patches/ViewCarry.cs`, around every main-loop render and every redraw: it writes
`lerp(prev, cur, phase)` into the camera block's store, calls the renderer with
null arguments so it draws from the store, and puts back afterwards what the
game's own call would have left there (and the cell the camera block would have
derived). `prev` and `cur` are the camera at the last two ticks, sampled on each
tick's first picture; `phase` is the time since that picture over 50 ms. It
interpolates rather than extrapolates, for King's Field II's reason (a damped turn
overshoots), so the picture trails the game by one tick. The angles take the
shortest way round their 12-bit circle; a step of more than 4000 units in a tick,
or a gap of more than 250 ms since the last main-loop picture, starts again from
the new camera. Measured walking: `60/60 renders carried, mean phase 0.33, move
181 u/tick`.

### Creatures and objects

`patches/ObjectCarry.cs` does the same for the two tables that move in world
units, the creatures and the objects, around the model walk: a slot live and of
the same model at both ticks, and not moved further than 2000 units, is drawn at
`lerp(prev, cur, phase)` for position and rotation and put back afterwards.
Measured at the start of `KF/B1`: 3-4 live slots, 1.2-3.0 of them carried a
picture. **Their animation (the model's pose) is still stepped at 20**; the
creature entry's `+4` names the animated path, which is where that would start.

### VSync outside the renderer blocks

The port's VSync returns at once (`0021`'s non-blocking timeline). That is right
for the renderer's own `VSync` in the main loop, since the gate paces those
pictures, and wrong for every other loop that times itself by `VSync` — and GAME
has several: a second flip routine used by six menu-like callers
(`func_8002AC34`: `DrawSync`, `VSync(0)`), a 75-vblank wait (`func_80032B5C`), a
`0x4B00`-iteration poll (`func_80032BDC`), and `CdInitFileSystem`'s four. Those
ran at the host ceiling. `GateRedraw.BeforeVSync` now makes a `VSync(0)` or
`VSync(n)` outside a paced render wait until the vblank count is 1 or n past the
last `VSync`'s, delivering interrupts while it waits — the hardware's behaviour.
The renderer's own call inside the main loop or a redraw still returns at once
(`ViewCarry.InPacedRender`). Measured: the title still reaches the game, and play
is unchanged (`60.0 drawn, 20.0 ticks/s, 0.0 blocking VSyncs/s`).
`KF2_VSYNC_OUTSIDE=free` is the comparison. **Whether a menu now runs at the
speed it did on the console has not been checked by eye.**

## Mouse look

`patches/MouseLook.cs`. The player's rotation is an SVECTOR at `0x800A0838`:
**pitch at `+0`**, which stage A holds inside ±`0xBF` (about 17° either side of
level; its look velocity is `0x800A0848`), and **yaw at `+2`**, 12 bits to the
circle, read signed by the walking code. Escape captures the pointer; the motion
since the last tick is added to yaw and pitch at the end of stage A (pitch
clamped where the game clamps it), so it lands before stage B copies the player
into the camera and before the next tick walks, as the D-pad's turn does. The
view does not wait for it: `ViewCarry` shows the mouse's share of the last tick's
turn whole instead of interpolating it, and adds the motion not yet spent — King's
Field II's "The mouse leads the tick" (`docs/INPUT.md`). The three buttons press
Square, Triangle and Cross while the pointer is captured; **which of those King's
Field uses to attack has not been checked**, so `KF2_MOUSE_BUTTONS` may need
reordering. 0.15° a pixel at sensitivity 1, as KF2's.

**Twin-stick** (`patches/TwinStick.cs`): on a gamepad the runtime binds the left
stick onto the D-pad, a tank control. The left stick's X now presses L1/R1 (the
strafe) instead of Left/Right unless the D-pad itself is held, and the right stick
turns and looks — a squared response, full deflection about 1.5 times the D-pad's
turn — spent at the end of stage A with the mouse, but into the tick and
interpolated like the D-pad's turn rather than led. `KF2_ANALOG=0` restores the
runtime's mapping. **Untested with a pad**: without one it is inert, measured.

Measured with a synthetic source (`KF2_MOUSE_SYNTH=30`, 30 px right a tick): the
carried yaw steps 51 units a tick (30 × 0.15° × 4096/360 = 51.2), so the game
takes the written yaw and the camera turns with it. **Capture, the real mouse and
the feel are the user's to judge.** There is no settings page for it yet.

## Scripted input reaches the game now

`KF2_AUTOPAD` wrote `Controller.State`, and `BiosB.PadRead` (`B(16)`, how this
game reads the pad) polls the host input first — which rewrote `State` from the
keyboard before the game read it. In the title, where the game reads the pad many
times a second, a press sometimes landed; in play, where stage A reads it once a
tick, the player never moved. The script holds its buttons through
`Controller.ScriptMask`, which the input poll ANDs into the keyboard's state
(`0085`).

## What the enhancements measure

In the first area, standing still at New Game (`KF2_*_PROBE=1`):

```
perspective: 27160 vertices projected/s, 54280 caught/s, 34180 copied/s, 96.2% hit     (20 frames/s)
subpixel:    40740 vertices/s carrying a fraction, mean offset 0.770 px
zbuffer:     25350 tris/s tested, 750 painter's/s, 97.1% of submitted
ao:          25350 tris/s kept for normals; 12.7% of the picture shaded, darkest 0.73
widescreen:  23.7% of 26880 prims reach the margin
```

**The vertex map follows King's Field's geometry as well as KF2's** (KF2 measured
91.6-94.7%): the mechanism keys on the addresses the game's own `lw`/`sw` move a
GTE result through, and nothing in it is specific to one game. The Z-buffer and
the occlusion pass **take their depth from the address map** here
(`KF2_ZBUFFER_SOURCE` now defaults to it): KF2's default was the packet records its
C# polygon assemblers write, and KF1 has none. **A quarter of what the game draws
crosses the screen edge**, as in KF2, so the widescreen margin has something to
show; whether KF1 culls objects against a 4:3 cone as KF2 did is unknown.

## What came across from King's Field II

**Every patch is still compiled; only the ones that do not depend on KF2's own
routines are installed** (`Program.cs`; KF2's is `reference/kf2/Program.cs`). The
KF2 patches that call a KF2 routine directly by its generated name compile against
`patches/kf2/KingsField2Game.cs`, a table of KF2 addresses dispatched — nothing
installed reaches it, since KF2's GAME.EXE is never loaded.

Installed, with the settings page for each: `Perspective`, `Subpixel`, `ZBuffer`
(its arm and model-submit hooks are KF2 routines and are off: `ArmDraw` and
`ModelSubmit` are 0), `AmbientOcclusion`, `NoDither`/`TrueColor` (Shading),
`Anisotropic`, `Widescreen` (without `CullCone` and `ViewClip`, which rewrite KF2's
cull tables), `Pgxp` (env only), `AudioQuality`, `AudioProbe`, `UiScale`,
`KeyLayout`, `Prejit`, the VRAM snapshot (`0039`) and the settings window.

Not installed — each needs King's Field's own routine found first:

| feature | what it needs in KF1 |
|---|---|
| `FramePacing`, `FrameSmoothing`, `ObjectSmoothing` | **done differently**: `GateRedraw`, `ViewCarry`, `ObjectCarry` (above) |
| `AnimSmoothing`, `FluidSmoothing`, `LoopPacing`, `MenuPacing`, `LoadPacing`, `SpriteAnim`, `TintHold` | the creatures' pose, the scrolling textures, and the modal loops, which keep the game's 20 |
| `PolyAssembler`, `TileWalk`, `ModelWalk`, `Stage13`, `CameraBlock` | KF1's renderer, from the map tiles to the model submitter |
| `PerPixelLighting`, `EvenFog`, `Reflections`, `Murk`, `Waves`, `PlanarWalk`, `Retained*`, `Remaster.*` | the C# assemblers above (they record what these read) |
| `CullCone`, `ViewClip`, `CullGrid`, `PrimBuffer` | KF1's cull and its primitive buffers |
| `Mouse` | **done differently**: `MouseLook` (above) |
| `Analog` | **done differently**: `TwinStick` (above) |
| `AutoStart` | **done differently**: `KF2_AUTOSTART=new` in `Program.cs` |
| `MenuMouse` | the menu layout |
| `AgentBeacon` | **done differently**: `Kf1Beacon` — overlay, position, rotation, and the two HP/MP pairs at `0x800A0790`/`0x800A0794` (30/30 and 20/20 at a New Game; the renderer's HUD code reads them to size its gauges, so they are the stats; which of each pair is current is not confirmed) |
| `Map*`, `AutoReload`, `AgentServer`, `AreaWarp`, `HitGuard`, `MenuWorld`, `MessageText` | the game's state (area, death, save slots, menus) |
| `PositionalAudio` | the game's 3D sound routine |
| `CardIcon` | **installed**: KF1's card icon is `KF/TIM/ICO1-3.TIM`, three plain 16×16 4-bit TIMs (a knight swinging a sword), read directly; KF2's path stays for KF2's disc |
| `EndingHold`, `BootExe` | KF2's `END.EXE` and boot stub |

`mods/kf2debug` fails to compile at load (it calls KF2 routines the shim does not
carry); it is off unless enabled in the Mods panel.

## The shipped launcher

`Verdite2.Launcher/` builds the game at first run from the player's disc, as it
did for King's Field II, and now does it for this one: `DiscCheck` accepts a disc
with **no** `SYSTEM.CNF` and `PSX.EXE`, `OPEN.EXE`, `GAME.EXE` and
`KF/COM/COM.DAT` on it, and names the North American "King's Field"
(`SLUS-00158`) as King's Field II, verdite2's game, when it is offered;
`BuildKey` hashes the three executables; `Recompile` stages and rewrites
`kf1.json`; the data directory is `verdite1` (`VERDITE1_DATA`), so an installed
verdite2's build cache and cards are not shared. Measured: from an empty data
directory, the recompile (1,639 functions) and the compile run and the game plays.
**The project, the executable, the packaging and the icons are still named
Verdite2** (`Verdite2.Launcher`, `packaging/*/verdite2.*`); renaming them is a
separate change that touches CI and both packaging scripts.

## Driving it without a human

**`KF2_AUTOSTART=new`** presses Start every 1.5 s from 6 s after OPEN.EXE loads
until GAME.EXE does, then stops: Start twice takes the title to a New Game in
`KF/B1` (about 18 s after boot). It has to stop, because a Start or a Cross that
lands in the area opens the in-game menu, which is a modal 2D loop (no
projections, DrawOTag at 60/s). **`KF2_AUTOPAD` with `KF2_AUTOPAD_FROM=game`** then
scripts the area on a clock that starts at GAME.EXE's load:

```bash
KF2_AUTOSTART=new KF2_AUTOPAD_FROM=game KF2_AUTOPAD=6:Left:2000,9:L1:1500 KF2_AGENT=1 \
  dotnet run --project KingsField2Recomp.csproj -- "disc/King's Field (Japan).cue"
```

With `KF2_LOG=sdk`, the read of `MIXB.DAT` (`CdRead … lba=2923`) is the last thing
the area load does. Both scripts hold their buttons through `Controller.ScriptMask`
(see below).

The files OPEN reads say where it is: `KF/B0/L0.` then `MIX0.`/`MIXA0.`/`OPEN0.`/
`MIXB0.` for the attract; Start loads `MIXA1.` and `OPEN1.`; Start again loads
`OPEN3.`, `MIX3.`, `MIXA3.`, `MIXB3.`; Cross hands over to GAME.EXE.
