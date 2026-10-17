# Widescreen

Verdite3's `docs/WIDESCREEN.md` is the model. The runtime draws a display buffer
into a render target with extra columns either side, copies only the original
columns back to VRAM, and presents the widened target at the chosen aspect; the
projection is untouched, so pixels keep their aspect and the margin shows only
what the game submits past the screen edge. **Off (4:3) until judged by eye.**

## The margin, the latches and the tints

`patches/Widescreen.cs`, Verdite3's file with this game's executables:
`KF1_WIDESCREEN=16:9` (or a ratio, or `off`) for the run, otherwise the aspect
kept as `kf1.widescreen.aspect` (Settings ▸ Testing ▸ Aspect). It clears the
margin latches when OPEN.EXE or GAME.EXE loads, and stretches full-screen tints
(flat, semi-transparent quads spanning the clip rectangle) across the margin
(`KF1_WIDESCREEN_EFFECTS=0` compares). `KF1_WIDESCREEN_PROBE=1` counts the
primitives that reach past the 320 columns; `=2` also lists the screen-wide ones.
The stretch came with Verdite3's file. **This game draws no full-screen tint**
(measured 2026-10-15; its fades go through the colour matrix, "The tints between
ticks" in `docs/SMOOTHING.md`), so the stretch has nothing to act on.
The command channel's `aspect` verb reads or sets it.

Measured 2026-10-05 at 16:9, 144 fps, turning and walking at the start of area 1:
**about half of the primitives reach the margin** (50.5-53.6%), so the game
submits plenty past the 4:3 edges for the margin to show. No screen-space test
was found in the cell drawer `func_8001E5EC` or the model submitters (no screen
width constant in them), unlike Verdite3's near path.

## The HUD at the edges

Verdite3's anchored HUD, off by default: `KF1_WIDESCREEN_HUD=1`, or Settings ▸
Testing ▸ HUD at the screen edges (`kf1.widescreen.hud`). Around each of the two
HUD drawers, at a wide aspect, `Widescreen` moves the records' X out by the margin
and puts them back after:

| drawer | table | records |
|---|---|---|
| HUD sprites `func_8001F9D4` | `0x80055C5C`, 14 bytes, X at +6, to a 0xFF | 13: the HP and MP bars (0-1 at x 31), the two numbers over 100 (2-3 at x 129), the condition marks (4-7), the panels (8-11 at x 10 and 94), the compass dial (12 at x 269) |
| compass `func_8001F8B0` | `0x80055D74`, 0x1C bytes, translation X at +6 | the needle at (286, 34, 200); `SetGeomScreen(200)` and Z 200 make its X a screen pixel |

**A record goes left when its X is under 160 and right otherwise.** Verdite3 took
the sprite's middle; here the gauge code rewrites widths every frame (records 2
and 3 are `u16[0x800A0798] / 100` and `u16[0x800A079C] / 100` wide), so a middle
could cross 160 and send a number to the other edge. The HUD models (the selected
item, from `0x80055D20`) are centred and stay. `KF1_WIDESCREEN_PROBE=1` logs each
distinct set of records drawn.

Measured 2026-10-17 at 16:9 (margin 54): the probe drew the sprites at 0@31,21
1@31,35 2@129,22 3@129,36 8@10,18 9@10,32 10@94,19 11@94,33 12@269,18 and the
compass at 0@286,34. A RAM dump held their FT4s at (−23, 21), (−23, 35), (75, 22),
(−44, 18) and (323, 18), each 54 px out, and the compass's triangles about x 340.
The records read 31 and 286 again after each frame. **By eye, still to do**: the
HUD at 16:9 and 21:9, against its own art.

## The menu's background, tiled

The in-game menu's background is one 128 x 128 tile drawn four times: four
prebuilt FT4s per buffer at `0x800580E8` (0xA0 a buffer, CLUT `0x78C0`, tpage
8, colour `0x404040`), each 160 x 120 with UVs 0-127, in a 2 x 2 grid over the
4:3 screen, linked at slot 3000, the back of the menu's table. Four routines link
them: the window writer `func_8002A510`, and inline `AddPrim`s in `func_80022D7C`
(which also heads and presents its own frame), `func_80025DA0` and
`func_800292F8`. At a wide aspect the margin beside them was the clear's black.

