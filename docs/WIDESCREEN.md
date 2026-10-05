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
The command channel's `aspect` verb reads or sets it.

Measured 2026-10-05 at 16:9, 144 fps, turning and walking at the start of area 1:
**about half of the primitives reach the margin** (50.5-53.6%), so the game
submits plenty past the 4:3 edges for the margin to show. No screen-space test
was found in the cell drawer `func_8001E5EC` or the model submitters (no screen
width constant in them), unlike Verdite3's near path.

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
