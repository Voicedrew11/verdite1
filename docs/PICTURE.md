# The picture: 24-bit colour, perspective, sub-pixel and the Z-buffer

Verdite3's `docs/PICTURE.md` is the model. Every feature here is the fork's work
(`GteDepth`, `GteVertexMap`, the GL backend); this game's files are the switches,
the probes, and the two places the dither reaches the GPU. **All are off until
judged by eye**, and each is live in Settings ▸ Testing.

## Unit 1: no dither, 24-bit

`patches/NoDither.cs` (`KF1_NODITHER=1`) clears bit 9 of the draw mode on both
routes it takes: `PutDrawEnv`'s `dtd` byte (`DRAWENV + 0x16`), and E1 words linked
into the ordering table, each in a pre hook and put back in the post, so the
game's memory is unchanged either side. `patches/TrueColor.cs`
(`KF1_TRUECOLOR=1`) is the fork's 24-bit output switch. The Testing tab offers
them as one Shading combo: Dither (the console), None, Smooth (24-bit).

Measured 2026-10-05 in area 1, `KF1_NODITHER_PROBE=1`: **every draw environment
asks for dither** (144 a second at 144 fps, one a frame) and **no table word
does**; GPUSTAT bit 9 reads 0 with it on.

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

## Unit 3: the Z-buffer

`patches/ZBuffer.cs` (`KF1_ZBUFFER=1`). Verdite3 feeds its depth buffer from
records its C# polygon assemblers write beside each packet; **this game has no
assemblers in C# yet, so the depth comes from the address map** (the fork's
fallback when `GtePacketDepth` is off): a triangle with all three corners
recovered is depth-tested, any other keeps painter's order. The coplanar
tolerance is Verdite3's (`0051`: 1 SZ unit plus 0.5 of the depth's slope), and
blended surfaces draw after opaque ones (`0079`, `KF1_BLENDORDER=0` compares).

Measured 2026-10-05, the same walk: **94.2% of submitted triangles are tested**
(65,088 a second), 4,032 keep painter's order; the depth-clear threshold is off
and never fires. Open: the address map also answers for the arm, the compass and
anything else drawn through the GTE, which Verdite3's records left out; whether
the arm is cut by walls it is pushed into is to be judged by eye.
