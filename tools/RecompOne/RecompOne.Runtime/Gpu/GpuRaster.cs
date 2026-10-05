using RecompOne.Runtime.Events;

namespace RecompOne.Runtime;

//old soft raster
public sealed partial class Gpu
{
    // W is the view depth GteDepth recovered for this vertex, and HasW says
    // whether it found one at all. Without it the vertex interpolates affinely,
    // exactly as the GPU did. HasZ is the same recovered number offered to the
    // Z-buffer, independently: untextured geometry has a depth and no texture
    // to correct.
    //
    // Fx and Fy are the fraction of a pixel the GTE truncated off this vertex's
    // position, and HasSub says whether that was recovered. X and Y stay whole
    // pixels so that a RenderPrimEvent listener still sees the coordinates the
    // packet carried -- the fraction is added back below them, at the point each
    // renderer actually places the vertex.
    struct Vert
    {
        public int X, Y, R, G, B, U, V;
        public float W; public bool HasW, HasZ, Clamped;
        public float Fx, Fy; public bool HasSub;
        // PGXP's answer, before it is folded into the fields above: the precise
        // screen position it recovered, whether it recovered one at all, and
        // whether the W that came with it is a real view depth rather than the
        // vertex cache's 1.0 stand-in.
        public float Px, Py; public bool Precise, PreciseW;
        // 0048. What GteLightMap recorded for this vertex; Light 0 is none.
        public float Lx, Ly, Lz, Fog; public uint Light; public int LightGen;
        // 0060. The polygon's texture rectangle, the same for every corner.
        public uint TexRect; public bool HasTexRect;
        public bool Solid;
        public byte Material;
        // GtePacketDepth.Rec.NotRect.
        public bool NotRect;
        // 0067. The vertex map or PGXP answered: the GTE projected this vertex, so
        // the polygon is part of the 3D scene, whether or not it carries a depth.
        public bool Projected;
    }

    static readonly RenderPrimEvent _primEvent = new();

    static readonly int[,] Dither =
    {
        { -4,  0, -3,  1 },
        {  2, -2,  3, -1 },
        { -3,  1, -4,  0 },
        {  3, -1,  2, -2 },
    };

    // Software Z-buffer. Dead on any machine where GL comes up (GpuHle.Active),
    // and kept in step with the hardware path so forcing the software rasterizer
    // still depth-tests. One float per VRAM pixel, +inf meaning empty; LEQUAL
    // so a later ordering-table entry at the same depth still wins, which is
    // the painter's-algorithm tie the console had.
    float[]? _zbuf;
    int _zClipLeft, _zClipTop, _zClipRight = int.MinValue, _zClipBottom;
    int _zGen = -1;

    void DrawPolygon()
    {
        uint cmd = _fifo[0];
        bool gouraud = (cmd & (1u << 28)) != 0;
        bool quad = (cmd & (1u << 27)) != 0;
        bool tex = (cmd & (1u << 26)) != 0;
        bool semi = (cmd & (1u << 25)) != 0;
        bool raw = (cmd & (1u << 24)) != 0;
        int n = quad ? 4 : 3;

        Span<Vert> v = stackalloc Vert[4];
        Span<int> vwAt = stackalloc int[4];
        int idx = 1;
        int clut = 0;
        int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);

        for (int i = 0; i < n; i++)
        {
            if (gouraud && i > 0)
            {
                uint cw = _fifo[idx++];
                cr = (int)(cw & 0xFF); cg = (int)((cw >> 8) & 0xFF); cb = (int)((cw >> 16) & 0xFF);
            }
            v[i].R = cr; v[i].G = cg; v[i].B = cb;

            vwAt[i] = idx;
            uint vw = _fifo[idx++];
            int rawX = CoordX(vw), rawY = CoordY(vw);
            v[i].X = _drawOffsetX + rawX;
            v[i].Y = _drawOffsetY + rawY;
            v[i].W = 1f;

            if (tex)
            {
                uint uvw = _fifo[idx++];
                v[i].U = (int)(uvw & 0xFF);
                v[i].V = (int)((uvw >> 8) & 0xFF);
                if (i == 0) clut = (int)((uvw >> 16) & 0xFFFF);
                else if (i == 1) SetTexpageFromWord((uvw >> 16) & 0xFFFF);
            }
        }

