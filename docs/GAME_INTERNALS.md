# Game internals

The reverse-engineered game: the main loop and its stages, player state,
movement, areas, saves and the boot stub — every address and routine this project
learns, written here rather than left in the commit that found it. Verdite3's
`docs/GAME_INTERNALS.md` is the model, but none of its addresses apply.

## Status

Read so far (2026-10-05): the boot, GAME.EXE's set-up and main loop, the
renderer (stage I) whole, its frame gate, and where the HUD's and menus'
palettes are loaded.

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
| `0x800A0792` | u16 | HP: damage (`func_8001564C`) adds to it, clamped, and calls the death routine `func_80015164` at 0 or below; stage A's tail also calls it (return `0x80019A90`). Death and its state byte `0x800A0822`: "Drawing contexts" in `GPU_RENDERER.md` |
| `0x800A0794` | u16 | max MP (20 at a New Game) |
| `0x800A0796` | u16 | MP |
| `0x800A078A` | u8 | the area, n for `KF/Bn`: the sequence loader `func_80032A4C` writes it plus `'0'` into `B?\SND?.SEQ` |
| `0x800A0824` | VECTOR | position x, y, z (s32); y is height, negative up (-11500 standing at the start) |
| `0x800A0834` | s32 | the floor under the eye, -100 · the cell's height (-10000 at the start); y follows it, 1500 above, each tick, so a poke of y alone does not hold (2026-10-16) |
| `0x800A0822` | u8 | 0 standing; 1 to 8 for a hit's flinch, then 0; `0xFF` dying, `0xFE` on the respawn |
| `0x8009F847` | u8 | the ending's gate: area 1's start cell exits to the ending only while it is non-zero (written by `func_8002EDD4`, `func_8002F8CC`; "Drawing contexts" in `GPU_RENDERER.md`) |
| `0x800A0838` | SVECTOR | rotation: pitch at `+0`, yaw at `+2` (`0x800A083A`), 0x1000 a turn |
| `0x800A0840` | s16 | strafe velocity: R1 adds `speed >> 2`, L1 subtracts, clamped at ±speed; neither decays it by `speed >> 2` (2026-10-17) |
| `0x800A0842` | s16 | walk velocity: Up adds `speed >> 2`, Down subtracts, clamped at ±speed; neither decays it by `speed >> 3` |
| `0x800A0844` | u16 | this tick's walk speed, from the two velocities through `SquareRoot0` |
| `0x800A0846` | s16 | turn velocity: Left adds `rate >> 2`, Right subtracts, clamped at ±rate; neither decays it by `rate >> 2` |
| `0x800A0848` | s16 | pitch velocity: R2 adds 3, L2 subtracts, clamped at ±10; neither decays it by 2 |
| `0x80057E68`, `0x80057E70` | s32 | the walk speed and turn rate, written by stage A after its pad read: 180 and 28, or 36 and 5 while bit 3 of the condition word `0x800A07AA` is set |

A New Game starts at (31000, -11500, 4000), yaw 0, in area 1. Holding Up for a
second walks about 4000 units along the facing; holding Left for half a second
raised yaw by 280. The New Game set-up at `0x80015280` writes both halves of each
HP and MP pair, and copies `0x800650B4`, `0x800650B6` and `0x800650B8` into
`0x800A07A2`, `0x800A07A4` and `0x800A0784`: base STR, base MAG and the EXP for
level 2 (20, 20, 50; the table below).

### The rest of the block, and the routines that write it

Read off the code 2026-10-17 for the debug mod (`docs/MODS.md`), and measured
there where it says so.

