# Game internals

The reverse-engineered game: the main loop and its stages, player state,
movement, areas, saves and the boot stub — every address and routine this project
learns, written here rather than left in the commit that found it. Verdite3's
`docs/GAME_INTERNALS.md` is the model, but none of its addresses apply.

## Status

Read so far (2026-10-05): the boot, GAME.EXE's set-up and main loop, its
frame gate, and where the HUD's and menus' palettes are loaded.

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

## Saves, the menu and the title

**Saving is at save points.** Stage A examines on Circle (`0x20` in its pad word,
edge-triggered against the last frame's at `0x80057B30`) by calling
`func_80034DE4(&pos, &rot)`, which finds the interaction object in the cell ahead
of the player and dispatches on its kind through the jump table at `0x80012A7C`
(84 kinds). The objects are the 44-byte records at `0x8006EDE0` (type at `+0`,
cell x/z at `+2`/`+4`, position at `+8`/`+0xC`/`+0x10`); a type indexes 8-byte
records at `0x8006E8E0` whose first byte is the kind. **Kind `0xE` is a save
point** (type `0x74` in area 1, three of them, one at (52850, -12000, 75000)):
its case calls `func_800222B4`, the save screen `func_800250C4`, and through
`func_8002B648` the writer `func_8002B73C`. Circle, then Circle again, writes the
save to card A slot 1 through BIOS `open`/`lseek`/`write`/`close` (B(32h), B(33h),
B(35h), B(36h)).

The save is `BISLPS-00017KF`, **5 blocks** (40960 bytes), its title block
`SC`, icon flag `0x13` (three frames), title `<<  KING'S FIELD  >>` in full-width
Shift-JIS. `func_8002B73C` also writes `BISLPS-00017KFTMP` (the strings are at
`0x80056034` and `0x80056050`); GAME's set-up calls `func_8002C70C`, which checks
the card with `_card_info` (A(ABh)) and creates and erases `KFTMP` to see that it
can write, and shows a card message (`func_8001B7B0(2)`) when it cannot.

**Loading is from the in-game menu.** Cross (`0x40`) opens it
(`func_80036E38` from stage A, then `func_80022348`), seven items dispatched
through the table at `0x800122C8`; **item 5 is Load** (`func_80024E64`, the load
screen `func_8002552C`, the reader `func_8002BDE4`). Measured: from a New Game,
Cross, Down five times, Circle, Circle, Right, Circle loads card A slot 1, and
the player stands where the save was made.

**OPEN.EXE's title has one way into GAME**: a Down before the Starts reads the
same files and starts the same New Game, and OPEN.EXE has no card code at all.
GAME hands one value back to OPEN when its main loop ends: it stores the exit
flag `*0x800958F8` into `argv[1]`, which the boot stub passes to the next
`OPEN.EXE` as `argv[0]`, and OPEN's main hands it to `func_800156BC`.

## Changing areas

After the frame gate the main loop reads the player's cell, z at `0x800A084A`
and x at `0x800A084B` (each `/2000` of the position), and the byte of the area's
cell grid at `0x8009A748 + z*100 + x`. **A cell reading `0x40` is an exit**: on
entering one (the last cell, `0x800A084C`/`4D`, differs) it calls
`func_80036AF0`, which runs the transition (`func_80036850`, `func_80036618`) and
loads the next area inside GAME.EXE; a non-zero return instead sets the exit flag
to `0xFE` and leaves the main loop. Area 1 has four exit cells (z, x): (2, 15),
where a New Game starts, (11, 25), (35, 39) and (56, 29). Measured: standing on
(11, 25) loads `KF/B3` (area 3) and keeps x and z, so the levels share one
coordinate frame.

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

## The HUD and the menus

The HUD and the menus are 2D: flat textured quads (`POLY_FT4`, op `0x2C`, `0x2E`
semi-transparent) with no GTE depth, sorted into the same ordering table as the
world. Counted in area 1 (2026-10-05): about 71 a frame in play beside 368 of the
world's gouraud quads (`0x3C`), and in the in-game menu 116 plus 12 blended,
nothing else, which clear the screen (`isbg` 1) and draw at 60 a second.

**Their palettes are 4-bit CLUTs that GAME.EXE loads as 16x16 blocks at rows 497,
498, 499 and 500** (`MIX.TIM`'s CLUT headers, `(0,497)` and so on), each running
past VRAM's bottom edge and wrapping to row 0 on the console. The menu draws from
the page at `(768,256)` through `(0,498)`, the HUD's panels from `(896,256)`
through `(0,500)`. The fork's GL backend dropped any load that crossed the edge
whole, so those palettes read zero, which is transparent, and **the HUD and every
menu were drawn and invisible** until fork `0092` (2026-10-05). Measured after:
GL's VRAM equals the CPU's everywhere outside the two display buffers, through
boot, area 1 and the menu.

## What runs at the render rate

With pacing on, stage I runs on every frame and stages A-H only on a tick, so
anything the renderer advances itself would run at the drawn rate.
`KF1_RATECENSUS=12` at `KF1_FPS=144`, standing at a New Game in area 1 (229 pairs
of idle frames, 2026-10-05), finds:

- the frame's own working memory: the double-buffer index `0x80070E98`, the two
  ordering tables and packet buffers (`0x80070EA8`-`0x80076BB8`,
  `0x80080EC0`-`0x80086BB8`, `0x80090EBC`), the projected-vertex cache
  `0x800911B0`-`0x800912A0` (`func_8001C60C` fills it each frame from the vertex
  list `0x800910BC`), and the stack;
- the vblank count `0x80057B0C` and the gate's `0x80057B10`, and the sound
  handler's private stack round `0x80063254`;
- `0x800597DC`, `0x8005B280`-`0x8005B2A0`, `0x8009522C` and
  `0x800A8FD0`-`0x800A8FEC`, which no GAME.EXE instruction addresses directly
  (DMA, the BIOS pad buffers, or a pointer); unread.

**Nothing a frame shows advances at the render rate while standing.** A census
with creatures and objects moving has not been taken.
One thing the census missed, read in the code: **the flipbook sprites' cel**
(`+0x14` of the list at `0x80095098`) steps on every draw of the sprite, so it
advances at the render rate; `ModelSmoothing` holds it to one step a tick ("Creatures,
objects and their clips" in `docs/SMOOTHING.md`).
