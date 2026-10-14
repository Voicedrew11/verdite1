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

**Death**: the damage routine `func_8001564C` adds to HP, clamps it, and calls
`func_80015164` at 0 or below, which sets the state byte `0x800A0822` to `0xFF`.
Stage A's `func_80018540` then drops the eye and rolls it, calling the renderer
itself, and `func_800154B0` resets HP and MP for the respawn (state `0xFE`, then
0 in `func_800186C4`). The main loop keeps running: no loop of its own. Measured
by accident in area 5 (below): HP to 0, the eye falls, and about 3 s later the
player stands in area 1 at (31000, 5000) with HP 30; the renderer ran at 60-145
calls a second throughout. Poking `0x800A0822` to `0xFF` does not kill the player:
the eye jolts for a second and play goes on.

**The ending** (read, not run): `func_80036AF0` returning non-zero makes the main
loop store `0xFE` in its exit flag `0x800958F8`; GAME.EXE then hands `0xFE` to
OPEN.EXE, which loads `B0\END.` and `B0\ENDG.` and presents through its own
`func_80016DD0`.

OPEN.EXE's `func_8001A82C` presents through its own `PutDrawEnv`/`PutDispEnv`
and has no 3D; no word in OPEN.EXE or `KF/B0` holds its address, so its caller,
an indirect call, is still not found.

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

**Chunk 0 is the map and chunk 1 the models**: `func_8001E83C` selects TMD 0
(`func_8001C0E8(0)`) and `func_8001F218` TMD 1. `func_8001C5B0` stores the
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

| area | map verts | map prims | F3 | F3 semi | FT3 | FT4 | G3 | G3 semi | GT3 | G4 | model verts | model prims (FT4) | bank records (2/3/4) | cell values |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 5552 | 8451 | 4019 | 64 | 1025 | 494 | 2548 | 0 | 20 | 281 | 5741 | 3788 (3501) | 6/18/7 | 71 |
| 2 | 4398 | 6435 | 2259 | 64 | 1497 | 472 | 1282 | 320 | 260 | 281 | 6695 | 4397 (4160) | 4/18/8 | 86 |
| 3 | 4963 | 7614 | 2772 | 64 | 891 | 547 | 1967 | 832 | 260 | 281 | 5958 | 3866 (3592) | 2/18/6 | 81 |
| 4 | 4946 | 7712 | 2741 | 64 | 1229 | 304 | 1951 | 832 | 310 | 281 | 5009 | 3280 (2952) | 2/18/7 | 61 |
| 5 | 3769 | 5583 | 1384 | 216 | 1184 | 222 | 1436 | 860 | 0 | 281 | 5670 | 3715 (3420) | 2/18/- | 70 |

The models' other primitives are F3, FT3 and a few F4. **The map writer takes only
FT3 and FT4**, so most of chunk 0's primitives (F3, G3, G4) never reach a
packet through it: who reads them, if anything, is open. Area 5's MIXB has no
chunk 4; whether the loader's fourth call reads past the table there is not
checked.

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