| address | type | what |
|---|---|---|
| `0x800A0780` | u32 | EXP, capped at 99999 by the level-up |
| `0x800A0784` | u32 | EXP for the next level |
| `0x800A0788` | u8 | level |
| `0x800A0798`, `0x800A079C` | u16 | weapon and magic charge gauges, 5000 full; a cast needs 5000 |
| `0x800A079E`, `0x800A07A0` | u16 | STR and MAG practice: +1 at 100 (`func_80015F28`, `func_80015FC0`) |
| `0x800A07A2`, `0x800A07A4` | u16 | base STR and MAG (20 and 20 at a New Game) |
| `0x800A07A6`, `0x800A07A8` | u16 | adjusted STR and MAG (STR -20 while poisoned), derived |
| `0x800A07AA` | u16 | condition bits: 0 poison, 3 slows the walk and turn |
| `0x800A07AC` | u32 | gold, 150 at a New Game |
| `0x800A07B0`–`0x800A07B8`, `0x800A07BC`–`0x800A07C6` | 5, 6 × u16 | ratings the recompute sums from the equipment; meanings unresolved |
| `0x800A07C8`–`0x800A07D2` | 6 × s16 | condition and effect timers, -1 off; `0x7C8` poison (600 on a poisoning hit), `0x7CA` set by a hit's flag bit 2 |
| `0x800A07DC` | u8 | held spell, 0xFF none; `0x800A07E0` points at its record |
| `0x800A07E4` | u8 | weapon; `0x800A07E8` points at `0x8009FF10 + 44·id` |
| `0x800A0810`–`0x800A0815` | 6 × u8 | armour, 0xFF empty; record pointers at `0x7FC + 4·n` = `0x800A00DC + 28·id` |
| `0x800A084A`–`0x800A084D` | 4 × u8 | cell z, x and the last cell z, x ("Changing areas") |

- `func_80016058(s16 gain)`, the level-up: EXP += gain (cap 99999); if EXP ≥
  next, **one** level (level < 255): max HP, max MP, the STR and MAG gains and
  the next threshold from the 12-byte table at `0x800650B0` for levels below 40,
  and the last two entries' difference above; caps 9999 and 999. Then the
  recompute and a sound. HP and MP are not refilled.
- `func_80015714`, the recompute: the ratings reset, adjusted STR and MAG from
  base, and the weapon and armour records summed.
- `func_80016A30(id)` sets the weapon, `func_80016848(id, slot 0-5)` an armour
  slot (jump table `0x80012030`: slots 0-5 store `0x810`, `0x811`, `0x813`,
  `0x814`, `0x812`, `0x815`); both end in the recompute. The equip menu
  `func_800236AC` lists, per row, weapons 0-12, spells 4-8, then armour ids
  26-31 (slot 4), 13-18 (0), 19-25 (1), 32-34 (2), 35-38 (3), 48-51 (5), and
  refuses rows 5 and 6 while slot 1 holds id 21.
- `func_80016324(a0..a3 + 4 on the stack)`, the player hit: conditions (ring
  0x31 protects), HP down clamped at 0 with no death call, and the flinch
  `0x822 = 1`. Eight callers; none reads the return.
- `func_80015164`, death: `0x822 = 0xFF`, a sound. Called by `func_8001564C`, by
  the 0x52 hazard cell in `func_80017A80`, and by stage A on HP == 0.
  `func_800156BC` is `func_8001564C` for MP. `func_80033E10` is a full heal (HP,
  MP, condition bits 0-3 cleared).
- `func_800171FC(angle, distance)`, the player's collision move: stage A calls
  it for the walk and the strafe, and nothing else does. The rate words
  `0x80057E68`/`70` are written and spent inline in stage A, with no call between.
- **Inventory**: one count byte per id at `0x800652A8`, ids 0-79; the shop
  (`func_80021538`) sells up to 99, and the next 80 bytes per shop are its stock.
  Names at `0x80058E60 + 20·id` are ten u16 glyph codes; an unused id's record
  opens `FFFF FFFF` (4, 10, 17, 23, 57, 58, 63, 64, 69, 70, 71). A New Game holds
  ids 0 and 43. No give routine has been found.
- **Spells**: records of 20 bytes at `0x8009CE60`, +0 known, u16 +0x10 the MP
  cost. Records 0-3 (4, 5, 8, 12 MP) are cast from the magic menu
  (`func_8002317C`); 4-8 (24, 4, 15, 5, 2 MP) are held for the cast button and set
  by `func_800167E4(id)`, which also empties the magic charge. Records 9 on have
  no cost and are not the player's. A New Game knows and holds record 8.
- **Creatures**: 128 records of 0x48 at `0x8006C4B8` (`+0 == 0xFF` empty), walked
  by stage E `func_80030818`: `func_8002E6A8` wakes and sleeps them (`+6`), and
  `func_8002FA88` thinks for an awake one through the jump table at `0x80012524`
  on `+8`. `func_8002E2E8(distance)` picks a behaviour by testing the type
  record's u16 ranges against the distance to the player.

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

