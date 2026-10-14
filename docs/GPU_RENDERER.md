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

`func_8001C7F8` is a **display-list interpreter**: a switch on the top byte of
each list word (line 10748) with cases `0x20`-`0x2C`, one per face kind (`20` F3,
`22` F3 semi, `23` FT3, `24` F4, `25` F4 semi, `26` FT4, `27` G3, `28` G3 semi,
`29` GT3, `2A` G4, `2B` G4 semi, `2C` GT4); `0x21` and codes past `0x3C` are
skipped, and `0x2D`-`0x3C` go through the table at `0x8001222C` (not yet read).
No sprite, tile or line packet is written anywhere in GAME.EXE's world path.

Writers outside the renderer, all 2D FT4 (some semi-transparent), into tables of
their callers': the card message `func_8001B7B0`, the exit picture
`func_80014314`, the menu's `func_80022D7C`, `func_80025DA0`, `func_80027E58`,
`func_80027EA0`, `func_80027EE4`, `func_8002A510`, `func_8002ACCC`,
`func_8002AD1C`, `func_800292F8`, and the event picture `func_8002C794`.

Open: where the projection happens for the map's and objects' writers (they have
no GTE in their bodies, so it is likely the vertex cache `func_8001C60C`); kinds
`0x2D`-`0x3C`; the depth sum in `func_8001ED38`'s path; whether the packets
written by `func_8001B7B0` before the frame head, and by the menu after the last
present, are ever drawn.

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

Not located: death, the ending. OPEN.EXE's `8001A82C` presents but is reached only
through a function table.

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
  Whether 135 is the map's tile meshes and 200 the models is to be found.

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
**Not tried**: areas 2, 4 and 5, for want of a known cell in them; the music.

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
- **The arm draws nothing**, standing or walking with a New Game's empty hands;
  whether it draws with a weapon equipped is to be found.
- The probe slows the frame (54-143 renderer calls a second at 144), and counts
  the renderer's modal-loop calls apart from the main loop's.