        // Bound before the prim event, so a mod is free to move X afterwards --
        // widescreen does -- and the depth and the fraction stay attached to the
        // vertex. Each vertex is asked for by the address its coordinate word was
        // read out of, which is the vertex itself and not merely the pixel it landed
        // on, so nothing has to be picked between and a miss is simply affine.
        //
        // There are two mechanisms that can answer, and exactly one of them does.
        // GteVertexMap follows the value through memory with a ring keyed on the
        // word itself; PGXP follows it through the CPU's registers with hooks the
        // recompiler emitted. PGXP wins whenever it is on -- that is what the
        // switch means -- and everything below this block is shared.
        bool pgxp = Pgxp.Pgxp.Enabled;
        // DepthWanted, not ZBuffer: ambient occlusion needs the same recovered
        // depth on the same vertices and differs only in what is done with it
        // downstream. Untextured geometry reaches this block through that term
        // alone -- most of the architecture in this game is flat-shaded, so
        // leaving it at ZBuffer meant a depth buffer holding only the textures.
        if (!Detached && (GteDepth.Active || pgxp) && (tex || GteDepth.Subpixel || GteDepth.DepthWanted))
        {
            //0045/0046.
            var lookup = Diagnostics.Profiler.Begin(Diagnostics.Profiler.VertexLookup);
            int hits = 0;
            bool wantW = tex && (pgxp ? Pgxp.Pgxp.TextureCorrection : GteDepth.Enabled);
            bool wantZ = GteDepth.DepthWanted;
            bool wantSub = GteDepth.Subpixel;

            // The old screen-position table, kept only as a comparison: it answers
            // for whatever last projected to this pixel, which is what the exact map
            // exists to stop doing. Off unless KF2_PERSPECTIVE_FALLBACK asks for it.
            if (!pgxp && GteDepth.PositionFallback)
            {
                Span<GteDepth.Attr> a = stackalloc GteDepth.Attr[4];
                for (int i = 0; i < n; i++)
                {
                    a[i].X = v[i].X - _drawOffsetX;
                    a[i].Y = v[i].Y - _drawOffsetY;
                }
                GteDepth.Apply(a[..n], wantW || wantZ, wantSub);
                for (int i = 0; i < n; i++)
                {
                    v[i].W = a[i].Z; v[i].HasW = wantW && a[i].HasW; v[i].HasZ = wantZ && a[i].Z > 0f;
                    v[i].Fx = a[i].Fx; v[i].Fy = a[i].Fy; v[i].HasSub = a[i].HasSub;
                }
            }

            if (pgxp)
            {
                ApplyPgxp(v, vwAt, n, wantW, wantZ, wantSub);
                for (int i = 0; i < n; i++)
                    if (v[i].Precise) { hits++; v[i].Projected = true; }
            }
            else
                for (int i = 0; i < n; i++)
                {
                    if (!GteVertexMap.TryGet(_fifoSrc[vwAt[i]], _fifo[vwAt[i]], out var a)) continue;
                    hits++;
                    // 0067, amended. A vertex published with no depth was placed on
                    // the screen, not projected (the HUD): its fraction applies, and
                    // the polygon stays 2D.
                    v[i].Projected = a.Z > 0f;
                    v[i].W = a.Z;
                    v[i].HasW = wantW && a.Z > 0f;
                    v[i].HasZ = wantZ && a.Z > 0f;
                    v[i].Fx = wantSub ? a.Fx : 0f;
                    v[i].Fy = wantSub ? a.Fy : 0f;
                    v[i].HasSub = wantSub;
                    v[i].Clamped = a.Clipped;
                }
            Diagnostics.Profiler.End(lookup);
            Hle.GpuTrace.Sink?.Vertices(n, hits);
            if (wantSub && GteDepth.Probe) SubCensus(v, n, quad);
        }

        // 0050. The depth buffer takes the assembler's record or nothing; W and the
        // fraction above are left to the address map.
        if (!Detached && GtePacketDepth.Active)
        {
            for (int i = 0; i < n; i++) { v[i].HasZ = false; v[i].NotRect = false; }
            ref readonly var dr = ref GtePacketDepth.Find(_fifoSrc[0], _fifo[0], _fifo[vwAt[0]], _fifo[vwAt[n - 1]], out bool has);
            if (has && dr.Z0 > 0f && dr.Z1 > 0f && dr.Z2 > 0f && (n == 3 || dr.Z3 > 0f))
            {
                v[0].W = dr.Z0; v[1].W = dr.Z1; v[2].W = dr.Z2; v[3].W = dr.Z3;
                for (int i = 0; i < n; i++) { v[i].HasZ = true; v[i].Solid = dr.Solid; v[i].Material = dr.Material; v[i].NotRect = dr.NotRect; }
            }
        }

        // 0048. The lighting inputs, by the packet's address. Only the GL backend
        // draws them, so the software path never asks.
        if (!Detached && HleOn && GteLightMap.Active)
        {
            ref readonly var lr = ref GteLightMap.Find(_fifoSrc[0], _fifo[0], _fifo[1], out bool lit);
            if (lit)
            {
                v[0].Lx = lr.L0x; v[0].Ly = lr.L0y; v[0].Lz = lr.L0z; v[0].Fog = lr.F0;
                v[1].Lx = lr.L1x; v[1].Ly = lr.L1y; v[1].Lz = lr.L1z; v[1].Fog = lr.F1;
                v[2].Lx = lr.L2x; v[2].Ly = lr.L2y; v[2].Lz = lr.L2z; v[2].Fog = lr.F2;
                v[3].Lx = lr.L3x; v[3].Ly = lr.L3y; v[3].Lz = lr.L3z; v[3].Fog = lr.F3;
                for (int i = 0; i < n; i++) { v[i].Light = lr.Light; v[i].LightGen = lr.Gen; }
            }
        }

        // 0060. The texture rectangle the filters stay inside: the port's record for a
        // clipped fan, whose own UVs are only part of its face's, else the polygon's.
        if (tex && !Detached && HleOn && GteTexRect.Active)
        {
            if (!GteTexRect.Find(_fifoSrc[0], _fifo[0], _fifo[vwAt[0]], _fifo[vwAt[n - 1]], out uint rect))
            {
                int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
                for (int i = 0; i < n; i++)
                {
                    u0 = Math.Min(u0, v[i].U); u1 = Math.Max(u1, v[i].U);
                    v0 = Math.Min(v0, v[i].V); v1 = Math.Max(v1, v[i].V);
                }
                rect = (uint)u0 | (uint)v0 << 8 | (uint)u1 << 16 | (uint)v1 << 24;
            }
            for (int i = 0; i < n; i++) { v[i].TexRect = rect; v[i].HasTexRect = true; }
        }