then a check of the byte at `0x800A078D + 0xBD`. Stage I is the renderer, read
whole below ("The renderer in C#"); `0x800650A0`/`0x80065098` are the camera's
position and rotation.

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
`func_8002B648` the writer `func_8002B73C`. Circle opens the screen, Circle on
slot 1 asks Yes/No (`func_800286D4`), and Circle on Yes writes the save to card A
slot 1 through BIOS `open`/`lseek`/`write`/`close` (B(32h), B(33h), B(35h),
B(36h)) and closes it (measured 2026-10-16; this said two Circles before). Cross
leaves; Triangle is not read.

The save is `BISLPS-00017KF`, **5 blocks** (40960 bytes), its title block
`SC`, icon flag `0x13` (three frames), title `<<  KING'S FIELD  >>` in full-width
Shift-JIS. `func_8002B73C` also writes `BISLPS-00017KFTMP` (the strings are at
`0x80056034` and `0x80056050`); GAME's set-up calls `func_8002C70C`, which checks
the card with `_card_info` (A(ABh)) and creates and erases `KFTMP` to see that it
can write, and shows a card message (`func_8001B7B0(2)`) when it cannot.

**Loading is from the in-game menu.** Cross (`0x40`) opens it
(`func_80036E38` from stage A, then `func_80022348`), seven items dispatched
through the table at `0x800122C8`; **item 5 is Load** (`func_80024E64`, the load
screen `func_8002552C`, the reader `func_8002BDE4`). Measured 2026-10-05: from a
New Game, Cross, Down five times, Circle, Circle, Right, Circle loaded card A
slot 1. On 2026-10-16 that did not load, and Cross, Down five times and four
Circles did, the player standing where the save was made.

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

## The renderer in C#

`patches/Renderer.cs` (stage I, `func_8001FDE4`) and `patches/CameraBlock.cs`
(`func_8001C184`), **on by default**; `KF1_RENDERER` and `KF1_CAMERABLOCK` take `0`
(the recompiled routine) or `verify`. Verdite3's `Stage15` and `CameraBlock`, with
their verifiers, on this game's routines. Every callee is called through a
delegate, so each hook on it still fires.

**The renderer** is twenty call sites and three blocks of arithmetic:

| site | callee | what |
|---|---|---|
| View | `func_8001C184(pos, rot)` | the camera block, below |
| FrameHead | `func_8001BFB8` | flips the buffer index `0x80070E98`, points the packet record pointer `0x80070EB8` (a record of start, end and cursor at `+0`/`+4`/`+8`; every writer bumps the cursor) and the ordering-table pointer `0x80090EBC` (`0x80070EBC + idx * 0x10000`) at its half, `ClearOTagR(ot, 0x4000)`, zeroes three counters |
| PoseMark | `func_800209A8` | marks each of the 12 pose slots at `0x800910C0` (0x14 bytes) in use as unclaimed (`1`) |
| GeomScreen | `SetGeomScreen(200)` | |
| Map | `func_8001E83C` | the map walk: cells through one of 16 stencils (`func_8001E5EC`, `func_8001DE18`) |
| LightMap | `SetLightMatrix(0x80055FE8)` | |
| *gauges* | | with the HUD up (`u8[0x800A0818] == 1`): the HP and MP bars' lengths into the HUD records (`(cur - 1) / 50 + max * 50 / ...` as the code has it, `+0xA` of the records at `0x80055C5C`), the two numbers over 100, and one of four condition marks by the lowest set bit of `u16[0x800A07AA]`; else all eight records off |
| *compass* | | `u8[0x800A0819]` into the records at `0x80055D04` and `0x80055D74`, the needle the negated yaw of the camera copy (`0x80055D86`) |
| Compass | `func_8001F8B0` | model `0x15` |
| HudSprites | `func_8001F9D4(0x80055C5C)` | each record (0xE bytes, to a `0xFF`) with `+0 == 1` through `func_8001E480`, under the light colours at `0x80095060..64` |
| LightHud | `SetLightMatrix(0x80056008)` | |
| HudSetup | `func_8001FAFC` | the selected item's display, a state machine at `u8[0x80095088]` (below) |
| *HUD models* | | `RotMatrix` of (`u16[0x8009508A]`, 0, 0) with the translation (0, 160, 200), and each of the records at `0x80055D20`, `0x80055D2E` and four from `0x80055D3C` with `+0 == 1` drawn by `func_8001E230(rec + 2, 0, 0)`, under the colours at `0x80095066` and `0x8009506A` |
| Models | `func_8001F218` | the model walk ("Creatures, objects and their clips" in `docs/SMOOTHING.md`) |
| Arm | `func_8001F798` | |
| Present | `func_8001C050` | `DrawSync`, `VSync`, `PutDrawEnv`, `PutDispEnv`, `DrawOTag` |
| PoseSweep | `func_80020A98` | frees (`func_800209E4`) each pose slot still unclaimed |

