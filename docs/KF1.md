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
runs the first area at the game's own 20 fps. Perspective correction, sub-pixel
positions, the Z-buffer, ambient occlusion, true colour, anisotropic filtering,
widescreen and the audio quality settings all engage on KF1's geometry (measured,
below). **The picture has not been looked at by eye** — every number here is a
counter.

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
| `FramePacing` and the four smoothers, `LoopPacing`, `MenuPacing`, `LoadPacing`, `SpriteAnim`, `TintHold` | the main loop's stages and the frame gate (`func_800149F4`, above), the camera and the object tables |
| `PolyAssembler`, `TileWalk`, `ModelWalk`, `Stage13`, `CameraBlock` | KF1's renderer, from the map tiles to the model submitter |
| `PerPixelLighting`, `EvenFog`, `Reflections`, `Murk`, `Waves`, `PlanarWalk`, `Retained*`, `Remaster.*` | the C# assemblers above (they record what these read) |
| `CullCone`, `ViewClip`, `CullGrid`, `PrimBuffer` | KF1's cull and its primitive buffers |
| `Analog`, `Mouse`, `MenuMouse` | the player's movement and camera words, and the menu layout |
| `Map*`, `AutoReload`, `AutoStart`, `AgentBeacon`, `AgentServer`, `AreaWarp`, `HitGuard`, `MenuWorld`, `MessageText` | the game's state (area, HP, save slots, menus) |
| `PositionalAudio` | the game's 3D sound routine |
| `CardIcon` | reads KF2's icon out of `FDAT.T`; KF1's is probably `KF/TIM/ICO1.TIM` |
| `EndingHold`, `BootExe` | KF2's `END.EXE` and boot stub |

`mods/kf2debug` fails to compile at load (it calls KF2 routines the shim does not
carry); it is off unless enabled in the Mods panel.

## Driving it without a human

`KF2_AUTOPAD` works, with its clock starting when OPEN.EXE loads
(`KF2_AUTOPAD_FROM=game` for GAME.EXE). The title needs Start twice and then
Cross, and it ignores a press during its fades, so a schedule of repeated presses
is what gets through reliably:

```bash
KF2_AUTOPAD=8:Start:200,9.5:Start:200,11:Start:200,12.5:Start:200,14:Start:200,15.5:Start:200,17:Cross:200,18.5:Cross:200
```

That starts a New Game in `KF/B1` about 20 s after boot. With `KF2_LOG=sdk`, the
read of `MIXB.DAT` (`CdRead … lba=2923`) is the last thing the area load does.

The files OPEN reads say where it is: `KF/B0/L0.` then `MIX0.`/`MIXA0.`/`OPEN0.`/
`MIXB0.` for the attract; Start loads `MIXA1.` and `OPEN1.`; Start again loads
`OPEN3.`, `MIX3.`, `MIXA3.`, `MIXB3.`; Cross hands over to GAME.EXE.
