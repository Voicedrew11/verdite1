# The renderer: inventory and the retained path

The working log for `docs/RENDERER_PLAN.md`. Everything below **was read from
`generated/` and the disc, not exercised in play**, unless a measurement says
otherwise (2026-10-14, Step 0). Line numbers are `generated/game.cs`'s at fork
`c79d32f`.

## The frame's buffers

`func_8001BFB8` (the renderer's frame head) flips the index `0x80070E98` and
points two words at its half:

- `0x80070EB8`, the **packet record**: `+0` start, `+4` end, `+8` cursor. A
  writer reads the cursor, bumps it by the packet's size and stores it back,
  checking it against the end first (`func_8001E480`, line 12682).
- `0x80090EBC`, the **ordering table**: `0x80070EBC + idx * 0x10000`, 0x4000
  words, cleared by `ClearOTagR(ot, 0x4000)`. A packet goes to
  `ot + slot * 4`.

## Who writes packets

| writer | packets | GTE | slot | reached from |
|---|---|---|---|---|
| `func_8001C7F8` | F3, FT3, F4, FT4, G3, GT3, G4, GT4, some semi-transparent | per face `NCLIP`, and `NCDS`/`NCDT`/`NCCT` (`func_8004DD14`/`DD40`/`DDB0`) | `depth & 0x3FFF`; skipped under 5 | submitters `EBB8`, `E9A4`, `ED38`, `EEDC`, `F0C4`; arm `F798`; compass `F8B0` |
| `func_8001D730` | FT3, FT4, GT3, GT4 | none in its body | by depth | objects `E9A4` |
| `func_8001DE18` | GT3, GT4 | none in its body | by depth | **the map**: `E83C` → `E5EC` → `DE18` |
| `func_8001E230` | FT4 | none | `A0 & 0x3FFF` | sprites `ED90`, creatures `EEDC`, the HUD models (renderer) |
| `func_8001E480` | FT4 | none | slot 1 | HUD sprites `F9D4` |

`func_8001C7F8` is a **display-list interpreter**, and its list is the TMD
primitive stream itself (below). It dispatches on the top byte of each
primitive's first word, the TMD mode, through the 29-word table at `0x8001222C`
(codes `0x20`-`0x3C`, read from GAME.EXE). Only the even codes do anything, and
they are the GPU's own polygon commands: `20` F3, `22` F3 semi, `24` FT3, `28` F4,
`2A` F4 semi, `2C` FT4, `30` G3, `32` G3 semi, `34` GT3, `38` G4, `3A` G4 semi,
`3C` GT4. The odd codes, and the gaps between, go to `0x8001D6D4`, which skips the
primitive. The map writer `func_8001DE18` takes only `0x24` and `0x2C`, and
writes them as GT3 and GT4. No sprite, tile or line packet is written anywhere in
GAME.EXE's world path.

**Projection** is `func_8001C60C`: `RTPS` (`func_8004DA4C`) one vertex at a time
from the list at `0x800910BC` into the cache at `0x800911B0`, 8 bytes an entry
(`SXY`, `SZ`, `IR0 << 1`). The map writer calls it itself; the object writer
`func_8001D730` reads what `func_8001E9A4` cached before calling it.

**Depth**: `func_8001C7F8` takes a triangle's mean `SZ` over 4
(`(SZ0+SZ1+SZ2)/3 >> 2`) and a quad's `(SZ0+..+SZ3) >> 4`, adds its second
argument (`func_8001ED38` passes 1000), skips the face under 5, and links it at
`(depth & 0x3FFF) * 4`. The map writer uses `(SZ mean >> 2) + 0xC8` and skips the
face at `0x4000` or more.

Writers outside the renderer, all 2D FT4 (some semi-transparent), into tables of
their callers': the card message `func_8001B7B0`, the exit picture
`func_80014314`, the menu's `func_80022D7C`, `func_80025DA0`, `func_80027E58`,
`func_80027EA0`, `func_80027EE4`, `func_8002A510`, `func_8002ACCC`,
`func_8002AD1C`, `func_800292F8`, and the event picture `func_8002C794`.

Open: whether the packets written by `func_8001B7B0` before the frame head, and
by the menu after the last present, are ever drawn.

## Drawing contexts

| context | entry | presents through | world drawn | own loop |
|---|---|---|---|---|
| gameplay | main loop `800146B8` → `func_8001FDE4` | `func_8001C050` | yes | |
| in-game menu | `func_80036E38` → `func_80022348` | `func_8002AC34`, table cleared by `func_8002ABB4` | **no** | yes |
| save screen | `func_80034DE4` → `800222B4` → `800250C4` | `func_8002AC34` | no | yes |
| load screen | `func_80024E64` → `8002552C` | `func_8002AC34` | no | yes |
| card message | `func_8001B7B0` | its own | no | yes |
| event picture | `func_8002C794`, after two renderer calls | its own | behind it | yes |
| area load | `func_80036618` | renderer | yes | yes |
| area change | `func_80036AF0` fade loops | renderer | yes | yes |
| fade | `func_80036D3C` | renderer | yes | yes |
| interaction | `80033820`, `80033D80`, `80034438`, `800346A8` | renderer | yes | yes |
| exit picture | `800144D4` | frame head + `func_8001C050` | no | 18 frames |
| title, attract | OPEN.EXE `80014804` | `func_80016DD0` | **3D through the GTE** (`8002D820`) | yes |
| death | `func_80015164`, ticked by `func_80018540` in stage A | renderer | yes | no |
| ending | GAME.EXE exits with `0xFE`; OPEN.EXE `func_80014B34`, `func_80014E28` | `func_80016DD0` | read only | yes |

**What each loop waits on** (read 2026-10-15). These loops call the frame gate
`func_800149F4` after their renderer call, so they draw at 20 a second under
pacing as on the console: the area load, the area change, the fade, and the
interactions `80033820`, `80033D80` and `80034438`. The menu, save and load
screens and the exit picture wait a `VSync(0)` a frame. The card message presents
once, then spins on the pad. **Death has no loop**: stage A steps it once a tick
and calls the renderer twice on the tick the roll ends. **The area-5 scene
`800346A8` and the event picture had no wait under pacing**. They are held now
("Menus wait for a vblank" in `docs/DEVELOPMENT.md`).

**Death**: the damage routine `func_8001564C` adds to HP, clamps it, and calls
`func_80015164` at 0 or below, which sets the state byte `0x800A0822` to `0xFF`.
Stage A's `func_80018540` then drops the eye and rolls it, calling the renderer
itself, and `func_800154B0` resets HP and MP for the respawn (state `0xFE`, then
0 in `func_800186C4`). The main loop keeps running: no loop of its own. Measured
by accident in area 5 (below): HP to 0, the eye falls, and about 3 s later the
player stands in area 1 at (31000, 5000) with HP 30; the renderer ran at 60-145
calls a second throughout. Poking `0x800A0822` to `0xFF` does not kill the player:
the eye jolts for a second and play goes on.

**The ending**: `func_80036AF0` returning non-zero makes the main loop store
`0xFE` in its exit flag `0x800958F8` (line 506); GAME.EXE then hands `0xFE` to
OPEN.EXE, which loads `B0\END.` and `B0\ENDG.` and presents through its own
`func_80016DD0`. The only exit that returns non-zero is area 1's handler on the
New Game's start cell (z 2, x 15, lines 52976-52986), and only while the byte
`0x8009F847` is non-zero; `func_8002EDD4` and `func_8002F8CC` write it, and what
sets it in play is not traced. Poking `0x800958F8` does nothing: the loop only
writes it. **Run 2026-10-16**: with `0x8009F847` left at 0, walking onto the
cell does nothing; poked to 1, walking onto it fires the exit, GAME.EXE gives way
to OPEN.EXE, and `END.`, `ENDG.`, `MIX9.`, `MIXAE.`, `MIXAF.`, `MIXAG.`,
`MIXBE.` and `RTBL.` are read from `KF/B0`. After the last read OPEN.EXE presented
about 60 `PutDispEnv` a second for 4 to 6 minutes with no further read and no
return to the title. Whether the ending is over and waiting for a button there is
for a person to see.

**The title and attract, run 2026-10-16** with no input for 3 minutes: OPEN.EXE
stays up throughout and repeats a 30 s cycle, reading `KF/B0`'s `MIXA0.`,
`MIXA1.`, `MIXB0.`, `OPEN0.` and `OPEN1.` each time: about 20 s at 20 to 45
`PutDispEnv` a second, then about 10 s with no SDK call and no present at all,
then the reads again. What the quiet 10 s is (a pause by design, or a wait the
port does not satisfy) is open; it recovers every cycle. Three Starts then reach
area 1 as usual. The pacing and stage probes count only in GAME.EXE, so these
rates are `KF1_LOG=sdk`'s `PutDispEnv` lines.

**The save and load screens, run 2026-10-16** (recipes in "The acceptance test",
`DEVELOPMENT.md`): both open, write or read card A, and return to the loop. They
test only Up `0x1000`, Down `0x4000`, Circle `0x20` and Cross `0x40`, so
Triangle does nothing in them; Cross leaves. The save's second Circle opens a
Yes/No (`func_800286D4`, from `func_800250C4` at line 22474), and the third
(Yes) writes the card and closes the screen, which is the "stays up" of the
earlier acceptance run. The load screen `func_80024E64` hands row 0 to the reader
`func_8002552C`, whose Circle opens the same kind of confirmation; the fourth
Circle after the menu's loads.

**OPEN.EXE's `func_8001A82C` has no caller on the disc** (searched 2026-10-16):
no word, `j`/`jal`, `lui`/`addiu` or `lui`/`ori` pair, halfword table entry or
branch in OPEN.EXE, GAME.EXE, PSX.EXE or anywhere in the disc image yields
`0x8001A82C`, and the word before it is a `jr ra` and its delay slot, so nothing
falls into it. OPEN.EXE's 57 `jalr` sites load from slots that are filled at run
time from arguments or return values. Read from the code, it is a
display-position adjuster: it loops while R1 is held, and the four face buttons
step an x/y pair by 4 (x masked `0x3FF`, y `0x1FF`), presenting through its own
`PutDispEnv`/`PutDrawEnv`. Most likely a debug screen nothing reaches; only a
watch on those slots at run time would rule a call in.

## What is on the disc for each area

Five areas, `KF/B1`-`KF/B5`, each `MIX.TIM` (295,904 bytes: eight 4-bit TIMs with
CLUTs), `MIXA.DAT`, `MIXB.DAT` and its sequences; `KF/B5` also has `CHR1`-`CHR3.MIM`.
`KF/B0` is OPEN.EXE's.

The area loader is `func_8001B558`, from `func_80036554`. Paths are
`\KF\` + `B<n>\` + name; the reader is `func_8001ACF0` (search, `CdRead`,
`CdReadSync`).

- `MIXA.DAT`: eight u32-length-prefixed chunks. 0 and 1 are the VAB; **2 is the
  map, five 100x100 byte grids** copied to `0x8009A748` (the cell grid),
  `0x80095900` (heights), `0x80069018`, `0x800668E8`, `0x80098018` (unread); 3-7
  go to `func_80020B4C`, `func_80031008`, `func_800308C0`, `func_80030A6C`,
  `func_800338B8`.
- `MIXB.DAT`: five chunks (four in `B5`). **0 and 1 are TMDs** of 135 and 200
  objects in every area, read by `func_8001C5B0`; 2-4 go to `func_800204C0`.

**Chunk 0 is the models and chunk 1 the map** (corrected 2026-10-16; this said
the reverse before). The loader hands chunk 0 to slot 1 and chunk 1 to slot 0: in
RAM, area 1's TMD 1 (`*0x80090FAC` = `0x800E7BF0`) is chunk 0's 274,592 bytes,
and TMD 0 (`*0x80090FA8`) starts 4 bytes after it, at `0x8012AC94`, the next
chunk's length word skipped. `func_8001E83C` selects TMD 0, the map
(`func_8001C0E8(0)`), and `func_8001F218` TMD 1. `func_8001C5B0` stores the
pointer at `0x80090FA8 + 4 * n` and calls the TMD walker `func_8001C2B0`, which
turns the primitives' vertex and normal indices into byte offsets (`<< 3`) in
place; the writers then read the primitives as they lie. The map walk takes the
cell grid's byte as the object (`0xFF` empty; read as object `v - 1`, with
`0x44`-`0x46` remapped, which is inferred). Chunks 2-4 are banks of one-object
TMDs, each record `{size, w1, offset to the TMD, w3}` and a word block at +16,
stored from slot `0x0A` (chunk 2), `0x1E` (chunk 3) and 0 (chunk 4) at
`0x80090FCC`. What the records' other words mean, and whether they hold clips, is
not found.

Census, every TMD parsed with no gaps or overlaps (a scratch reader over the
disc; light-source calculation is on in every primitive):

| area | model verts | model prims | F3 | F3 semi | FT3 | FT4 | G3 | G3 semi | GT3 | G4 | map verts | map prims (FT4) | bank records (2/3/4) | cell values |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 5552 | 8451 | 4019 | 64 | 1025 | 494 | 2548 | 0 | 20 | 281 | 5741 | 3788 (3501) | 6/18/7 | 71 |
| 2 | 4398 | 6435 | 2259 | 64 | 1497 | 472 | 1282 | 320 | 260 | 281 | 6695 | 4397 (4160) | 4/18/8 | 86 |
| 3 | 4963 | 7614 | 2772 | 64 | 891 | 547 | 1967 | 832 | 260 | 281 | 5958 | 3866 (3592) | 2/18/6 | 81 |
| 4 | 4946 | 7712 | 2741 | 64 | 1229 | 304 | 1951 | 832 | 310 | 281 | 5009 | 3280 (2952) | 2/18/7 | 61 |
| 5 | 3769 | 5583 | 1384 | 216 | 1184 | 222 | 1436 | 860 | 0 | 281 | 5670 | 3715 (3420) | 2/18/- | 70 |

(The first column group is chunk 0, the models, and the second chunk 1, the map;
the headings said the reverse before 2026-10-16.) The map's other primitives are
F3 (55-155 an area), FT3 and a few F4. **The map writer takes only FT3 and FT4**,
and the objects the cell grid names hold 2,494 FT4 and 84 FT3 in area 1 against 3
F3 or F4 at most (11 F3/F4 and 3-11 F3 in areas 2-5): those few are skipped, and
nothing else reads the map's TMD (2026-10-16: the slots' only reader is the
accessor `func_8001C114`, whose callers each select their own TMD first, and the
map is selected only between `func_8001E83C` and `func_8001F218`). The models'
F3, G3 and G4 are drawn by the general writer `func_8001C7F8`, which takes every
code. Area 5's MIXB has no chunk 4; whether the loader's fourth call reads past
the table there is not checked.

**The bank records** (chunks 2-4; read 2026-10-16 from `func_800204C0`,
`func_8002059C` and the pose blender `func_800205D4`, and parsed over all five
areas). A chunk is `u16` count, `u16` (0 in `B1`), then the records, each from
the next slot (chunk 2 from `0x0A`, 3 from `0x1E`, 4 from 0; `0x14` is the arm's
and `0x15` the compass's, set by `func_8002055C`). Offsets are from the record:

| field | what |
|---|---|
| `+0` | the record's size |
| `+4` | **the clip count**; 0 is a rigid model, and the blender returns before its tables |
| `+8` | the offset of a one-object TMD; `func_8002059C` makes it the current TMD (`0x80090FC8`) |
| `+0xC` | the offset of the key table: record offsets of keys, as many as fit before the first key |
| `+0x10` | the offset of the clip table (20 in all 134 records): one record offset a clip |
| clip | `u16` segment count, `u16` (20 in all 155 clips), then a record offset a segment |
| segment | `u16` flag (0 in all 492), `u16` duration, `u16` the key it starts from, `u16` K and K key indices; a clip's durations sum to 4096 |
| key | `+0` (0 seen), `+4` first vertex, `+8` count, then count 8-byte entries, which `gteMIMefunc` adds to those vertices at the segment's weight |

88 records are rigid and 46 posed; chunk 3, the shared effects (the same domes,
spheres and column in every area), has one posed record an area but none in
`B5`. Not explained: some records repeat a key, and in 19 of the 46 the key
table is one longer than the segments name.

## A warp

The area byte is `u8[0x800A078A]`. `func_80036AF0` picks the destination from a
handler per area (table `0x80012C14`, areas 1-5) keyed by the exit cell; the
transition `func_80036850(area, entry)` loads it and snaps x and z to the cell's
centre. The proposed `warp <area> <x> <z> [yaw]`: in a pre hook on stage A
(`func_80018880`), call `func_80036850` with the area, then write the position,
the yaw `0x800A083A` and the last-cell bytes `0x800A084C`/`4D`, so the exit test
does not fire on arrival. Built as `patches/Warp.cs` (the `warp` verb). The
entry is 0: area 1's dispatcher zeroes `A1` (line 52901) and its exits to areas
2, 3 and 4 set only `A0`; `func_80036618` hands it on to the loader.

Measured 2026-10-14 at `KF1_FPS=144`: area 3 at (51000, 23000), area 1 at
(31000, 4000), area 1 at (45000, 30000) and area 3 again each took about 6 s, after
which the beacon read the area, `loop` true and the requested position, and still
did 3 s later. The game settles the height itself (-11800 at (45000, 30000)).

The handlers snap x and z to `2000 * cell + 1000` of the exit cell, the same cell
in the next area; the exit cells read `0x40` in the cell grid. Measured
2026-10-14 in the other areas: `warp 2 57000 37000`, `warp 2 61000 113000`,
`warp 4 79000 71000` and `warp 4 13000 45000` land and stand; `warp 2 59000
113000`, `warp 4 15000 45000` and `warp 5 79000 139000` (exit cells) leave the
player with no floor, y growing without bound while the loop runs. **Area 5 has
no good cell yet**: of six cells round its exit, two fell and four stood, at
y -12700, and the player died within 1-13 s each time and woke in area 1. Area 5
is entered with entry 1 and the verb sends 0, which is the first suspect. The
music is not checked.

**Why warps fell, found 2026-10-16: the floor word.** The eye's y follows the
floor under it, `s32[0x800A0834]` (-10000 on the New Game's cell), 1500 above,
and the transition leaves both as they were. A warp from a `0x64` cell (floor
-10000) into a `0x70` cell (floor -11200) put the feet 1,200 under the floor, and
nothing stops a fall from under it. The cells that fell are all `0x70`: area 5's
arrival room, area 4's (13000, 45000) and (15000, 45000), and area 2's (61000,
113000) and (59000, 113000); the ones that stood are `0x64`, or lower (area 2's
(57000, 37000) is `0x58`: arriving above a floor drops onto it). Writing y alone does not hold, because the next tick takes it
from the floor word again. The verb now writes the floor, `-100·h` from the
height grid, and y 1500 above it. Measured: the arrival cell (79000, 139000) with
entry 0 and 1, (77000, 137000), area 4 (13000, 45000) and area 2 (61000, 113000)
each stand at -12700. The 2026-10-15 fault after a tour of warps was not
re-run with the floor written.

**Why area 5 killed the player: a creature.** With HP raised to 999 after
arriving at (79000, 139000), entry 1, two runs of four took hits within 10-16 s:
155 HP a hit, and once 39, every 1-7 s, each followed by `0x800A0822` counting 1
to 8 and back to 0 (the flinch; `0xFF` is death). At a New Game's 30 HP one hit
kills, which is what happened in the earlier runs: the deepest area at level 1,
not a port fault. The other two runs were not attacked in 60 s. For a repeatable
corpus in area 5, raise HP (`poke 800A0790 e703e703`, 999/999) after arriving.
Entry 1 (the real one, `func_80036AF0`'s area-4 handler, lines 53061-53062) is
kept in `0x800A078C`, and `func_8001B558` loads a different object set for it
(line 9353 on); the warp does not depend on it. Area 5's own handler sends the
arrival cell back to area 4, so `warp 5 79000 139000 - 1` arrives on an exit, and
the last-cell bytes keep it from firing.

## Measured: who submits, and how full the buffer gets

`patches/StageProbe.cs` (`KF1_STAGEPROBE=1`) and `patches/PrimBufferProbe.cs`
(`KF1_PRIMBUF_PROBE=1`), Verdite3's probes on this game's addresses. A stage
counts only on calls that ran: frame pacing skips stages A-H with a pre hook
whose skip the other hooks cannot see, so the probe asks
`FramePacing.IterationTicked`. The renderer flips to the other table, so for it
the probe counts occupied slots (an empty one links to the slot below it)
instead of diffing.

Measured 2026-10-14 at `KF1_FPS=144`, area 1, standing, walking and turning:

- **Only the renderer submits.** Stages A-H run 20 times a second and write no
  table word and present nothing; the renderer runs 144 times with one present
  each and fills 64-133 slots; the gate is skipped under pacing.
- **The packet buffer is 104,000 bytes a half** (`func_8001BCE0` sets the record
  up) and a frame spends 17,260-39,492 of it (16.6-38.0%); no frame ran out.

## Measured: what each renderer call draws

`patches/GeometryProbe.cs` (`KF1_GEOPROBE=1`), Verdite3's probe: the table walked
before and after each of the renderer's drawing calls, packets new to it counted
by type. 2026-10-14 at `KF1_FPS=144`, per renderer call:

| site | area 1, standing and walking | area 3, on arrival |
|---|---|---|
| map walk `func_8001E83C` | GT4 368-428 | GT4 438-474 |
| models `func_8001F218` | FT4 62-74, FT3 5-12, G3 0-50 | GT3 224-235, G3 49-53 |
| compass `func_8001F8B0` | G3 7 | G3 7 |
| HUD sprites `func_8001F9D4` | FT4 9 | FT4 9 |
| selected item, HUD models, arm | none | none |

- **The map is all GT4**, so the map writer `func_8001DE18` is one path.
- **No sprite, tile, line or draw-mode packet** was added by any site.
- **The arm draws only during a swing.** `func_8001F798` returns when the swing
  phase `s16[0x800A07F0]` is -1, which it is at rest whatever is held. Triangle
  starts a swing (`func_80016B24`, with a weapon at `u8[0x800A07E4]`, `0xFF`
  empty; a New Game holds weapon 0), and `func_80016BC0` steps it by `0x12C` a
  tick to -1 at 4096, about 14 ticks. Measured swinging in area 1: GT3 97 and F3 49
  a call, 4.2 KB a frame (5.0 KB at most), the frame 26.5 KB against 22.3 KB.
- The probe slows the frame (54-143 renderer calls a second at 144), and counts
  the renderer's modal-loop calls apart from the main loop's.

## Measured: the address map's coverage and the draw census

`patches/MapCoverage.cs` (`KF1_MAPCOVERAGE=1`, Verdite3's): each writer's call is
the span of the packet buffer it bumped, the submitter split by the arm or the
compass that called it; at the frame head, before the flip, the table is walked
and each polygon's corners are looked up in the address map. `patches/DrawCensus.cs`
(`KF1_DRAWCENSUS=1`, Verdite2's): the cursor's advance across each of 13 renderer
routines, exclusive and inclusive. Verdite2's mode naming each model is not
ported: it needs the eye's and player's records, not yet found here.

2026-10-14 at `KF1_FPS=144`, area 1 standing: the renderer spends 22,332 bytes a
frame, the map walk 19,136 of it (the map writer, 119 calls), the models 2,640
(the submitter 2,600, the sprite writer 40), the compass 196 and the HUD sprites
360; the tree sums exactly. **The address map holds 98.8-99.7% of the map's
packets** and 98.9-100% of the submitter's in every area measured; the compass
and HUD sprites hold none, as they are not transformed by the GTE.

## Measured: areas and contexts

2026-10-14 at `KF1_FPS=144`, all five probes on, by warp; the renderer's bytes a
frame, mean (largest), and the map's GT4 a call:

| place | renderer bytes | map walk bytes | GT4 a call | buffer peak | models drawn |
|---|---|---|---|---|---|
| area 1 (31000, 4000), standing | 22,918 (35,100) | 19,136 | 368 | 35,212 | FT4 62, G3 50, FT3 5 |
| area 2 (57000, 37000) | 16,671 (29,959) | 12,238 | 219 | 45,056 | F3 121, G3 14 |
| area 2 (61000, 113000), walking | 25,700 | 12,600 standing | | 32,200 | |
| area 4 (79000, 71000) | 45,012 (52,390) | 41,505 | 763 (805) | **55,712** | G3 75, FT4 49, F3 10 |
| area 4 (13000, 45000), walking | 29,500 (32,200) | 18,900 standing | | | |
| area 5, falling | 19,880 (37,129) | 9,079 | 183 | 39,020 | G3 230, FT3 257, GT3 61 |

**The busiest frame is area 4 at (79000, 71000): 805 GT4 in one call and 55,712
bytes, 53.6% of the 104,000-byte half.** No frame ran out. No sprite, tile, line
or mode packet appeared in any place, standing, walking, swinging or dying.

- **The in-game menu** (Cross; Cross closes it) stops the main loop: the beacon's
  `loop` reads false and the renderer does not run. `func_8002ABB4` points the
  same table pointer `0x80090EBC` at the same halves and clears them, and points
  the packet record at its own per-half record `0x80070EA0 + 12 * idx`;
  `func_8002AC34` draws the table. `KF1_STAGEPROBE` counts it on a `menu:` line:
  60 presents and clears a second, and `func_8002ACCC` and `func_8002AD1C` about
  124 calls a present, the latter 10 words a call (an FT4) on both the record
  and the menu's packet pointer `0x80057E88`. The other menu writers did not run
  on the top screen.
- **Start's message** (`func_8001B7B0(3)`) leaves the loop running and the frame
  unchanged; Cross while it shows opens the menu.
- **Death** runs on the renderer (above): 60-145 renderer calls a second, with 0-9
  calls a second from its own paths.

## The draw path in C#

Step 1 of `docs/RENDERER_PLAN.md`, 2026-10-14. All 24 routines under the renderer
are transcribed literally from `generated/`, in `patches/Draw/`, one class each,
and all are on by default. A transcription keeps every register store, memory
access and call of the recompiled body, in the same order: only the control flow
is rewritten as `if`/`while`. Gotos are left where removing one would duplicate
or reorder code: 52 in the polygon writer (its jump table and shared quad tails),
10 in the map cell's stencil state machine and 4 in the pose blender. Callees are
called through `KingsField1_game`, so their hooks still fire.

| routine | address | `KF1_` | what |
|---|---|---|---|
| vertex cache | `8001C60C` | `VERTEXCACHE` | RTPS into the cache at `0x800911B0`: SXY, SZ at +4, IR0×2 at +6 |
| vertex cache (arm) | `8001C6A8` | `VERTEXCACHE_ARM` | the same, for the arm |
| vertex cache (compass) | `8001C754` | `VERTEXCACHE_COMPASS` | RTPT; stores SZ at both +4 and +6 (the game's own quirk, kept) |
| polygon writer | `8001C7F8` | `POLYASSEMBLER` | a TMD object's 12 face kinds through the table at `0x8001222C` |
| polygon writer (objects) | `8001D730` | `POLYASSEMBLER_CACHED` | GT3/GT4/FT3/FT4 with the colours at `0x80095058`/`5A` |
| map writer | `8001DE18` | `MAPASSEMBLER` | a map object's GT3/GT4 |
| sprite writer | `8001E230` | `SPRITEASSEMBLER` | the flipbook sprites |
| HUD sprite writer | `8001E480` | `HUDSPRITEASSEMBLER` | the HUD's FT4s |
| map cell | `8001E5EC` | `MAPCELL` | one cell's stencil |
| map walk | `8001E83C` | `MAPWALK` | the cells around the eye |
| objects … people | `8001E9A4`, `EBB8`, `ED90`, `EEDC`, `F0C4` | `SUBMIT_OBJECT`, `_INTERACTION`, `_SPRITE`, `_CREATURE`, `_PERSON` | the five submitters |
| bank model | `8001ED38` | `BANKMODEL` | TMD 4's object 0 at depth +1000: the menu's item view (`func_800279C4`, `80027B7C`, `800292F8`) |
| model walk | `8001F218` | `MODELWALK` | the five tables |
| arm, compass, HUD sprites | `8001F798`, `F8B0`, `F9D4` | `ARM`, `COMPASS`, `HUDSPRITES` | |
| item display | `8001FAFC` | `ITEMDISPLAY` | the selected item's model turning in |
| pose blender, mark, sweep | `800205D4`, `209A8`, `20A98` | `POSEBLEND`, `POSEMARK`, `POSESWEEP` | |

The object submitter draws through the cached writer when the high nibble of its
type's flags byte (`u8[0x8006BD99 + 152 × type]`) is set. That holds for types 13,
16, 22, 25, 31, 34 and 43 in every area, none of them placed in areas 1-4, and
for types 0, 2 and 4 in area 5, which places them. **Only area 5 exercises the
cached writer.**

### Verifying

`patches/Literal.cs` is the harness. `KF1_<NAME>=verify` re-executes each call
the way the renderer's `Differential` does. It snapshots RAM, the scratchpad, the
registers and the GTE, runs the recompiled body, restores the snapshot and runs
the C# body, then compares. The recompiled result stands. The comparison skips a
0x2000-byte window under SP. Interrupts are deferred for both runs (the poll
countdown is raised and then restored), so a long routine is not cut short
differently in each run. `KF1_LITERAL_DUMP=dir` writes the three RAM images at a
routine's first mismatch.

Results, with pacing off:

- **The corpus with the menu** (`scratch/drive.py`, every routine at `verify`):
  0 mismatches in all 24 in the first run. Calls included the vertex cache
  258,541, the map writer 123,638, the map cell 86,310, the polygon writer
  10,201, the pose blender 5,061, the bank model 838 and the item display 635.
  The re-run on the final tree read 0 except at the pose slots (arm 3, model
  walk 1, objects 2, pose blender 10).
- **Nested, through area 5** (0 interrupted): the map writer 236,757 calls,
  creatures 4,275, interaction objects 2,855, the map walk 644 and the model
  walk 628, all at 0. The only mismatches were at the pose slots (below): arm 3,
  pose blender 10, objects 3, model walk 1. Re-run on the final tree, with the
  depth records in: the same, 0 except the pose slots (pose blender 18 of 10,769
  calls).
- **Area 5 by hand**: the cached writer 865 calls, 0 mismatches. Objects 1,188
  calls, 13 of them at the pose slots.
- **The person in area 1** (`warp 1 17000 121000`; the corpus passes no person):
  people 163 calls, model walk 505, pose blender 2,440. All 18 mismatches were at
  the pose slots.
- **The renderer's own verify** (`KF1_RENDERER=verify`) with every routine on
  its C# body: 1,683 frames, 0 mismatches.

Two artifacts of re-execution, not defects:

- **The pose slots.** The pose blender takes a slot through `func_8001AAB0` →
  `func_800502DC`, which is the BIOS `malloc` (A(0x33)), run by the runtime's HLE.
  The second run gets a different block, so any routine whose call reaches the
  blender can differ at a slot's block pointer. The 12 slots are 0x14 bytes from
  `0x800910C0`, with the pointer at +0xC, so `0x800910CC`, `0x800910E0` and
  `0x800910F4` are slots 0-2. Every mismatch in every run starts at one of
  these, and the dumps show nothing else.
- **Stateful hooks run twice.** Under pacing, `ModelSmoothing`'s cel hold runs in
  both runs of the walk, so the walk is verified with pacing off
  (`kf1.pacing=0` in `interface.ini`).

With pacing at 144 fps and every routine on, the corpus plays. The walk runs
720 times every 5 s, and the item display 100 times, which is the 20 Hz tick.

### What moved with it

- **The model smoothing carry sits in the walk.** `ModelWalk` calls
  `ModelSmoothing.BeforeSubmit`/`AfterSubmit` around each of its five submitters
  and sets `ModelWalk.Carrying`. The submitter hooks stand down while it is set,
  and still carry when the walk is recompiled (`KF1_MODELWALK=0`). What is carried
  is unchanged.
- **The item display steps once a tick** (`ItemDisplay`). Under pacing, its body
  runs only on a frame on which the tick moved, so its state (`u8[0x80095088]`,
  the countdown `u8[0x80095089]`, the angle `u16[0x8009508A]` stepping 0x80 up to
  0x200) moves at the console's rate. `KF1_ITEMDISPLAY_HOLD=0` lets it step on
  every frame again.
- **Packet depths** (`PacketDepth`, Verdite3's `PolyAssemblerDepth`). Just
  before `AddPrim`, the map writer and both polygon writers seal each packet's
  four corner SZ values from the vertex cache with its command word and its first
  and last vertex words. The polygon writers record only inside the model walk.
  For the arm, the compass and the item view they drop the record at that
  address, so those keep painter's order. The cache holds the full 16-bit SZ, so
  no bits are lost, unlike Verdite3's model path. Measured 2026-10-14 over the
  corpus at `KF1_ZBUFFER=1`:
  - every record written is found (hits equal records);
  - 95% of polygons have a depth and 97% of triangles are tested;
  - the polygons with no depth, about 23 a frame, are the arm, the compass and
    the HUD.
- **No HUD transform to port.** The compass projects through the GTE (RTPT,
  `func_8004DADC`), so the address map already sees its fraction. Nothing in the
  24 routines clips or subdivides: the writers cull by NCLIP and drop a packet
  whose table slot is below 5 (the map writer also drops one at 0x4000 and above).

## The retained scene

Step 3 of `docs/RENDERER_PLAN.md`, 2026-10-15. The map and the model walk's
models are published to the shared runtime's `RetainedScene` and drawn by its
main view at the ordering table's far end, under the packets the game still
writes. No runtime change was needed. `KF1_GPU_WORLD` selects the mode
(`docs/ENV_VARS.md`). It is off by default until it has been looked at.

- **`GpuWorld`** (`patches/GpuWorld.cs`) holds the switch, the per-domain
  toggles, the frame and the census. Hooks on the renderer, the map walk, the
  model walk and the arm keep a domain count. The frame head begins a retained
  frame with the view matrix at `0x800956A0` and the camera at `0x80095744`.
  Under pacing these are the carried camera, so the GPU path is smoothed as the
  packets are. The projection is read in the walks: the frame head leaves the
  GTE's H at 1000, from another draw, and the walks run at 200. Every face left
  to the packets is counted by domain, routine, caller and reason.
- **`RetainedMap`** builds the whole 100 × 100 grid as one world-space static
  mesh: about 10,000 cell slots and a million corners.
  - Each cell and variant (object, object + 100) gets its own gate slot.
  - The map cell notes its slot and skips its writer, so the GPU draws exactly
    the cells the game's walk visits.
  - Colours are worked out once per build by NCCS with the turn's light matrix.
  - The depth cue: the writer passes DPCS twice RTPS's IR0 (the cache's `+6`),
    which is exactly the runtime's offset curve with DQB raised by 800 × 4096.
  - The mesh rebuilds only when the grids, the matrices or the GTE's light and
    fog change, and only once the change has held for ten walks. The fog's fade
    at an area's exit steps every tick, and those walks draw from packets
    (`map-source-changing`).
- **`RetainedModels`** takes the polygon writer `func_8001C7F8` at its entry.
  - The mesh is the TMD object's faces, kept by header and content hash.
  - The instance is placed in view space from the GTE's rotation and
    translation, which already carry the smoothing. It is lit from the GTE's
    light matrix, colour matrix and back colour.
  - A rigid model's pose is its vertex list, kept once.
  - An animated model's pose is the pose blender's block plus its last key, kept
    once per segment. Only the weight (IR0 after the blender) changes from frame
    to frame, so **a pose change uploads no vertices**.

### What stays packets, by decision

Each of these is counted with its reason.

| domain | routine, caller | reason |
|---|---|---|
| arm | `8001C7F8`, `8001F89C` | `packet-owned-arm`: view space, no depth record, painter's order over the world |
| hud | `8001C7F8`, `8001F9A0` | `packet-owned-hud`: the compass and the menu's model |
| models | `8001E230` from `8001EE9C` (the flipbook sprites) | none while drawing: retained as textured quads; `sprite-semi` for a semi-transparent one (none met) |
| models | `8001E230` from the creature submitter `8001EEDC` and the HUD | `sprite-writer`: the same writer, still packets |
| models | `8001D730` | `model-cached-writer`: area 5's objects |
| models | `8001C7F8` | `model-semi-abr-unmeasured`: a semi mesh whose blend mode has not been measured (`800EED50` in area 1, and area 5's); `model-semi-quad`: a semi quad (none met). The seven measured meshes are retained, their semi faces as blended faces (below) |
| map | `8001DE18`, `8001E5EC` | `map-not-built`, `map-source-changing`: the walks before a build |

### Semi-transparent models, 2026-10-15

Every face with the semi bit counts, untextured too; before 2026-10-15 the check
looked only at textured faces, and eight untextured meshes in areas 1-5 drew
opaque (`GAME_INTERNALS.md`, "Semi-transparent model faces"). No textured semi
face is met in any area.

An untextured semi face takes its blend mode from the GPU's draw mode, which the
game never sets with E1. `KF1_SEMI_ABR` measured ABR 0 for seven meshes on every
walk, and `RetainedModels` keeps that table by mesh header and hash. Those meshes
are retained: the opaque corners first, then the semi ones, each published with
`AddBlendFace`.
- The key is the writer's slot (the three SZ summed, over 3, `>> 2`, plus the bias),
  plus the main view's `0xF0`. A face the writer would drop (NCLIP not positive,
  slot under 5) is not published.
- The box is the face's own screen box; the main view uses it only for overlap
  with pending packets.

Measured over areas 1-4 (seven stops, `warp`, turning and swinging):
- `KF1_GPU_WORLD=capture KF1_GPU_CHECK=1`: 482,899 semi faces' slots against the
  slots the writer linked, **0 mismatches**, 0 extra, 0 missing. Models 50.9 M
  vertices, within 1.13 px, depth exact, posed vertices exact.
- `KF1_GPU_WORLD=1`: 246,290 main-view draws, 0 missed; 1,033,113 of 1,033,163
  models retained (the 50 are `800EED50`, `model-semi-abr-unmeasured`); blended
  faces 853,603 noted, 853,603 drawn; model packet bytes 174 KB, down from 1.6 MB.
- The map check read 41,860 px on this walk. All of it came at one stop, area 2
  at (61000,113000): that warp drops the player through the floor, and at y =
  201,915 under the map the GTE's projection saturates where the GPU's does not.
  Before that stop the map was within 1.77 px (267 of 67 M vertices over 1.5 px).
  That stop is a bad warp target, not a renderer fault.

### Measured

The run starts a new game, walks area 1, warps to area 4 beside a walking
creature, then turns and swings (`scratch/step3.py`).

Under `capture` with `KF1_GPU_CHECK=1`, unpaced and at `KF1_FPS=144`:

- **Map**:
  - 26 million vertices checked.
  - Screen position: within 1.7 px; 31 vertices off by more than 1.5 px. This is
    measured near the screen, where the GTE's divide is unsaturated, and the
    worst cases are under 1,300 units from the eye.
  - Depth: within 2. Behind the eye, 0.04% disagree, all at the near plane.
  - Depth cue: within 8 of 8192.
  - Corner colours: 36 million, all exact.
- **Models**:
  - 9.5 million vertices checked.
  - Screen position: within 1.11 px. Depth: exact. Behind the eye: no
    disagreements.
  - Blended poses: 4.2 million vertices, all exact.
- **Uploads**:
  - The map is built once per area: 124 ms for area 1, 35 ms for area 4.
  - 13 meshes and 25 poses (5,919 texels) serve 267,000 model draws.
  - No model vertex is uploaded per frame.

Under `KF1_GPU_WORLD=1`:

- 54,893 GPU main-view draws, none missed; 267,379 instances drawn.
- Packet bytes written while drawing:
  - map: 172 KB, all from the 1,071 cells walked before a build;
  - models: 1.6 MB, the sprites (before they were retained; see below);
  - arm: 14 MB, the arm's swings.

### Sprites on the retained scene

`SubmitSprite`'s writer calls (`func_8001E230` from `8001EE9C`) are retained as
textured quads. `SpriteAssembler` runs the writer's own GTE work (RTPS, RTPT, NCDS)
and writes the packet at the cursor without advancing it. While drawing, it then
publishes an instance instead, with no link and no cursor move. The creature
submitter and the HUD still call the same writer and stay packets (table above).

- **Quad**: a rigid pose of the four corners in the plane, one per (x, y, w, h), and
  a six-corner textured mesh per (cel rectangle, CLUT, tpage), both uploaded once.
  Placement is the GTE's R and T at the writer's entry, which carry the smoothing
  and are the same ones the packet uses.
- **Colour**: the flat cue is an offset-curve instance cue with its ir0 at
  800 + IR0/2, so the curve's w is IR0 at any depth. The lit colour is the shader's
  NCDS dots path, the same one the models use.
- **Blend**: the packet's code and tpage are read every call. All sprites met are
  code `0x2C`, tpage `0x8E` (ABR 0), so none is semi-transparent. A semi sprite is
  kept as a packet and counted `sprite-semi`.
- **Gate**: `KF1_GPU_CHECK` compares the four corners against the packet's xy, the
  OT index against the writer's, and the colour the shader's formula gives against
  the packet's colour.

Measured 2026-10-15 (the worktree's build; the disc loaded; areas 1, 4, 3, 5, 2, 1):

- **Check** (`KF1_GPU_WORLD=capture KF1_GPU_CHECK=1`): 2,399 sprites written as
  packets, 9,596 corners. Worst corner error 0.57 px (none over 1.5 px); OT index
  off 0; colour: 7,197 channels, worst 0.2 of 255 (none over 2). The colour figure
  is the shader's formula evaluated in C#, against the packet's NCDS colour; it is
  not a readback of the GPU's pixels.
- **Retained** (`KF1_GPU_WORLD=1`): 2,403 sprites retained and drawn; the census
  shows `sprite-writer` at 0 bytes and 0 packets. Sprite meshes built 3 (2,300
  hits); sprite poses built 1, 4 texels, once (2,402 hits). No model or sprite
  vertex is uploaded per frame. The main view made 2,402 draws, none missed.
- **Open, found in this run**: with `KF1_GPU_CHECK=1` the game aborted in area 5
  twice in three runs of this build (`PoseBlend` read an unmapped address through
  the object walk, at an area 5 warp followed by presses). The HEAD build with the
  check ran the same route once with no abort, and this build without the check
  ran it once with no abort, and retained once with no abort. The cause is not
  known. Reading the sprite path, its writes go to the packet cursor and to the
  retained store, not to the objects the faulting walk reads; that is a reading,
  not a test. Treat the sample as too small to call it a regression.

**Not yet looked at by eye**: the picture under `KF1_GPU_WORLD=1`, the order of
the sprites and the arm against the retained world, and the frame at an area's
exit.

### Does the grid change mid-area? (2026-10-15)

`RetainedMap.DiffGrid` compares the three grid arrays (cells, heights, turns) at
every walk with the last walk's copy, and counts the bytes that changed
(`map grid:` lines, and the census line after `gpu world`). It runs under
`capture` and `1`, with no switch of its own.

Driven, on port 27911 (`scratch/wt3d_door.py`, `scratch/wt3d_verify.py`):

- **The double door** in area 1 at (46000, 68500), Circle, from four yaw
  settings (the leaves swung at 0, 16384 and -16384; at 32768 nothing changed).
  Under `capture`: **1,445 walks, 0 changed walks, 0 changed bytes**. The leaves'
  records changed, the leaves at `0x8006F124` (record 19) and `0x8006F17C`
  (record 21): their +0x1B, +0x21 and +0x28 bytes (`GAME_INTERNALS.md`). So the
  door moves, and its state is the object list's, not the map's.
- **The Step 3 gate** (warp to area 4, then walk and turn): **870 walks, 2 changed
  walks, 28,808 bytes**. One is the warp into area 4 at walk 388 (14,404 bytes:
  the cells of area 1 replaced by area 4's, logged cell by cell); the other,
  near walk 770, is the walk back out to area 1 by an exit (the final state
  reads area 1; its frame was not logged). **Neither changed a cell inside an
  area.**

**Result: nothing rewrites the grid mid-area in the cases driven.** Area
transitions replace the grid whole, which the rebuild already does. No dirty
update was written, because there is no mid-area writer to serve. Not measured:
lifts, the gate that rises (state 2 in `SMOOTHING.md`), and any other interaction
kind. A lift or a gate that moves a cell would show in the same counter and needs
driving to be checked. The door's swing alone changes no map cell.

Verification, the same run (Step 3 gate, `KF1_GPU_CENSUS=1`):

- `capture` with `KF1_GPU_CHECK=1`: map 736,819 vertices, screen max 1.61 px
  (3 over 1.5), depth max 2, behind the eye 12 of 38,711 disagree, cue max 8,
  corner colours exact (955,618, none off); models 352,437 vertices, screen max
  1.10 px, no disagreements, posed 99,540 exact. The same figures as above.
- `KF1_GPU_WORLD=1`: **866 GPU main-view draws, 0 missed**, 4,175 instances, 3
  map builds, 1.84 MB of map packets written while drawing (the cells walked
  before a build and the 118 walks in which the source was changing; the
  pre-existing fallbacks, not the diff).

## The picture on the retained scene

Step 4 of `RENDERER_PLAN.md`, built 2026-10-16. Every switch is the runtime's
existing mechanism, set from `patches/SceneFeatures.cs` (Settings ▸ Testing,
"World on the GPU", and the `KF1_*` switches in `ENV_VARS.md`). Each acts only
while the retained renderer draws (`KF1_GPU_WORLD=1`), and each is off until
judged.

| switch | what it sets |
|---|---|
| `KF1_PERPIXEL` | `GteLightMap.Enabled`: the world shader lights a pixel, not a corner |
| `KF1_FOG` | `corners` (the GTE's), `depth` (`MainFogFromZ`), `distance` (`RadialFog` too); the last two need per-pixel |
| `KF1_AO`, `KF1_AO_NORMALS`, `KF1_AO_QUALITY` | screen-space AO, its normal buffer and its resolution and samples |
| `KF1_ANISO`, `KF1_MIPMAPS` | the world textures' filtering |
| `KF1_ENHANCEDIST` | `PlainDepth`: the enhancements stop past this many cells (2-16) |
| `KF1_RENDERDIST`, `KF1_RENDERDIST_FADE` | "Render distance", below |

### The game's fog

The depth cue is GTE fog: the writers pass DPCS twice RTPS's IR0 (the vertex
cache's +6), toward a black far colour, so the retained path draws it as the
runtime's curve 1 with DQB + 800 × 4096, keeping `1 - 2·IR0/4096`. **One pair for
the whole game** (measured on a walk of areas 1-5, `scratch/fog_measure.py`): DQA
-17,600, DQB 20,971,520 at H = 200. The cue starts at 11,000 units (5.5 cells) and
is black at 18,333 (9.17 cells). DQA reads -17,169 in area 4 during the death and
damage dimming, which rebuilds the map (66 builds in a run with no Step 4 switch
on): a game effect, not Step 4's.

So this is Verdite2's kind of fog, not Verdite3's near/far kind. `Fog`'s
`depth` and `distance` modes evaluate the same curve per pixel, from the pixel's
depth or its distance to the eye.

**Not applicable: `EvenFog`** (`0088`, neighbour blending across tile edges).
KF1's cue is per vertex from the GTE curve, with no per-tile fog level to blend.

### Checks (`KF1_GPU_CHECK=1` under `capture`)

- **Interior fog** (`RetainedMap.CheckInterior`): at each face triangle's
  centroid and edge midpoints, the cue interpolated the way the shader does
  against the GTE's DPCS at that point's depth. 97,964,032 samples: 76.4 M
  exact, 21.6 M off by 1, **none worse** (max 1 of 255).
- **Model light** (`patches/LightCheck.cs`): at every corner the polygon writer
  lights through `func_8004DD40` (NCDT) and `func_8004DD14` (NCDS), the world
  shader's sum (LLM · normal, BK + LCM, × RGBC, kept by the cue) in floats against
  the packet's colour. 16,459,976 corners: 7.67 M exact, 8.79 M off by 1, **none
  worse** (max 1).
- The map in the same run: 34 M vertices within 1.79 px, and 42.4 M corner colours
  exact.

### Measured with everything on

Route `scratch/step4.py` (area 1 walk, warp to area 4, warp to area 2) with
per-pixel, depth fog, AO, anisotropy 8, mipmaps, render distance 16, fade 2 and
`KF1_GPU_SURFACE_PROBE=1`:

- 59,969 GPU main-view draws, **0 missed**; no errors.
- Mipmaps: 665 of 2,831 texture keys found mipped.
- AO: 60,282 passes; 335 frames without a target (the menus and loads).
- Normal triangles drawn: map 5.44 G. The model instances' normals are not in
  that counter; the surface probe shows they reach the buffer: 13 checks,
  314,519 pixels, 119 behind, 72 ahead, 84 missing.
- Render distance: 102.5 cells added a frame (max 185) and 183,401 models past
  the stencil.

`fog=distance` with `KF1_ENHANCEDIST=6` and fade 2: `plain = 12000`, no errors.

### Not built

- **`Pgxp`**: neither Verdite2's retained path nor Verdite3 uses it, and the
  retained scene leaves no world packet geometry that needs it.
- **`AoWorld`**: the fork's world-AO grid is hard-coded for 80 × 80 tiles, a
  2,048-unit height span and a 128 height unit. It needs a game-neutral fork
  patch making those settable, and a KF1 adapter, and which cells stop sight here
  is not known. Deferred.
- **`ProjectileLights`**: the glowing effects are not identified, and every
  sprite drawn so far is opaque. Deferred.

## Render distance

`patches/RenderDistance.cs`, Verdite3's mechanism on this game's walk.

**The game's reach.** The map walk `0x8001E83C` draws the cells of one of
sixteen 14 × 14 stencils around the eye's cell (`GAME_INTERNALS.md`), and the
model walk's five walks test the same stencil. The nearest an undrawn cell inside
the 4:3 cone comes to the eye, over every stencil, heading and place in the eye's
cell, is **15,285 units (7.64 cells)**, measured in-game (`KF1_RENDERDIST_PROBE`).
That is inside the fog's black (9.17 cells), so at 4:3 the game's reach shows
fog-dimmed edges. At 16:9 the wider cone sees cells beside the eye that no 14 × 14
stencil holds, about 1.6 cells away, so the edge is measured on the 4:3 cone and
those side cells are left to the render distance.

**Past it.** After the map walk, every cell whose nearest point is within the
distance and whose box meets the horizontal cone (plus 0.25 rad for pitch) gets
its gate byte in `CurrentMainHalves` (a KF1 slot is its gate index). The model
walk's stencil tests ask `RenderDistance.Reach` before each record advance
(`ModelWalk.Past`), so the models standing there are submitted too. The guest's
stencils are never changed, and the packet path keeps the game's reach. With 10
cells: 12.5 cells added a frame (max 23) and 36,575 models past the stencil over
the route.

**The fade.** Runtime `0089`'s `DistanceFade`, ending at the render distance, or
at the game's edge when the distance is off.

Since the fog is black by 9.17 cells, a distance past that adds only black at
4:3. What it buys is the wide aspect's side cells, and the fade's softer edge.

## Water: none to classify (Step 5, 2026-10-16)

**This game has no surface the siblings' rule would call water**, so `Murk`,
`Waves`, `WaterSwell` and the reflections have nothing to draw on and are not
built. Both siblings call a face water when it is **blended and samples a
scrolling texture** (Verdite3's `WaterRects`, Verdite2's fluid slots); this disc
has neither half:

- **Nothing scrolls.** `KF1_VRAMCENSUS` saw no VRAM block change in 200 ticks
  standing in each of areas 1-5 ("Textures" in `SMOOTHING.md`). A scroll written
  into a mesh's UVs in RAM would not show there, so RAM was compared: two dumps 6
  s apart, standing in each area, change **no byte** of the map TMD, the model TMD
  or any bank record (areas 1-4 after a warp; area 5 at its arrival with HP
  raised; the controls changed 395 and 53,497 bytes elsewhere in RAM).
- **No textured face is blended.** No map or model primitive on the disc has the
  textured semi codes (`0x26`, `0x2E`, `0x36`, `0x3E`); the map writer
  takes only `0x24` and `0x2C`. Every blended face is untextured.
- **No blended object is a surface.** Every TMD object with a blended face, in
  the two TMDs and the banks of all five areas, parsed from the disc: the model
  TMD's objects 40, 41, 47 (and area 5's 11 and 42) are 100-540 units across, under
  a cell's 2000; the banks' are domes, spheres and a 2000-tall column (chunk 3, the
  same in every area, pale yellow: spell effects by their shape, not confirmed),
  and four meshes in chunks 2 and 4 of 128-617 faces, standing 1,375-1,900 tall. Counted by corners at one height, **none has a level face** except area
  5's object 42 (24 of 152, 250 units across). The control, the map's opaque
  untextured faces, finds 515 level ones.

What the user may still know: if a place in the game shows water or lava, its
cell and area would say which mesh draws it, and these checks would be re-run on
that mesh.

## Widescreen and menus: nothing to open (Step 6, 2026-10-16)

**No clip in this game's draw path is bound to the screen**, so `ViewClip` and
`NearScreen` have nothing to widen. Every skip in the writers and their C# ports
(`func_8001C7F8`, `8001D730`, `8001DE18`, `8001E230`, `8001E5EC`, `8001E9A4`;
`patches/Draw/`, `RetainedMap`, `RetainedModels`) is NCLIP, an ordering-table
slot (under 5, or `0x4000` and over), a primitive code, the buffer's end, or a
cell, stencil or cone test in world space; none compares a screen coordinate
with 320, 240, a multiple of them or the GPU's `|dx|`/`|dy|` limits, and no
clip-volume table like Verdite2's is stored. The only 320 is the draw
environment's clip from `func_8001BB94`, which the runtime's margin already
widens. The `-64..384` screen bounds in `RetainedMap`/`RetainedModels` belong to
`KF1_GPU_CHECK` and drop nothing.

**The menus show no world, so `MenuWorld` would change the game, not fix it.**
`func_8001BB94` sets the draw environments' `isbg` to 1 (black), and the menu's
presenter `func_8002AC34` puts the same environment each frame. Measured with
the in-game menu open at 16:9 (`KF1_LOG=sdk`): `PutDrawEnv env=0x80090EC0
clip=(0,0)-320x240 isbg=1`, then `PutDispEnv` and `DrawOTag`, and no
`MoveImage`, `StoreImage`, `LoadImage` or `ClearImage` anywhere in the run. There
is no stored frame to paste: each menu frame is cleared to black, across the
margin too (the runtime widens the background clear, `LibGpu.PutDrawEnv`), and
the menu's FT4s are drawn over it. The save and load screens present through the
same routine. The card message and the event picture turn `isbg` off for their
own presents (lines 9549 and 31973) and back on after, so they draw over the
last world frame left in the buffers; that is frozen on the console too. Whether
the margin behind them reads right at a wide aspect is for a person to look at.
`MenuDraw` is not needed: the menus' quads are 2D and inside the 4:3 columns.

**`PrimBuffer` is not needed.** Measured at `KF1_FPS=144`, `KF1_GPU_WORLD=0`
(`KF1_PRIMBUF_PROBE=1`), standing in area 1, standing in area 4 at (79000,
71000), and turning there; peak of the 104,000-byte half:

| aspect | area 1 | area 4 | area 4, turning |
|---|---|---|---|
| 4:3 | 22,124 (21.3%) | 45,136 (43.4%) | 43,784 (42.1%) |
| 16:9 | 22,332 (21.5%) | 45,788 (44.0%) | 47,160 (45.3%) |
| 21:9 | 22,504 (21.6%, the mean) | 45,056 (43.3%) | 48,004 (46.2%) |

No frame ran out at any aspect. The first window after an area loads reaches
about 28,800 at every aspect. The render distance draws only through the
retained scene, so it adds no packets.