The HUD-model loop is the routine's one loop, and the recompiled body polls
interrupts at its head; the C# polls there too.

**The selected item's display** (`func_8001FAFC`): state 0 waits for an item in
the selected slot (`u8[0x8009506E + u8[0x80095086]]`, `0xFF` empty); states 2 and
3 turn it in, stepping `u16[0x8009508A]` and the countdown `u8[0x80095089]` (15)
**on every renderer call**, so under pacing it animates at the drawn rate (not yet
held to the tick). Item `0x13` shows four digit models (`func_8002ADF8`,
`func_8001FAE4`) instead of one; the slots are rewritten from the game's own state
each tick, so a `poke` there does not stick.

**The camera block** copies a non-null `pos` (16 bytes) to `0x80095744` and its
cell, `X / 2000` and `Z / 2000`, to the halfwords `0x8009575C`/`0x8009575E`; a
non-null `rot` (8 bytes, `lwl`/`lwr`, so unaligned) to `0x80095754`; then
`RotMatrix` (`func_8004E9B8`) of the angles into the view matrix `0x800956A0`, and
of (pitch, 0, 0) into the pitch matrix `0x800956C0`.

**Verify**, the renderer: the recompiled routine runs and every call it makes from
its own body is recorded (all registers, all of RAM and the scratchpad, on entry
and exit); the C# routine then runs from the same entry with each call answered
from the record. Each call must be the next recorded one, with every register,
RAM and the scratchpad equal on entry, and the return state must match. A stretch
of the body in which the recording's poll could have run a handler
(`Interrupts.SlowPolls` moved) is not compared for RAM. The camera block uses the
shared `Differential`.

