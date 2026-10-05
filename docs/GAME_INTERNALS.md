# Game internals

The reverse-engineered game: the main loop and its stages, player state,
movement, areas, saves and the boot stub — every address and routine this project
learns, written here rather than left in the commit that found it. Verdite3's
`docs/GAME_INTERNALS.md` is the model, but none of its addresses apply.

## Status

Read so far (2026-10-05): the boot, GAME.EXE's set-up and main loop, and its
frame gate.

## The boot

`PSX.EXE` runs `OPEN.EXE`, then `GAME.EXE`, then OPEN again, for ever (see "What
is on the disc" in `docs/RECOMPILATION.md`). OPEN plays the attract and the title;
three Starts, about three seconds apart, take it from the attract to a New Game
and hand over to GAME (the files OPEN reads on the way: `KF/B0/L0.`, `MIX0.`,
`MIXA0.`, `OPEN0.`, `MIXB0.` for the attract; `MIXA1.`, `OPEN1.` after the first
Start; `OPEN3.`, `MIX3.`, `MIXA3.`, `MIXB3.` after the second). GAME reads
`KF/COM` (`STAT.DAT`, `MIX.TIM`, `COM.DAT`), the three `KF/ITEM*` directories and
`KF/WEPON/WEP00.MIM`, then the area: `KF/B1/MIX.TIM`, `MIXA.DAT`, `SND0.SEQ` and,
last, `MIXB.DAT`.

## The player

Found 2026-10-05 by diffing RAM dumps (`dump` on the command channel) across a
walk and a turn, and by reading the code that uses each word:

| address | type | what |
|---|---|---|
| `0x800A0790` | u16 | max HP (30 at a New Game) |
| `0x800A0792` | u16 | HP: damage (`0x80016648`) subtracts from it, clamped at 0, and stage A calls the death routine `func_80015164` when it reads 0 (`0x80019A78`) |
| `0x800A0794` | u16 | max MP (20 at a New Game) |
| `0x800A0796` | u16 | MP |
| `0x800A078A` | u8 | the area, n for `KF/Bn`: the sequence loader `func_80032A4C` writes it plus `'0'` into `B?\SND?.SEQ` |
| `0x800A0824` | VECTOR | position x, y, z (s32); y is height, negative up (-11500 standing at the start) |
| `0x800A0838` | SVECTOR | rotation: pitch at `+0`, yaw at `+2` (`0x800A083A`), 0x1000 a turn |

A New Game starts at (31000, -11500, 4000), yaw 0, in area 1. Holding Up for a
second walks about 4000 units along the facing; holding Left for half a second
raised yaw by 280. The New Game set-up at `0x80015280` writes both halves of each
HP and MP pair, and copies `0x800650B4`, `0x800650B6` and `0x800650B8` into
`0x800A07A2`, `0x800A07A4` and `0x800A0784` (unidentified; `0x800A0784` reads
50).

## GAME.EXE's set-up and main loop

`func_800146B8` clears its tables, initialises the subsystems, opens the vblank
handler (below), calls `func_80014674(1)`, under which the area loads, and enters the loop at
`0x8001482C`:

| stage | routine | arguments |
|---|---|---|
| A | `func_80018880` | |
| (skip) | if `*0x800958F8` is set, the stages below are skipped | |
| B | `func_80017E3C` | `0x800650A0`, `0x80065098` |
| C | `func_8003303C` | the same |
| D | `func_8002CAD4` | the same |
| E-H | `func_80030818`, `func_80031CC8`, `func_8003A760`, `func_8003596C` | |
| I | `func_8001FDE4` | `0x800650A0`, `0x80065098` |
| gate | `func_800149F4` | |

then a check of the byte at `0x800A078D + 0xBD`. What each stage does is not read
yet; the earlier `kf1-port` attempt measured stage I as the renderer (every
projection and the one `DrawOTag`) and `0x800650A0`/`0x80065098` as the camera's
position and rotation, which is to be confirmed here.

## The frame gate

`func_800149F4` spins, with interrupts masked around each read, until the vblank
count `*0x80057B0C` is more than two past the count at the last frame
(`*0x80057B10`), or has wrapped below it, then stores it: **three vblanks a
frame, a 20 fps game.** The count is bumped by `func_800149D4`, a handler GAME
opens at `0x800147C8` with `OpenEvent(0xF2000003 (RCntCNT3), EvSpINT,
EvMdINTR, 0x800149D4)` and enables.

The gate never calls `VSync`, and on the fork's own vblank timeline (`0021`) the
vblank was delivered only from inside `VSync`, so after the area loaded GAME drew
four frames and spun in the gate for good (the managed stack: `func_800149F4`
under `func_80036618` under `func_80014674`). `Program.cs` sets
`LibEtc.VBlankFromPoll` (fork `0091`), and the interrupt poll the spin makes
delivers the vblanks that are due: about 20 frames a second in the first area.
