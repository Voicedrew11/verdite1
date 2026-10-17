# The picture: 24-bit colour, perspective, sub-pixel and the Z-buffer

Verdite3's `docs/PICTURE.md` is the model. Every feature here is the fork's work
(`GteDepth`, `GteVertexMap`, the GL backend); this game's files are the switches,
the probes, and the two places the dither reaches the GPU. **All are off until
judged by eye**, and each is live in Settings ▸ Testing.

## Unit 1: no dither, 24-bit

`patches/NoDither.cs` (`KF1_NODITHER=1`) sets the fork's `Gpu.SuppressDither`
(`0094`), which masks bit 9 of the draw mode where every E1 word lands, so both
routes it takes — `PutDrawEnv`'s `dtd` byte (`DRAWENV + 0x16`) and E1 words linked
into the ordering table — reach the GPU without it and the game's memory is never
touched. Until 2026-10-14 it cleared the bit in RAM around each call, which walked
the whole table before every `DrawOTag`; the walk now runs only for the probe. `patches/TrueColor.cs`
(`KF1_TRUECOLOR=1`) is the fork's 24-bit output switch. The Testing tab offers
them as one Shading combo: Dither (the console), None, Smooth (24-bit).

Measured 2026-10-05 in area 1, `KF1_NODITHER_PROBE=1`: **every draw environment
asks for dither** (144 a second at 144 fps, one a frame) and **no table word
does**; GPUSTAT bit 9 reads 0 with it on. Re-measured 2026-10-14 on `0094`: GPUSTAT bit 9
reads 0 with it on and 1 with it off, about 866 draw environments a second
asking for dither either way.

## Unit 2: perspective and sub-pixel

`patches/Perspective.cs` (`KF1_PERSPECTIVE=1`) and `patches/Subpixel.cs`
(`KF1_SUBPIXEL=1`) switch the fork's address map on: it follows each GTE result
from the register the game stores it from to the packet word the GPU reads, and
gives that corner its view depth (perspective-correct texturing) and the fraction
of a pixel the GTE truncated. Nothing in the map is specific to a game.

Measured 2026-10-05, area 1 at 144 fps, turning then walking: **94.1% of the GPU's
vertex lookups hit** (150,624 a second, 9,360 misses), 87,120 vertices a second
carry a fraction, mean offset 0.764 px. The earlier `kf1-port` attempt read 96.2%
standing still.

### The compass's fraction

The compass (`func_8001F8B0`) does not project: its vertex cache `func_8001C754`
transforms each vertex with `func_8004DADC`, a bare MVMVA (`sf` 12, rotation and
translation, no divide) at a screen-space translation (286, 34), and copies MAC1
and MAC2 into the cache as halfwords. No screen coordinate leaves the GTE through
SXY, so the address map never saw a compass vertex: **0 of 5,184 a window were
mapped**, with the recompiled routines and with the C#.

Verdite3's `PublishScreenVertex` (`PolyAssemblerHud`) is the fix, in
`patches/Draw/VertexCacheCompass.cs`: with sub-pixel on, each vertex's fraction
(the low 12 bits of `(TR << 12) + R·V`, recomputed from the GTE's own registers;
none where the integer disagrees) is published at depth 0, placed rather than
projected, and the cache's X and Y are stored as one word, the same bytes, so the
map follows it into the packet. The needle always turns, so Verdite3's exception
for unturned pieces is not needed; the HUD's 2D sprites are left whole.

Measured 2026-10-17 (`KF1_HUD_PROBE=1` looks each G3 the compass writes up in the
map): **6,048 of 6,048 vertices a window mapped, all with a fraction** (7
triangles a frame then; 10 since the cull below), standing
and turning; `KF1_VERTEXCACHE_COMPASS=verify` and `KF1_COMPASS=verify` 0
mismatches over 576 and 288 calls. The about 54 misses a frame the sub-pixel probe
counts were the compass's 18 and the HUD sprites' 36. Perspective has nothing to
do on it (untextured G3) and the Z-buffer leaves it in painter's order. **Not judged
by eye.**

### A thin face was culled on whole pixels

Verdite2's `0052` ("A thin face was culled on whole pixels" in its
`docs/RENDERING.md`), seen here as gaps along the compass needle at a high render
scale. Every polygon writer keeps a face when NCLIP (`func_8004DE4C`) of its three
cached **whole-pixel** screen words is positive. With sub-pixel on, a face a pixel
wide collapses to a line on whole pixels at some angles, is dropped, and leaves a
hole between neighbours drawn at their fractional corners. The needle is made of
such slivers.

`patches/Draw/Facing.cs`: the 18 cull sites in the C# writers (`PolyAssembler`,
`PolyAssemblerCached`, `MapAssembler`) call `Facing.Nclip`, which runs NCLIP (so the
GTE is left as the game leaves it) and then, with sub-pixel on and the writer not
under `verify`, decides on the signed area at the fractional corners read from the
vertex map at the three cache words, both ways. A corner with no fraction or
clamped leaves the game's answer. A quad is still decided on its first triangle,
as the game does; Verdite2 later took a quad's whole loop (`QuadFaces`), not ported.
`KF1_SUBPIXEL_CULL=0` compares; `KF1_SUBPIXEL_PROBE=1` counts the faces decided
otherwise.

Measured 2026-10-17, 144 fps, area 1, standing and holding Left (`KF1_HUD_PROBE=1`
counts the compass's G3s): **on the whole-pixel cull the compass drew 7 of its 10
triangles standing and 6 to 7 while turning; with this, all 10 on every frame**.
Over everything drawn it kept 432-576 faces a second that whole pixels dropped, and
dropped 0 they kept. **Not judged by eye**: the needle at a high render scale.

## Unit 3: the Z-buffer

`patches/ZBuffer.cs` (`KF1_ZBUFFER=1`). As in Verdite3, the depth buffer is fed
from the records the C# polygon writers write beside each packet
(`patches/Draw/PacketDepth.cs`, 2026-10-14), whenever the map writer, both
polygon writers and the model walk run in C#. Otherwise, or with
`KF1_ZBUFFER_PACKETS=0`, the depth comes from the address map (the fork's
fallback when `GtePacketDepth` is off), where a triangle with all three corners
recovered is depth-tested. A triangle with no depth keeps painter's order. The coplanar
tolerance is Verdite3's (`0051`: 1 SZ unit plus 0.5 of the depth's slope), and
blended surfaces draw after opaque ones (`0079`, `KF1_BLENDORDER=0` compares).

Measured 2026-10-05, the same walk: **94.2% of submitted triangles are tested**
(65,088 a second), 4,032 keep painter's order; the depth-clear threshold is off
and never fires. On the address map, the arm, the compass and anything else drawn
through the GTE are depth-tested as well, which Verdite3's records leave out.

On the packet records, measured 2026-10-14 over the corpus: every record
written is found, 95% of polygons have a depth and 97% of triangles are tested.
The arm, the compass and the HUD (about 23 polygons a frame) keep painter's
order. Open, by eye: the two sources compared, and whether walls cut the arm
on the address map.