Measured 2026-10-05, `KF1_FPS=144`, both on `verify`: **8,588 renderer frames, 0
mismatches** (19 stretches interrupted) over standing, turning, walking, a swing,
the menu, the double door, a person, the sprites, the walk onto area 1's exit and
area 3's arrival, and the HUD-model records switched on by `poke` (2,758 HUD-model
calls); **all twenty sites called**; the camera block 0 mismatches over 8,535
calls. (The first session, before the C# polled at the loop's head, had one: 59
bytes of the kernel's area, a vblank handled in the recording only.) Uncapped, standing at the start (`KF1_FPS=off`): 751-783 fps recompiled and
680-781 C#, run-to-run noise; 451 packets a frame both ways. At 144 the view and
model smoothing probes read as before.

**`Renderer.ViewOverride`** draws the frame from a camera of the port's: stored
into the block and `a0 = a1 = 0` handed to it; after the frame the block is
rebuilt from the camera the frame would have used. Shell verb `view [x y z pitch
yaw roll | off]`; its reply's `drawn` is the camera the last frame was drawn
with. Checked: `drawn` reads the given camera while it is set and the handed one
after `off`, and the player does not move.

**Under the renderer** (2026-10-14; each routine in "The draw path in C#",
`docs/GPU_RENDERER.md`):

- **Flipbook sprites.** `SubmitSprite` (`func_8001ED90`) draws each record of the
  counted list at `0x80095098` through the writer `func_8001E230`. The writer's
  argument is a 12-byte cel entry at `0x80055B00 + 12 × (u16 at record +0 + the
  cel byte at +0x14)`: bytes 0-3 the texture rectangle (u, v, w, h), shorts `+4`,
  `+6`, `+8`, `+A` the quad in its plane (x, y, w, h). The packet's CLUT is the u16
  at `0x80095058`, its tpage the u16 at `0x8009505A`, its colour the RGBC word at
  `0x8009505C`, lit by NCDS from the normal at `0x80057B64`. The writer's RTPS of
  the origin (`func_8004DA4C`) gives the OT index, SZ >> 2 plus the record's bias,
  and leaves IR0 at SP+0x54; a2 == 1 scales that IR0 by 3/2 (the depth cue, not the
  size). Measured 2026-10-15: every sprite in areas 1-5 has code `0x2C` and tpage
  `0x8E` (ABR 0), so none is semi-transparent.
- **Object types' flags.** `u8[0x8006BD99 + 152 × type]`, read by the object
  submitter. The low nibble is the animation model. A non-zero high nibble draws
  through the cached writer `func_8001D730`, with the CLUT and tpage from the
  halfwords at `0x80095038` written to `0x80095058`/`0x8009505A`. Set for types 0, 2 and 4 in
  area 5 only, and for 13, 16, 22, 25, 31, 34 and 43 everywhere.
- **Pose slots.** The blender `func_800205D4` takes a slot through
  `func_8001AAB0` → `func_800502DC`, the BIOS `malloc` (A(0x33)). There are 12
  slots of 0x14 bytes from `0x800910C0`, each with its block pointer at +0xC.
- **The selected item's display** (`func_8001FAFC`). The state is
  `u8[0x80095088]` (0, 2, 3), the countdown `u8[0x80095089]` (from 15), and the
  angle `u16[0x8009508A]` (+0x80 a step, up to 0x200).
- **The vertex cache** at `0x800911B0`: 8 bytes a vertex, SXY, then SZ at +4 and
  IR0×2 at +6.
- **The menu's item view** draws through `func_8001ED38`: TMD 4's object 0 at
  depth +1000.

### Addresses the retained scene reads

- `u8[0x8009A748 + 100*row + col]`: the map's cell grid; `0xFF` empty, else
  object = byte - 1. While `s16[0x800A07D2]` is not -1 and its low two bits
  are under 2, bytes 0x44/0x45/0x46 draw objects 0x16/0x17/0x18. A cell draws
  only an object under 100; the map cell's a2 = 1 draws object + 100.
- `u8[0x80095900 + cell]`: the cell's height h, placed at y = -100·h.
- `u8[0x80069018 + cell]`: the cell's turn k = byte - 1; k 1 adds 2000 to z,
  2 to x and z, 3 to x. Its orientation matrix is at `0x80095760 + 32k`, its
  light matrix at `0x800957E0 + 32k`.
- `0x80095744`/`48`/`4C`: the camera as the map cell subtracts it (s16 x, y,
  z); the view matrix is at `0x800956A0`.
- `*0x80090FA8`: TMD 0, the map's (MIXB chunk 1); `*0x80090FAC`: TMD 1, the
  models' (chunk 0); `*0x80090FC8`: the current TMD. Object
  headers at `tmd + 0xC + 28·obj`, offsets relative to `tmd + 0xC`.
- `0x800910BC`: the vertex list the writers read: the rigid object's own
  vertices, or the pose blender's scratch `0x800930F8`.
- `0x800911B0`: the vertex cache: per vertex s16 SX, SY, u16 SZ, and IR0 << 1
  at +6 (the depth cue the writers pass to DPCS/NCDS is twice RTPS's IR0).
- **Measured 2026-10-15**: the grid (all three arrays) does not change while a
  player stays in an area. The double door at (46000, 68500) in area 1, opened with
  Circle, changes no byte of it; its leaves (interaction records 19 and 21 at
  `0x8006F124` and `0x8006F17C`) change at +0x1B, +0x21 and +0x28. A warp or an
  exit replaces the grid whole. `docs/GPU_RENDERER.md`, "Does the grid change
  mid-area?".
- `0x80057B60`: the map's RGBC; `0x80057B58`: the models' textured RGBC.
- **The fog pair** (measured 2026-10-16, areas 1-5): DQA -17,600, DQB 20,971,520,
  with H = 200: the cue starts at 11,000 units and is black at 18,333. DQA moves
  (-17,169 seen in area 4) only while the death or damage dimming runs.
- **The draw stencils**: 16 at `0x80065BE8`, `0xCC` bytes each: u16 width,
  height (14, 14), origin x, z, then one byte a cell; 0 undrawn, 1 object + 100,
  2 the base object. The walk uses stencil 15 - (yaw >> 8). The eye's cell is u16
  `0x8009575C` (column) and `0x8009575E` (row).
- **The light leaves** the polygon writer calls: `func_8004DD40` is NCDT (normals
  at a0-a2, RGBC at a3, IR0 at sp+0x10, colours to the words sp+0x14/0x18/0x1C
  point at); `func_8004DD14` is NCDS (normal at a0, RGBC at a1, IR0 in a2, colour
  to a3). Both leave the a-registers as they found them.
- The pose blender `func_800205D4`: its record's +0xC is the block (the vertex
  list plus every whole key, rebuilt only when the segment changes), +8 the
  segment's last key (start at +4, count at +8, entries at +0xC). The scratch
  is the block plus that key times the weight left in IR0; its last
  gteMIMefunc starts one entry early on both lists and saves and restores the
  scratch entry before start, so vertex start + i takes entry i. The vertex
  count is the fifth argument (stack +0x10).
- The frame head `0x8001BFB8` leaves the GTE's H at 1000 (another draw's); the
  map and model walks run at H = 200, centre (160, 120).