        //dispatch the render event for prims
        if (!Detached) Hle.GpuHle.PortWidenedPrim = false;
        if (!Detached && Event.HasAnyListeners<RenderPrimEvent>())
        {
            var e = _primEvent;
            e.Context = Runtime.Cpu!; e.Memory = Runtime.Mem!;
            e.Count = n;
            for (int i = 0; i < n; i++) { e.X[i] = v[i].X; e.Y[i] = v[i].Y; }
            e.DrawLeft = _drawAreaLeft; e.DrawRight = _drawAreaRight; e.DrawTop = _drawAreaTop; e.DrawBottom = _drawAreaBottom;
            e.Textured = tex; e.SemiTransparent = semi; e.Gouraud = gouraud; e.Raw = raw; e.Clut = clut; e.TexPage = 0; e.Skip = false;
            Event.Dispatch(e);
            if (e.Skip) return;
            for (int i = 0; i < n; i++) { v[i].X = e.X[i]; v[i].Y = e.Y[i]; }
        }

        // The census is taken here rather than in either renderer because this is
        // the one place both of them pass through, and it is taken after the prim
        // event so it reports where the polygon was actually drawn. Bbox is clamped
        // to the draw area: a surface is only occluding what it covers on screen.
        if (!Detached && GteDepth.TriCensus && GteDepth.ZBuffer)
        {
            int x0 = int.MaxValue, y0 = int.MaxValue, x1 = int.MinValue, y1 = int.MinValue;
            float minZ = float.MaxValue, maxZ = 0f;
            bool tested = true, clamped = false;
            for (int i = 0; i < n; i++)
            {
                if (v[i].Clamped) clamped = true;
                if (v[i].X < x0) x0 = v[i].X;
                if (v[i].Y < y0) y0 = v[i].Y;
                if (v[i].X > x1) x1 = v[i].X;
                if (v[i].Y > y1) y1 = v[i].Y;
                if (v[i].HasZ) { if (v[i].W < minZ) minZ = v[i].W; if (v[i].W > maxZ) maxZ = v[i].W; }
                else tested = false;
            }
            x0 = Math.Max(x0, _drawAreaLeft); y0 = Math.Max(y0, _drawAreaTop);
            x1 = Math.Min(x1, _drawAreaRight); y1 = Math.Min(y1, _drawAreaBottom);
            if (x1 >= x0 && y1 >= y0)
                GteDepth.NoteTri(x0 - _drawAreaLeft, y0 - _drawAreaTop, x1 - _drawAreaLeft, y1 - _drawAreaTop,
                                 tested ? minZ : 0f, tested ? maxZ : 0f, tested, tex, semi, clamped,
                                 v[0].R, v[0].G, v[0].B);
        }

        if (!Detached) MaybeClearDepth(v, n);