Before the menu's presenter `func_8002AC34` (called by about twenty menu loops),
`Widescreen.BeforeMenuPresent` walks slot 3000's chain for packets in
`0x800580E8..0x80058228`, copies each 320 px left and right (and 640, and so on,
while a copy still reaches the margin) from the menu's own packet cursor
`0x80057E88`, and links them on the same slot. A 320 period repeats whatever the
four hold. A table whose slot-3000 head is already the last tile added is not
tiled twice. On whenever an aspect is set; `KF1_WIDESCREEN_MENU=0` is the
comparison.

A first version hooked the window writer only, and missed the pages that link
the background inline; the options page was seen without it.

Measured 2026-10-17 at 16:9 (margin 54), touring the seven pages with
`scratch/menutour.py` and `KF1_WIDESCREEN_PROBE=1`, which logs each presenter
caller once: 13 callers (`800223A8`, `80022408`, `800225D0`, `800228E4`,
`80022BC0`, `80023700`, `80023740`, `80024034`, `80024B8C`, `80025A48`,
`80025B48`, `80028544`, `800285CC`), each with 4 background packets and 4 tiles
added. A RAM dump's slot 3000 on each page held FT4s at x −160..0 and 320..480 on
both rows ahead of the original four. The menu's packet buffer is about 100 KB a
half (`0x800A3980`, `0x800BCFC0`), about 5.7 KB used by the open menu. Not
reached: the presenter's other callers (`800212D8`, `80021538`, `80021AFC`,
`80021FFC`, `800222B4`, `8002317C`, `800238D8`, `80024E64`, `800250C4`,
`8002552C`, `800286D4`), among them the save and load screens; the probe names
any that presents with no background packets. **By eye, still to do**: the seam
at x 0 and 320.

## The cull

The map walk `func_8001E83C` draws the cells of one of sixteen stencils around
the camera's cell, chosen by the yaw's high byte (stencil `15 - (yaw >> 8)`, each
serving a 22.5° sector of headings; a pitch past ±511 would take a 13x13 one at
`0x80055E9C`, which this game's ±0xBF never reaches). The model walk draws only
what stands in the same stencil's cells. **The stencils are data**: sixteen
records of `0xCC` bytes at `0x80065BE8`, read from `KF/COM/COM.DAT` (offset 428):
width and height (14), the camera's column and row in the grid, then a byte a
cell (0 not drawn, 1 drawn, 2 for 96 cells, unread). The camera sits at column 7,
row 1 of stencil 15. A yaw faces `(-sin, cos)` in x and z (measured walking).

The renderer sets `SetGeomScreen(200)`, so a 320-wide screen spans 38.7° either
side; a stencil must cover that plus 11.25° for the width of its sector, 50°. The
game's own tables measure 56-68° either side out to 8 cells and narrow beyond
(43-48° at 10 cells, 32-37° at 12): enough for 4:3 near, short of it far, where
the fog is. **They are not a clean rule**: the nearest cone (sampled over five
headings of the sector, nine camera places in its cell and sixteen points of
each cell) is about 48° within 10 cells, and still differs on 305 of about 2,100
cells.

`patches/CullCone.cs` therefore removes nothing of the game's. Before each map
walk it checks the tables in RAM against the game's (taken the first time they
are seen, and again whenever they are read in afresh), and at a wide aspect adds
to each stencil the cells that the 48° cone gains when it opens by the aspect's
extra half-angle (8.2° at 16:9, 16.2° at 21:9), inside the 14x14 grid; at 4:3 the
game's own tables go back. `KF1_CULLCONE=0` compares.

Measured 2026-10-05, standing at the start of area 1 (`KF1_CULLCONE_PROBE=1`):

| aspect | cells added (16 stencils) | cells drawn a frame |
|---|---|---|
| 4:3 | 0 | 136 |
| 16:10 | 75 | |
| 16:9 | 107 (5-9 a stencil) | 141 |
| 21:9 | 235 (11-19 a stencil) | 147 |

The additions are the same in each quarter of the circle, as a cone's should be.
The packet buffers hold about 64 KB a frame and about 24 KB are used at the start
(the rate census), so the extra cells fit. **Not judged by eye**: cells appearing
at the sides as you turn, models at the sides, the far reach past 8 cells where
the grid itself ends.