### Semi-transparent model faces

Scanned 2026-10-15 by the polygon writer's mesh build (`RetainedModels.Build`),
one line per mesh built, over the walk: area 1 (warp 1 at 17000,121000), area 2
(57000,37000), area 3 (51000,23000), area 4 (17000,72000 and 79000,71000), area 5
(79000,139000). Meshes are the TMD objects the writer submits, by their primitive
address. Code `0x32` is a Gouraud untextured triangle with the semi bit; `0x22` is
the flat one.

| area | prims at | faces | semi faces | code |
|---|---|---|---|---|
| 1 | `8018225C` | 240 | 240 untextured | `0x32` |
| 1 | `800EED50` | 64 | 64 untextured | `0x22` |
| 2 | `80176544` | 240 | 240 untextured | `0x32` |
| 2 | `8016A52C` | 617 | 28 untextured | `0x32` |
| 3 | `8016DA40` | 240 | 240 untextured | `0x32` |
| 4 | `80163F68` | 240 | 240 untextured | `0x32` |
| 5 | `800F1730` | 152 | 152 untextured | `0x22` |
| 5 | `8015F8BC` | 240 | 240 untextured | `0x32` |

No textured semi face (code bit 2 with bit 1) was met in any of them. An untextured
semi face has no blend mode in its primitive: the GPU takes the ABR from its draw
mode, which the game's packets set (E1 or a textured prim's texpage word) in the
order they are written.

**The ABR the semi packets draw with** (`KF1_SEMI_ABR=1`, `patches/SemiAbr.cs`, measured
2026-10-15 on a capture walk of areas 1-4, 3,770 table walks). The probe walks the
table in `DrawOTag` the way `LibGpu.WalkOTag` does and follows the GP0 state. Its end
state matched the GPU's own `_blendMode` at every walk. The game sends no E1 packets;
the mode comes from textured prims' texpage. Blend order was not active.

| mesh (header/hash) | area | ABR seen | packets walked |
|---|---|---|---|
| `80182240` (8018225C) | 1 | 0 only | 107,918 |
| `800E805C` | 1 | 0 only | 766 |
| `80176528` (80176544) | 2 | 0 only | 72,018 |
| `8016A510` (8016A52C, 617 prims) | 2 | 0 only | 501 |
| `8016DA24` (8016DA40) | 3 | 0 only | 35,982 |
| `80163F4C` (80163F68) | 4 | 0 only | 56,030 |
| `800E9320` | 4 | 0 only | 5,622 |
| `800EED50` (64 faces) | 1 | not seen | |

All seven seen meshes draw every untextured semi face with ABR 0. The 64-face mesh in
area 1 was not in view in any walk. Area 5 was not walked.

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

## OPEN.EXE's frame rate

The attract's loop (`0x80014984` in `func_80014804`) steps everything once a
frame: the sway phase by `0x64` (a 41-frame period), the script `func_80014100`,
two sprites a pixel each. Its present `func_80016DD0` (`DrawSync`, `VSync(0)`,
`PutDrawEnv`, `PutDispEnv`, `DrawOTag`) is the only wait, so on the console the
rate was the frame's cost. Recompiled it presented 60 a second and the attract
ran too fast. `func_80016DD0` is also the present for the fades (`func_800137C4`)
and the ending. No console measurement was found. **`patches/OpenPacing.cs` holds
that one `VSync` call (return `0x80016DEC`) to 3 vblanks**, which is GAME.EXE's
gate (`KF1_OPENPACING`, `1` for the old rate). Whether 2 (30 fps) is closer is for
a person to judge.