        if (HleOn)
        {
            HleTri(v[0], v[1], v[2], tex, gouraud, semi, raw, clut);
            if (quad) HleTri(v[1], v[2], v[3], tex, gouraud, semi, raw, clut);
        }
        else
        {
            RasterTriangle(v[0], v[1], v[2], tex, gouraud, semi, raw, clut);
            if (quad) RasterTriangle(v[1], v[2], v[3], tex, gouraud, semi, raw, clut);
        }
    }
    
    // 0052. The game culls on whole pixels (NCLIP > 0 on the first three corners); count
    // the polygons whose fractional corners wind the other way, and those with corners
    // both with and without a fraction.
    static void SubCensus(ReadOnlySpan<Vert> v, int n, bool quad)
    {
        int subs = 0;
        for (int i = 0; i < n; i++) if (v[i].HasSub) subs++;
        if (subs == 0) return;
        if (subs != n) { GteDepth.CensusMixed++; return; }

        GteDepth.CensusPolys++;
        double cf = CrossF(v[0], v[1], v[2]);
        if (Math.Abs(cf) < 2.0) GteDepth.CensusTiny++;
        if (Cross(v[0], v[1], v[2]) > 0 && cf <= 0) GteDepth.CensusFlipped++;
        if (quad && Cross(v[1], v[2], v[3]) < 0 && CrossF(v[1], v[2], v[3]) >= 0) GteDepth.CensusFlipped++;
    }

    static long Cross(in Vert a, in Vert b, in Vert c) =>
        (long)(b.X - a.X) * (c.Y - a.Y) - (long)(b.Y - a.Y) * (c.X - a.X);

    static double CrossF(in Vert a, in Vert b, in Vert c)
    {
        double ax = a.X + a.Fx, ay = a.Y + a.Fy;
        return (b.X + b.Fx - ax) * (c.Y + c.Fy - ay) - (b.Y + b.Fy - ay) * (c.X + c.Fx - ax);
    }

    // A vertex that recovered its sub-pixel position needs a finer grid than whole
    // pixels to sit on, so RasterTriangle works in 1/16ths of one whenever any
    // vertex of the triangle has a fraction to contribute -- and at shift zero,
    // which is the arithmetic it has always done, to the bit, whenever none has.
    //
    // 16 steps is the resolution the PlayStation's own successors settled on and it
    // is well past what a 320x240 buffer can show. It also keeps the edge functions
    // inside the types they already use: the widest primitive the GPU accepts is
    // 1024 pixels, so a coordinate stays under 2^15 here and the products below stay
    // far short of a long.
    const int SubBits = 4;
    const int SubOne = 1 << SubBits;

    // Whole pixels at shift zero; otherwise the position rounded to the nearest
    // sixteenth. A vertex that missed the table carries a fraction of zero, so it
    // stays exactly where the packet put it while its neighbours move.
    static int Fix(int whole, float frac, int sh) =>
        sh == 0 ? whole : (whole << sh) + (int)MathF.Round(frac * SubOne);

    void RasterTriangle(Vert a, Vert b, Vert c, bool tex, bool gouraud, bool semi, bool raw, int clut)
    {
        int spanX = Math.Max(a.X, Math.Max(b.X, c.X)) - Math.Min(a.X, Math.Min(b.X, c.X));
        int spanY = Math.Max(a.Y, Math.Max(b.Y, c.Y)) - Math.Min(a.Y, Math.Min(b.Y, c.Y));
        if (spanX > 1023 || spanY > 511) return;

        // Scaling every coordinate by 16 scales the edge functions and the area by
        // 256 and leaves every ratio taken from them alone, so this is a no-op for
        // a triangle whose vertices all missed -- which is what makes the switch
        // safe to flip in the middle of a frame.
        int sh = (a.HasSub | b.HasSub | c.HasSub) ? SubBits : 0;
        int ax = Fix(a.X, a.Fx, sh), ay = Fix(a.Y, a.Fy, sh);
        int bx = Fix(b.X, b.Fx, sh), by = Fix(b.Y, b.Fy, sh);
        int cx = Fix(c.X, c.Fx, sh), cy = Fix(c.Y, c.Fy, sh);

        long area = (long)(bx - ax) * (cy - ay) - (long)(by - ay) * (cx - ax);
        if (area == 0) return;
        if (area < 0) { (b, c) = (c, b); (bx, cx) = (cx, bx); (by, cy) = (cy, by); area = -area; }

        // A pixel is sampled at its own coordinate, as it always was; shifting the
        // bounds back down floors, so the first and last pixel whose sample point
        // lies inside the span are the ones covered.
        int minX = Math.Max(_drawAreaLeft, Math.Min(ax, Math.Min(bx, cx)) >> sh);
        int maxX = Math.Min(_drawAreaRight, Math.Max(ax, Math.Max(bx, cx)) >> sh);
        int minY = Math.Max(_drawAreaTop, Math.Min(ay, Math.Min(by, cy)) >> sh);
        int maxY = Math.Min(_drawAreaBottom, Math.Max(ay, Math.Max(by, cy)) >> sh);
        if (minX > maxX || minY > maxY) return;

        // Still the smallest possible nudge, since it is applied to the edge
        // function rather than to a coordinate: it breaks an exact tie on a shared
        // edge and nothing else, at either shift.
        int bias0 = IsTopLeft(bx, by, cx, cy) ? 0 : -1;
        int bias1 = IsTopLeft(cx, cy, ax, ay) ? 0 : -1;
        int bias2 = IsTopLeft(ax, ay, bx, by) ? 0 : -1;
        bool ditherTex = _dither && !raw;

        // All three or none: a triangle that mixed a recovered W with a fallback of
        // 1 would shear along the vertex that missed. U/W, V/W and 1/W are all
        // linear in screen space, so the barycentrics below interpolate those and
        // the divide per pixel undoes it.
        bool persp = tex && a.HasW && b.HasW && c.HasW;
        float ia = 0f, ib = 0f, ic = 0f, au = 0f, av = 0f, bu = 0f, bv = 0f, cu = 0f, cv = 0f;
        if (persp)
        {
            ia = 1f / a.W; ib = 1f / b.W; ic = 1f / c.W;
            au = a.U * ia; av = a.V * ia;
            bu = b.U * ib; bv = b.V * ib;
            cu = c.U * ic; cv = c.V * ic;
        }

        // Same all-or-nothing rule for the Z-buffer: a corner left at ndc z = 0
        // among two real depths would punch a hole through the triangle. 1/Z is
        // linear in screen space for a plane, so the pixel's view depth is the
        // reciprocal of the interpolated inverses — whether or not the texture
        // is also being divided.
        bool useZ = !Detached && GteDepth.ZBuffer && a.HasZ && b.HasZ && c.HasZ;
        float za = 0f, zb = 0f, zc = 0f;
        if (useZ)
        {
            GteDepth.ZTris++;
            EnsureZClear();
            za = 1f / a.W; zb = 1f / b.W; zc = 1f / c.W;
        }
        else if (GteDepth.ZBuffer && !Detached) GteDepth.ZSkipped++;

        // The steps are per pixel and the coordinates are per sixteenth, so a step
        // is one edge-function derivative times the width of a pixel.
        int sx0 = (by - cy) << sh, sy0 = (cx - bx) << sh;
        int sx1 = (cy - ay) << sh, sy1 = (ax - cx) << sh;
        int sx2 = (ay - by) << sh, sy2 = (bx - ax) << sh;

        long px = (long)minX << sh, py = (long)minY << sh;
        long w0Row = (long)(cx - bx) * (py - by) - (long)(cy - by) * (px - bx);
        long w1Row = (long)(ax - cx) * (py - cy) - (long)(ay - cy) * (px - cx);
        long w2Row = (long)(bx - ax) * (py - ay) - (long)(by - ay) * (px - ax);

        for (int y = minY; y <= maxY; y++, w0Row += sy0, w1Row += sy1, w2Row += sy2)
        {
            long w0 = w0Row, w1 = w1Row, w2 = w2Row;
            for (int x = minX; x <= maxX; x++, w0 += sx0, w1 += sx1, w2 += sx2)
            {
                if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
                if (Detached) Frag(x, y);

                int zi = 0;
                float zpix = 0f;
                if (useZ)
                {
                    float inv = (w0 * za + w1 * zb + w2 * zc) / area;
                    if (inv <= 0f) continue;
                    zpix = 1f / inv;
                    zi = y * VramWidth + x;
                    // 0051. Within the tolerance the later table entry wins.
                    if (zpix - GteDepth.DepthBias > _zbuf![zi]) { GteDepth.ZRejects++; continue; }
                }

                int r, g, bl;
                if (gouraud)
                {
                    r = (int)((w0 * a.R + w1 * b.R + w2 * c.R) / area);
                    g = (int)((w0 * a.G + w1 * b.G + w2 * c.G) / area);
                    bl = (int)((w0 * a.B + w1 * b.B + w2 * c.B) / area);
                }
                else { r = a.R; g = a.G; bl = a.B; }

                bool wrote;
                if (tex)
                {
                    int u, tv;
                    if (persp)
                    {
                        float iw = w0 * ia + w1 * ib + w2 * ic;
                        u = (int)((w0 * au + w1 * bu + w2 * cu) / iw);
                        tv = (int)((w0 * av + w1 * bv + w2 * cv) / iw);
                    }
                    else
                    {
                        u = (int)((w0 * a.U + w1 * b.U + w2 * c.U) / area);
                        tv = (int)((w0 * a.V + w1 * b.V + w2 * c.V) / area);
                    }
                    ushort texel = FetchTexel(u, tv, clut);
                    if (texel == 0) continue;
                    bool stp = (texel & 0x8000) != 0;
                    int tr = (texel & 0x1F) << 3, tg = ((texel >> 5) & 0x1F) << 3, tb = ((texel >> 10) & 0x1F) << 3;
                    if (!raw) { tr = tr * r >> 7; tg = tg * g >> 7; tb = tb * bl >> 7; }
                    wrote = Plot(x, y, tr, tg, tb, semi && stp, ditherTex, stp);
                }
                else wrote = Plot(x, y, r, g, bl, semi, _dither && gouraud);

                // Punch-through (texel 0) and a mask-bit reject both skip the write,
                // so a hole in the texture does not occlude whatever is behind it.
                // Semi-transparent tests against Z but does not write it, so two
                // overlapping additives still blend in table order.
                if (wrote && useZ && !semi && zpix < _zbuf![zi]) _zbuf[zi] = zpix;
            }
        }
    }

    // Takes coordinates rather than vertices because by this point the triangle is
    // in the rasterizer's own units, which may be sixteenths of a pixel.
    static bool IsTopLeft(int x0, int y0, int x1, int y1)
    {
        int dy = y1 - y0, dx = x1 - x0;
        return dy < 0 || (dy == 0 && dx > 0);
    }

    void DrawRectangle()
    {
        uint cmd = _fifo[0];
        int sz = (int)((cmd >> 27) & 3);
        bool tex = (cmd & (1u << 26)) != 0;
        bool semi = (cmd & (1u << 25)) != 0;
        bool raw = (cmd & (1u << 24)) != 0;
        int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);

        int idx = 1;
        uint vw = _fifo[idx++];
        int x = _drawOffsetX + CoordX(vw);
        int y = _drawOffsetY + CoordY(vw);

        int u0 = 0, v0 = 0, clut = 0;
        if (tex)
        {
            uint uvw = _fifo[idx++];
            u0 = (int)(uvw & 0xFF); v0 = (int)((uvw >> 8) & 0xFF);
            clut = (int)((uvw >> 16) & 0xFFFF);
        }

        int w, h;
        if (sz == 0) { uint wh = _fifo[idx]; w = (int)(wh & 0xFFFF); h = (int)((wh >> 16) & 0xFFFF); }
        else { w = h = sz == 1 ? 1 : sz == 2 ? 8 : 16; }
        //dispatch event
        if (!Detached) Hle.GpuHle.PortWidenedPrim = false;
        if (!Detached && Event.HasAnyListeners<RenderPrimEvent>())
        {
            var e = _primEvent;
            e.Context = Runtime.Cpu!; e.Memory = Runtime.Mem!;
            e.Count = 2;
            e.X[0] = x; e.X[1] = x + w; e.Y[0] = y; e.Y[1] = y + h;
            e.DrawLeft = _drawAreaLeft; e.DrawRight = _drawAreaRight; e.DrawTop = _drawAreaTop; e.DrawBottom = _drawAreaBottom;
            e.Textured = tex; e.SemiTransparent = semi; e.Gouraud = false; e.Raw = raw; e.Clut = clut; e.TexPage = 0; e.Skip = false;
            Event.Dispatch(e);
            if (e.Skip) return;
            x = e.X[0]; w = e.X[1] - e.X[0];
        }
        if (HleOn) { HleRect(x, y, w, h, u0, v0, clut, cr, cg, cb, tex, semi, raw); return; }

        for (int dy = 0; dy < h; dy++)
            for (int dx = 0; dx < w; dx++)
            {
                int px = x + dx, py = y + dy;
                if (px < _drawAreaLeft || px > _drawAreaRight || py < _drawAreaTop || py > _drawAreaBottom) continue;
                if (Detached && (uint)px < VramWidth && (uint)py < VramHeight) Frag(px, py);
                if (tex)
                {
                    ushort texel = FetchTexel((u0 + dx) & 0xFF, (v0 + dy) & 0xFF, clut);
                    if (texel == 0) continue;
                    bool stp = (texel & 0x8000) != 0;
                    int tr = (texel & 0x1F) << 3, tg = ((texel >> 5) & 0x1F) << 3, tb = ((texel >> 10) & 0x1F) << 3;
                    if (!raw) { tr = tr * cr >> 7; tg = tg * cg >> 7; tb = tb * cb >> 7; }
                    Plot(px, py, tr, tg, tb, semi && stp, false, stp);
                }
                else Plot(px, py, cr, cg, cb, semi, false);
            }
    }

    void DrawLine()
    {
        uint cmd = _fifo[0];
        bool gouraud = (cmd & (1u << 28)) != 0;
        bool semi = (cmd & (1u << 25)) != 0;
        int idx = 1;

        int r0 = (int)(cmd & 0xFF), g0 = (int)((cmd >> 8) & 0xFF), b0 = (int)((cmd >> 16) & 0xFF);
        uint v0w = _fifo[idx++];
        int r1 = r0, g1 = g0, b1 = b0;
        if (gouraud) { uint cw = _fifo[idx++]; r1 = (int)(cw & 0xFF); g1 = (int)((cw >> 8) & 0xFF); b1 = (int)((cw >> 16) & 0xFF); }
        uint v1w = _fifo[idx++];

        LineSegment(CoordX(v0w), CoordY(v0w), r0, g0, b0, CoordX(v1w), CoordY(v1w), r1, g1, b1, semi, gouraud);
    }

    void ExecutePolyline()
    {
        uint cmd = _fifo[0];
        bool gouraud = (cmd & (1u << 28)) != 0;
        bool semi = (cmd & (1u << 25)) != 0;

        var pts = new List<(int X, int Y, int R, int G, int B)>();
        int idx = 1;
        int r = (int)(cmd & 0xFF), g = (int)((cmd >> 8) & 0xFF), b = (int)((cmd >> 16) & 0xFF);
        bool first = true;
        while (idx < _fifoCount)
        {
            if (gouraud && !first) { uint cw = _fifo[idx++]; r = (int)(cw & 0xFF); g = (int)((cw >> 8) & 0xFF); b = (int)((cw >> 16) & 0xFF); }
            if (idx >= _fifoCount) break;
            uint vw = _fifo[idx++];
            pts.Add((CoordX(vw), CoordY(vw), r, g, b));
            first = false;
        }

        for (int i = 0; i + 1 < pts.Count; i++)
            LineSegment(pts[i].X, pts[i].Y, pts[i].R, pts[i].G, pts[i].B,
                        pts[i + 1].X, pts[i + 1].Y, pts[i + 1].R, pts[i + 1].G, pts[i + 1].B, semi, gouraud);
    }

    void LineSegment(int x0, int y0, int r0, int g0, int b0, int x1, int y1, int r1, int g1, int b1, bool semi, bool gouraud)
    {
        // Lines pass neither RenderPrimEvent dispatch, so nothing clears the
        // widescreen patch's widened-prim flag for them; clear it here.
        if (!Detached) Hle.GpuHle.PortWidenedPrim = false;
        x0 += _drawOffsetX; y0 += _drawOffsetY;
        x1 += _drawOffsetX; y1 += _drawOffsetY;
        if (HleOn) { HleLine(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, semi, gouraud); return; }
        int dx = Math.Abs(x1 - x0), dy = Math.Abs(y1 - y0);
        int steps = Math.Max(dx, dy);
        if (steps == 0) { Plot(x0, y0, r0, g0, b0, semi, _dither); return; }
        for (int i = 0; i <= steps; i++)
        {
            double t = (double)i / steps;
            int x = (int)Math.Round(x0 + (x1 - x0) * t);
            int y = (int)Math.Round(y0 + (y1 - y0) * t);
            int r = (int)(r0 + (r1 - r0) * t);
            int g = (int)(g0 + (g1 - g0) * t);
            int b = (int)(b0 + (b1 - b0) * t);
            if (x < _drawAreaLeft || x > _drawAreaRight || y < _drawAreaTop || y > _drawAreaBottom) continue;
            if (Detached && (uint)x < VramWidth && (uint)y < VramHeight) Frag(x, y);
            Plot(x, y, r, g, b, semi, _dither);
        }
    }

    ushort FetchTexel(int u, int v, int clut)
    {
        u = (u & ~(_texWinMaskX * 8)) | ((_texWinOffX & _texWinMaskX) * 8);
        v = (v & ~(_texWinMaskY * 8)) | ((_texWinOffY & _texWinMaskY) * 8);
        u &= 0xFF; v &= 0xFF;

        int row = (_texPageY + v) & (VramHeight - 1);
        if (_texDepth == 2 || _texDepth == 3)
            return Vram[row * VramWidth + ((_texPageX + u) & (VramWidth - 1))];

        int clutX = (clut & 0x3F) * 16;
        int clutY = (clut >> 6) & 0x1FF;
        int index;
        if (_texDepth == 0)
        {
            ushort block = Vram[row * VramWidth + ((_texPageX + (u >> 2)) & (VramWidth - 1))];
            index = (block >> ((u & 3) * 4)) & 0xF;
        }
        else
        {
            ushort block = Vram[row * VramWidth + ((_texPageX + (u >> 1)) & (VramWidth - 1))];
            index = (block >> ((u & 1) * 8)) & 0xFF;
        }
        return Vram[(clutY & (VramHeight - 1)) * VramWidth + ((clutX + index) & (VramWidth - 1))];
    }

    bool Plot(int x, int y, int r, int g, int b, bool semi, bool dither, bool maskBit = false)
    {
        if (x < _drawAreaLeft || x > _drawAreaRight || y < _drawAreaTop || y > _drawAreaBottom) return false;
        if (x < 0 || x >= VramWidth || y < 0 || y >= VramHeight) return false;

        int idx = y * VramWidth + x;
        ushort bg = Vram[idx];
        if (_checkMask && (bg & 0x8000) != 0) return false;

        if (dither)
        {
            int d = Dither[y & 3, x & 3];
            r = Clamp255(r + d); g = Clamp255(g + d); b = Clamp255(b + d);
        }

        int fr = Math.Min(31, r >> 3), fg = Math.Min(31, g >> 3), fb = Math.Min(31, b >> 3);

        if (semi)
        {
            int br = bg & 0x1F, bgn = (bg >> 5) & 0x1F, bbl = (bg >> 10) & 0x1F;
            switch (_blendMode)
            {
                case 0: fr = (br + fr) >> 1; fg = (bgn + fg) >> 1; fb = (bbl + fb) >> 1; break;
                case 1: fr = Math.Min(31, br + fr); fg = Math.Min(31, bgn + fg); fb = Math.Min(31, bbl + fb); break;
                case 2: fr = Math.Max(0, br - fr); fg = Math.Max(0, bgn - fg); fb = Math.Max(0, bbl - fb); break;
                default: fr = Math.Min(31, br + (fr >> 2)); fg = Math.Min(31, bgn + (fg >> 2)); fb = Math.Min(31, bbl + (fb >> 2)); break;
            }
        }

        ushort outp = (ushort)(fr | (fg << 5) | (fb << 10));
        if (_setMask || maskBit) outp |= 0x8000;
        Vram[idx] = outp;
        if (Owner != null) Owner[idx] = CoverTag;
        return true;
    }

    float _lastDepthMean;

    // The scene-break test. It has to run before the primitive is submitted and
    // after the batch it would join, so the GL backend is flushed first: the clear
    // happens inside the next batch's flush, and a batch still holding this frame's
    // earlier triangles would otherwise be cleared out from under them.
    void MaybeClearDepth(ReadOnlySpan<Vert> v, int n)
    {
        if (!GteDepth.ZBuffer) return;

        // The threshold is read after the census, not before it: at zero the clear
        // must not fire, but the drops still have to be counted or the one setting
        // that turns the guard off also turns off the measurement that would say
        // whether it should be off.
        float thr = GteDepth.DepthClearThreshold;

        float sum = 0f;
        int c = 0;
        for (int i = 0; i < n; i++)
            if (v[i].HasZ) { sum += v[i].W; c++; }

        if (c == 0) return;

        float mean = sum / c;
        float drop = _lastDepthMean - mean;

        // Every drop is censused, fired on or not, because the question the
        // threshold answers is whether this game *has* two populations of drop --
        // ordinary depth sorting within one scene, and a scene the game began
        // afresh -- or only the first, in which case any threshold that fires is
        // throwing away the world's own depth mid-frame.
        if (drop > 0f)
        {
            if (drop > GteDepth.ZDropMax) GteDepth.ZDropMax = drop;
            int bucket = drop < 10f ? 0 : drop < 50f ? 1 : drop < 150f ? 2
                       : drop < 300f ? 3 : drop < 1000f ? 4 : 5;
            GteDepth.ZDrops[bucket]++;
        }

        if (thr > 0f && drop >= thr)
        {
            if (HleOn) Hle.GpuHle.Backend?.Flush();
            GteDepth.Generation++;
            GteDepth.ZClears++;
        }

        _lastDepthMean = mean;
    }

    void EnsureZClear()
    {
        _zbuf ??= new float[VramWidth * VramHeight];
        if (_zClipLeft == _drawAreaLeft && _zClipTop == _drawAreaTop
            && _zClipRight == _drawAreaRight && _zClipBottom == _drawAreaBottom
            && _zGen == GteDepth.Generation)
            return;

        int x0 = Math.Max(0, _drawAreaLeft), x1 = Math.Min(VramWidth - 1, _drawAreaRight);
        int y0 = Math.Max(0, _drawAreaTop), y1 = Math.Min(VramHeight - 1, _drawAreaBottom);
        for (int y = y0; y <= y1; y++)
            Array.Fill(_zbuf, float.MaxValue, y * VramWidth + x0, x1 - x0 + 1);
        _zClipLeft = _drawAreaLeft; _zClipTop = _drawAreaTop;
        _zClipRight = _drawAreaRight; _zClipBottom = _drawAreaBottom;
        _zGen = GteDepth.Generation;
    }

    void ClearZ(int x, int y, int w, int h)
    {
        if (_zbuf == null) return;
        int x0 = Math.Max(0, x), x1 = Math.Min(VramWidth, x + w);
        int y0 = Math.Max(0, y), y1 = Math.Min(VramHeight, y + h);
        if (x0 >= x1 || y0 >= y1) return;
        for (int py = y0; py < y1; py++)
            Array.Fill(_zbuf, float.MaxValue, py * VramWidth + x0, x1 - x0);
        // A fill of the current clip is itself the frame clear, so the next
        // triangle must not think the clip is still virgin from a previous buffer.
        if (x0 <= _drawAreaLeft && y0 <= _drawAreaTop && x1 > _drawAreaRight && y1 > _drawAreaBottom)
        {
            _zClipLeft = _drawAreaLeft; _zClipTop = _drawAreaTop;
            _zClipRight = _drawAreaRight; _zClipBottom = _drawAreaBottom;
            _zGen = GteDepth.Generation;
        }
        else _zClipRight = int.MinValue;
    }

    void SetTexpageFromWord(uint tp)
    {
        _texPageX = (int)(tp & 0xF) * 64;
        _texPageY = (int)((tp >> 4) & 1) * 256;
        _blendMode = (int)((tp >> 5) & 3);
        _texDepth = (int)((tp >> 7) & 3);
        _texDisable = (tp & (1u << 11)) != 0;
    }

    // PGXP asks two places in order. The RAM shadow is the exact answer -- this
    // word, at this address, put there by a tracked store -- and it is what carries
    // a vertex through the game's own copy out of its transform cache and into the
    // packet. The screen-position cache underneath it is the inexact one, kept for
    // the vertices no tracked instruction ever touched; it can only offer a W when
    // the "cache W" option says its stand-in is worth having.
    void ApplyPgxp(Span<Vert> v, ReadOnlySpan<int> vwAt, int n, bool wantW, bool wantZ, bool wantSub)
    {
        Span<uint> words = stackalloc uint[4];
        Span<uint> seqs = stackalloc uint[4];

        for (int i = 0; i < n; i++)
        {
            uint word = _fifo[vwAt[i]];
            words[i] = word;

            float px = 0f, py = 0f, pw = 1f;
            bool validW = false;
            uint seq = 0u;

            Pgxp.PgxpStats.Asked++;

            uint src = _fifoSrc[vwAt[i]];
            bool found = src != 0u &&
                         Pgxp.PgxpMemory.TryLoad(src, word, out px, out py, out pw, out validW, out seq, out _);
            bool fromMemory = found;

            if (!found)
                found = Pgxp.PgxpGpu.TryGetVertex(word, 0u, false, out px, out py, out pw, out validW, out seq, out _);

            if (!found) continue;

            if (!WithinTolerance(px, py, word)) { Pgxp.PgxpStats.Refused++; continue; }

            if (fromMemory) Pgxp.PgxpStats.FromMemory++; else Pgxp.PgxpStats.FromCache++;
            if (!validW || pw <= 0f) Pgxp.PgxpStats.NoDepth++;

            v[i].Precise = true;
            v[i].PreciseW = validW;
            v[i].Px = px;
            v[i].Py = py;
            v[i].W = pw;
            seqs[i] = seq;
        }

        ResolveAmbiguous(v, words, seqs, n);

        for (int i = 0; i < n; i++)
        {
            if (!v[i].Precise) continue;

            bool depth = v[i].PreciseW && v[i].W > 0f;
            v[i].HasW = wantW && depth;
            v[i].HasZ = wantZ && depth;

            // Downstream keeps the packet's whole-pixel position and adds a
            // fraction to it, so that a RenderPrimEvent listener -- widescreen --
            // still sees the coordinates the packet carried and can move them.
            // PGXP produces an absolute position instead, so the fraction is the
            // difference. It is only meaningful where the packet word and the
            // 11-bit coordinate the GPU decodes are the same number; a game that
            // wrote something else into the top bits is not describing this vertex.
            if (!wantSub) continue;

            int rawX = CoordX(words[i]), rawY = CoordY(words[i]);
            if (rawX != (short)(words[i] & 0xFFFFu) || rawY != (short)(words[i] >> 16)) continue;

            v[i].Fx = v[i].Px - rawX;
            v[i].Fy = v[i].Py - rawY;
            v[i].HasSub = true;
        }
    }

    // Upstream defines a tolerance and never spends it. It is the guard this arm
    // wants: a recovered position that disagrees with the packet by more than a
    // couple of pixels is not a more precise version of this vertex, it is a
    // different vertex, and believing it moves geometry. Negative turns it off.
    static bool WithinTolerance(float px, float py, uint word)
    {
        float dx = px - CoordX(word), dy = py - CoordY(word);
        Pgxp.PgxpStats.NoteDisagreement(dx, dy);

        float tol = Pgxp.Pgxp.Tolerance;
        if (tol < 0f) return true;

        return Math.Abs(dx) < tol && Math.Abs(dy) < tol;
    }

    // A primitive that recovered some of its vertices and not others is the case
    // the vertex cache exists for, and the sequence number of a vertex that *was*
    // recovered says which of an ambiguous cell's candidates belongs to the same
    // batch. A primitive that recovered none of them, or all of them, has nothing
    // to disambiguate with or nothing left to ask.
    void ResolveAmbiguous(Span<Vert> v, ReadOnlySpan<uint> words, ReadOnlySpan<uint> seqs, int n)
    {
        uint hint = 0u;
        int resolved = 0;

        for (int i = 0; i < n; i++)
        {
            if (!v[i].Precise) continue;
            if (resolved == 0) hint = seqs[i];
            resolved++;
        }

        if (resolved == 0 || resolved == n) return;

        for (int i = 0; i < n; i++)
        {
            if (v[i].Precise) continue;
            if (!Pgxp.PgxpGpu.TryGetVertex(words[i], hint, true, out float px, out float py, out float pw,
                    out bool validW, out _, out _)) continue;
            if (!WithinTolerance(px, py, words[i])) { Pgxp.PgxpStats.Refused++; continue; }

            Pgxp.PgxpStats.Resolved++;
            if (!validW || pw <= 0f) Pgxp.PgxpStats.NoDepth++;

            v[i].Precise = true;
            v[i].PreciseW = validW;
            v[i].Px = px;
            v[i].Py = py;
            v[i].W = pw;
        }
    }

    static int CoordX(uint w) { int x = (int)(w & 0x7FF); return (x & 0x400) != 0 ? x - 0x800 : x; }
    static int CoordY(uint w) { int y = (int)((w >> 16) & 0x7FF); return (y & 0x400) != 0 ? y - 0x800 : y; }
    static ushort To15(int r, int g, int b) => (ushort)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
    static int Clamp255(int v) => v < 0 ? 0 : v > 255 ? 255 : v;
}