**The title's quiet stretch is a timed wait**: `func_8001455C` calls `VSync(0)`
up to 1000 times (16.7 s), stops the music at 600, and leaves early on
`*0x80043178` (a button). It draws nothing new, so the screen keeps the title. It
calls `VSync` itself and is not held. Measured 2026-10-16 with `KF1_LOG=sdk`, no
input: before the hold, 8 s of attract at 60 presents a second, a load, about 1 s,
then the 17 s wait. With it, 25 s at 20, the same load and the same 17 s wait. The
cycle is 50 s, not 30.

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

## The English translation

The fan patch "KF Jap to Eng v1.0" is a PPF3 file over the cue/bin (SHA-256
`18643044…a0da3fb`, 356,686 records), kept beside the disc and off by default
(`KF1_TRANSLATION`, or **Settings → Interface → English translation**, read at
the next launch). Measured 2026-10-05 by mapping every record onto the disc's
ISO 9660 tree: no record touches a sector header or the directory, and none
crosses a sector. The patch changes:

- **Pictures of text**: 249 TIMs (`KF/KAN`, `KF/PRSN`, `KF/TALK`, `KF/ENE1` and
  the rest), `E0`-`E2`, `KF/TIM/M*`, `KF/B0/MIX3`, `KF/B0/MIX9` and
  `KF/COM/MIX.TIM`, which are TIMs too, and `KF/COM/STAT.DAT`, the status screen's
  `POLY_FT4` packets. The rest of each sector's changes are its EDC/ECC.
- **63 bytes of GAME.EXE in eight functions**, every one an `ori` immediate or the
  register of an `sh` in code that builds a glyph string on the stack (x, y, codes,
  `0xFFFF`) for a text drawer: `func_800291EC` (two choices, a0 and a1),
  `func_80029DE0` (one string at a1) or `func_8002AD6C` (a menu of 20-byte
  entries). No branch, no game state. `func_80028380`'s はい/いいえ prompt (s0 == 2)
  is left Japanese.
- **One defect**, in `func_800286D4`, the save point's はい/いいえ: the edit is two
  bytes off, so `sh v1,44(sp)` became `sh at,44(sp)` (the "No" string's first code
  is whatever `at` holds) and `sh v1,46(sp)` became `sh v1,46(t5)`, a store of 6 to
  wherever t5 points. That is a write the Japanese game does not make.

So the port does not run the patched code. `patches/Translation.cs` lays the
records over every sector read (fork `0093`), **except GAME.EXE's 299**, so RAM
holds the Japanese executable either way (checked by `peek` at the edited words),
and puts the English codes into each string before its drawer runs, keyed by the
call's return address: 29 calls. Each slot is checked to hold the Japanese or the
English code first, and a site that holds neither is left Japanese and logged.
`func_800286D4` gets "Yes" `[0x00, 0x06]` as the patch writes it and "No"
`[0x50, 0x00, 0xFF]`, the patch's own "No" in `func_80028380`, with neither stray
store. Whether that reads right is for a person to look at. The save point's
prompt hits it (`[KF1] translation: func_800286D4 at 0x800288D4: English`), and
the same scripted run with the translation on and off reaches the same states.

## Message text

Dialogue is a picture: `func_8002C9D4` builds `TALK\Cxx\Txxxxx.TIM` in the buffer
at `0x8005606C` and calls the event picture `func_8002C794` (a0 = the name), which
loads the file, uploads it through `func_8001B100` (return address `0x8002C87C`),
and draws one `POLY_FT4` at screen (32,112)-(288,240), UV (0,0)-(255,128), 4-bit at
(960,256), CLUT (0,501), opaque, no backing. The fade's brightness rises from 0x20
to 127 and is stored each pass at SP+0x14..0x16 before the `DrawOTag` call
(`func_80050CF0`, return address `0x8002C938`); then it waits on the pad, presenting
(about 290 presents a message at 144 fps). Signs (`KAN\Bx\Kxxx.TIM`), `PRSN` and
`ENEx` go through the same routine. All 149 TALK files are 256x256 4-bit with two
colours used (147 of the English ones; two use three); rows below 128 are not shown.

**The fonts.** Measured 2026-10-17 from the disc, as shipped and with the
translation's PPF:

- *Japanese*: a bold proportional font of the game's own (Shinonome 12 matches 1
  glyph of 559). Lines are 12-row bands at y = 9 + 15j. Cut at empty columns, the
  pictures hold 559 distinct runs; い, ル, 化, 心 and one 八 are two runs each, and
  some glyph pairs with one bitmap (ー/一, カ/力, ニ/二, エ/工, タ/夕, ヘ/へ, ベ/べ)
  are settled by their neighbours (katakana beside katakana).
- *English*: an Arial-like proportional font. Lines are found by their ink
  (bands one empty row apart that fit 16 rows are one line) and placed by their
  baseline, the bottom row most runs end on; a glyph is keyed over baseline-11 to
  baseline+4. 120 distinct runs; letter gaps are 1-3 columns, word gaps 4 or
  more. `l` and `I` are one bitmap, settled by the word.

**The tables.** `scripts/msg_glyphs_jp.tsv` and `msg_glyphs_en.tsv` hold only run
hashes (FNV-1a 64 over the width and the rows) and the character each spells:
font data, no text. `scripts/msg_glyphs.py` checks them against the player's disc
(every hash is found, widths agree, no collision) and writes
`patches/Text/MessageGlyphs.cs`. Coverage, 2026-10-17: Japanese 120 of 149
pictures (522 runs labelled, 6 pairs; 27 runs not labelled, 38 uses), English
144 of 149 (114 runs; 6 not labelled, 15 uses). The English table read on the
Japanese bytes reads nothing, and the Japanese one on the English bytes reads
one picture, the one TALK file the PPF leaves Japanese.

**`patches/Text/MessageText.cs`** (`KF1_MESSAGETEXT`, off). Before the upload, the
picture is read through `PictureText` with the translation's font first when the
translation is on. The glyphs it read are kept by character and width, pixels and
CLUT colours as the disc has them: this is the game's font, gathered from the
player's own disc as pictures are shown. The text is then set again in that
font, at the game's positions, and only if that gives back every pixel of the
picture is the picture hidden (its CLUT row zeroed in RAM) and the text drawn
over the game's picture rectangle, each texel times the fade's brightness /128,
as the GPU blends it. Anything else (an unknown run, a coloured pixel that is not
ink) leaves the game's picture. Measured with `KF1_MESSAGETEXT_TEST` over 15 TALK
files: Japanese 13 read, 13 exact, 2 left; English 14 read, 14 exact, 1 left. No
text is logged; the probe gives counts and the refusal's reason.

Open: signs (KAN) are not read; the glyphs are gathered only from pictures shown,
so drawing text that no picture held (another language's) needs a pass over every
TALK file at start; what spacing rule the game's layout follows is not measured.

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

**Nothing a frame shows advances at the render rate while standing.**

**Nor while things move** (2026-10-15, `scratch/census_moving.py`, `KF1_FPS=144`
at the 15.1 Hz tick rate kept in `interface.ini`, 75 s): area 4 by (17000, 72000), where creature 11 (model 9) walks about 8,000
units a second, then turning, walking and swinging, with the model-smoothing probe
reading creatures, objects and the arm carried on 144 frames a second.

- **With nothing carried** (`KF1_SMOOTH=0 KF1_SMOOTH_MODELS=0`, the C# path,
  3,013 idle pairs): only the working memory above changes, and the stack.
- **Before Step 1** (`KF1_LITERAL=0`, carried) and **after** (carried) read the
  same working memory, plus `0x80090FC8`, `0x800931A8`-`0x80093200` and, in one
  run of the recompiled path, `0x800AAC88`-`0x800ACBAC` and
  `0x800C42C8`-`0x800C628C`. None of these change with nothing carried, so they
  are what the carry writes for the frame it draws, not state the renderer steps.
  They are not the pose slots, which sit at `0x801E17B8` and up in area 4.

The census samples RAM at a vblank, which can fall partway through a frame, so
the working memory it lists changes because of where it was caught. A probe of
every vertex-cache call over about 1,500 pairs of idle frames, standing in area 1
and by the walking creature in area 4, found no call that projected anything
different from one idle frame to the next.

One thing the census missed, read in the code: **the flipbook sprites' cel**
(`+0x14` of the list at `0x80095098`) steps on every draw of the sprite, so it
advances at the render rate; `ModelSmoothing` holds it to one step a tick ("Creatures,
objects and their clips" in `docs/SMOOTHING.md`).
