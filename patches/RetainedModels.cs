using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// The frame's models in the retained scene: every face the lit assembler is handed
/// inside the object walk, taken back to world space before the assembler culls a
/// single one (a reflection sees the side the camera does not), and handed to
/// <see cref="RetainedScene.AddDynamic"/>.
///
/// The model's corners are read from the vertex base the transform just read them
/// from, so an animated pose is the pose drawn. The GTE still holds the model's
/// rotation and translation into view space; the frame's camera takes that back to
/// the world. The colour is the same `NormalColorCol` the assembler's
/// `NormalColorDpq` makes, without the depth cue, which the vertex shader applies
/// from the reflection's own camera with the DQA, DQB and curve the model was
/// fogged with.
///
/// Not captured: the billboards and effects drawn by other assemblers, and anything
/// drawn outside the object walk (the arm, the HUD).
/// </summary>
static class RetainedModels
{
    const uint VertexBase = 0x8018EAA0;
    const uint VertexCache = 0x8018EB94;
    const uint LightColour = 0x8006E604;
    const uint FogMode = 0x80192EA8;

    static RetainedScene.Vertex[] _tris = new RetainedScene.Vertex[1024];

    /// <summary>Whether the lit assembler should hand its faces over.</summary>
    public static bool Capturing => RetainedMap.Ready && ModelWalk.InWalk && !PolyAssembler.Verifying;

    public static long Models, Faces;

    /// <summary>One model: its header, normals and faces as the lit assembler found
    /// them; <paramref name="abr"/> the blend mode a blended submit forces, or
    /// <see cref="uint.MaxValue"/> for an opaque one.</summary>
    public static void Capture(PSMemory mem, uint header, uint normals, uint face, uint count, uint abr)
    {
        var frame = RetainedScene.Find(RetainedScene.Serial);
        if (frame == null || count > 4096) return;
        var v = frame.View;

        // The model's view transform, as the GTE holds it.
        uint r0 = Gte.ReadControl(0), r1 = Gte.ReadControl(1), r2 = Gte.ReadControl(2), r3 = Gte.ReadControl(3);
        double m00 = (short)r0 / 4096.0, m01 = (short)(r0 >> 16) / 4096.0, m02 = (short)r1 / 4096.0;
        double m10 = (short)(r1 >> 16) / 4096.0, m11 = (short)r2 / 4096.0, m12 = (short)(r2 >> 16) / 4096.0;
        double m20 = (short)r3 / 4096.0, m21 = (short)(r3 >> 16) / 4096.0, m22 = (short)Gte.ReadControl(4) / 4096.0;
        double tx = (int)Gte.ReadControl(5), ty = (int)Gte.ReadControl(6), tz = (int)Gte.ReadControl(7);
        float dqa = (short)Gte.ReadControl(27), dqb = (int)Gte.ReadControl(28);
        int mode = (int)mem.ReadU32(FogMode);
        float curve = mode >= 32000 ? 0f : (mode & 0x8000) != 0 ? 1f : 2f;
        uint rgbc = mem.ReadU32(LightColour);
        uint verts = mem.ReadU32(VertexBase);
        byte mat = PolyAssembler.TileMaterial;

        int n = 0;
        Span<uint> idx = stackalloc uint[4], nrm = stackalloc uint[4], uv = stackalloc uint[4];
        Span<float> wx = stackalloc float[4], wy = stackalloc float[4], wz = stackalloc float[4];
        Span<uint> col = stackalloc uint[4];
        for (uint i = 0; i < count; i++)
        {
            uint word = mem.ReadU32(face);
            uint f = face + 4u;
            uint cmd = word >> 24;
            int corners;
            bool gouraud;
            switch (cmd & 0xFDu)
            {
                case 0x24u: corners = 3; gouraud = false; idx[0] = 0x0E; idx[1] = 0x10; idx[2] = 0x12; nrm[0] = 0x0C; break;
                case 0x2Cu: corners = 4; gouraud = false; idx[0] = 0x12; idx[1] = 0x14; idx[2] = 0x16; idx[3] = 0x18; nrm[0] = 0x10; break;
                case 0x34u: corners = 3; gouraud = true; idx[0] = 0x0E; idx[1] = 0x12; idx[2] = 0x16; nrm[0] = 0x0C; nrm[1] = 0x10; nrm[2] = 0x14; break;
                case 0x3Cu: corners = 4; gouraud = true; idx[0] = 0x12; idx[1] = 0x16; idx[2] = 0x1A; idx[3] = 0x1E; nrm[0] = 0x10; nrm[1] = 0x14; nrm[2] = 0x18; nrm[3] = 0x1C; break;
                default: corners = 0; gouraud = false; break;
            }
            face = f + ((word >> 6) & 0x3FCu);
            if (corners == 0) continue;

            for (int k = 0; k < corners; k++)
            {
                uint p = verts + mem.ReadU16(f + idx[k]);
                double x = (short)mem.ReadU16(p), y = (short)mem.ReadU16(p + 2u), z = (short)mem.ReadU16(p + 4u);
                double vx = m00 * x + m01 * y + m02 * z + tx - v.Tx;
                double vy = m10 * x + m11 * y + m12 * z + ty - v.Ty;
                double vz = m20 * x + m21 * y + m22 * z + tz - v.Tz;
                // The frame's R is a rotation: its transpose takes view back to world.
                wx[k] = (float)(v.R00 * vx + v.R10 * vy + v.R20 * vz + v.CamX);
                wy[k] = (float)(v.R01 * vx + v.R11 * vy + v.R21 * vz + v.CamY);
                wz[k] = (float)(v.R02 * vx + v.R12 * vy + v.R22 * vz + v.CamZ);
                if (RetainedMap.Checking) RetainedMap.CheckCorner(v, wx[k], wy[k], wz[k], mem.ReadU32(VertexCache + mem.ReadU16(f + idx[k])));
                if (gouraud || k == 0) col[k] = Light(mem, normals + mem.ReadU16(f + nrm[gouraud ? k : 0]), rgbc);
                else col[k] = col[0];
            }
            uv[0] = mem.ReadU16(f);
            uv[1] = mem.ReadU16(f + 4u);
            uv[2] = mem.ReadU16(f + 8u);
            uv[3] = corners == 4 ? mem.ReadU16(f + 0xCu) : 0u;
            uint clut = mem.ReadU16(f + 2u), tpage = mem.ReadU16(f + 6u);
            bool semi = abr != uint.MaxValue || (cmd & 2u) != 0;
            if (abr != uint.MaxValue) tpage = (tpage & 0xFF9Fu) | abr;
            tpage &= 0x1FFu;

            int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
            for (int k = 0; k < corners; k++)
            {
                int u = (int)(uv[k] & 0xFF), vv = (int)(uv[k] >> 8);
                u0 = Math.Min(u0, u); v0 = Math.Min(v0, vv); u1 = Math.Max(u1, u); v1 = Math.Max(v1, vv);
            }
            var t = new RetainedScene.Vertex
            {
                Clut = clut & 0x7FFF, Texpage = tpage, Dqa = dqa, Dqb = dqb, Curve = curve,
                Rect = (uint)u0 | (uint)v0 << 8 | (uint)u1 << 16 | (uint)v1 << 24,
                Flags = RetainedScene.FlagRect | mat | (semi ? RetainedScene.FlagSemi | ((tpage >> 5) & 3u) << 8 : 0u),
            };
            if (n + 6 > _tris.Length) Array.Resize(ref _tris, _tris.Length * 2);
            Put(ref n, t, wx, wy, wz, uv, col, 0); Put(ref n, t, wx, wy, wz, uv, col, 1); Put(ref n, t, wx, wy, wz, uv, col, 2);
            if (corners == 4) { Put(ref n, t, wx, wy, wz, uv, col, 1); Put(ref n, t, wx, wy, wz, uv, col, 3); Put(ref n, t, wx, wy, wz, uv, col, 2); }
            Faces++;
        }
        RetainedScene.AddDynamic(_tris.AsSpan(0, n));
        Models++;
    }

    /// <summary>One model the object walk hands the tile assemblers
    /// (`func_80030540`, `func_8002FECC`): the map's face format, lit flat per face
    /// by whatever light matrix the submitter set. <paramref name="twoCurves"/> is
    /// the clipped assembler's transform, whose fog has no offset curve.</summary>
    public static void CaptureFlat(PSMemory mem, uint header, uint normals, uint face, uint count, bool twoCurves)
    {
        var frame = RetainedScene.Find(RetainedScene.Serial);
        if (frame == null || count > 4096) return;
        var v = frame.View;
        uint r0 = Gte.ReadControl(0), r1 = Gte.ReadControl(1), r2 = Gte.ReadControl(2), r3 = Gte.ReadControl(3);
        double m00 = (short)r0 / 4096.0, m01 = (short)(r0 >> 16) / 4096.0, m02 = (short)r1 / 4096.0;
        double m10 = (short)(r1 >> 16) / 4096.0, m11 = (short)r2 / 4096.0, m12 = (short)(r2 >> 16) / 4096.0;
        double m20 = (short)r3 / 4096.0, m21 = (short)(r3 >> 16) / 4096.0, m22 = (short)Gte.ReadControl(4) / 4096.0;
        double tx = (int)Gte.ReadControl(5), ty = (int)Gte.ReadControl(6), tz = (int)Gte.ReadControl(7);
        float dqa = (short)Gte.ReadControl(27), dqb = (int)Gte.ReadControl(28);
        int mode = (int)mem.ReadU32(FogMode);
        float curve = mode >= 32000 ? 0f : twoCurves ? 2f : (mode & 0x8000) != 0 ? 1f : 2f;
        uint rgbc = mem.ReadU32(LightColour);
        uint verts = mem.ReadU32(VertexBase);
        byte mat = PolyAssembler.TileMaterial;

        int n = 0;
        Span<uint> uv = stackalloc uint[4], col = stackalloc uint[4];
        Span<float> wx = stackalloc float[4], wy = stackalloc float[4], wz = stackalloc float[4];
        for (uint i = 0; i < count; i++)
        {
            uint word = mem.ReadU32(face);
            uint f = face + 4u;
            uint cmd = word >> 24;
            uint type = cmd & 0xFDu;
            face = f + ((word >> 6) & 0x3FCu);
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            if (corners == 0) continue;
            uint idx = f + (corners == 4 ? 0x12u : 0x0Eu);
            for (int k = 0; k < corners; k++)
            {
                uint off = mem.ReadU16(idx + (uint)k * 2u);
                uint p = verts + off;
                double x = (short)mem.ReadU16(p), y = (short)mem.ReadU16(p + 2u), z = (short)mem.ReadU16(p + 4u);
                double vx = m00 * x + m01 * y + m02 * z + tx - v.Tx;
                double vy = m10 * x + m11 * y + m12 * z + ty - v.Ty;
                double vz = m20 * x + m21 * y + m22 * z + tz - v.Tz;
                wx[k] = (float)(v.R00 * vx + v.R10 * vy + v.R20 * vz + v.CamX);
                wy[k] = (float)(v.R01 * vx + v.R11 * vy + v.R21 * vz + v.CamY);
                wz[k] = (float)(v.R02 * vx + v.R12 * vy + v.R22 * vz + v.CamZ);
                if (RetainedMap.Checking) RetainedMap.CheckCorner(v, wx[k], wy[k], wz[k], mem.ReadU32(VertexCache + off));
            }
            uint lit = Light(mem, normals + mem.ReadU16(f + (corners == 4 ? 0x10u : 0x0Cu)), rgbc);
            for (int k = 0; k < corners; k++) col[k] = lit;
            uv[0] = mem.ReadU16(f);
            uv[1] = mem.ReadU16(f + 4u);
            uv[2] = mem.ReadU16(f + 8u);
            uv[3] = corners == 4 ? mem.ReadU16(f + 0xCu) : 0u;
            uint clut = mem.ReadU16(f + 2u), tpage = mem.ReadU16(f + 6u) & 0x1FFu;
            bool semi = (cmd & 2u) != 0;
            int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
            for (int k = 0; k < corners; k++)
            {
                int u = (int)(uv[k] & 0xFF), vv = (int)(uv[k] >> 8);
                u0 = Math.Min(u0, u); v0 = Math.Min(v0, vv); u1 = Math.Max(u1, u); v1 = Math.Max(v1, vv);
            }
            var t = new RetainedScene.Vertex
            {
                Clut = clut & 0x7FFF, Texpage = tpage, Dqa = dqa, Dqb = dqb, Curve = curve,
                Rect = (uint)u0 | (uint)v0 << 8 | (uint)u1 << 16 | (uint)v1 << 24,
                Flags = RetainedScene.FlagRect | mat | (semi ? RetainedScene.FlagSemi | ((tpage >> 5) & 3u) << 8 : 0u),
            };
            if (n + 6 > _tris.Length) Array.Resize(ref _tris, _tris.Length * 2);
            Put(ref n, t, wx, wy, wz, uv, col, 0); Put(ref n, t, wx, wy, wz, uv, col, 1); Put(ref n, t, wx, wy, wz, uv, col, 2);
            if (corners == 4) { Put(ref n, t, wx, wy, wz, uv, col, 1); Put(ref n, t, wx, wy, wz, uv, col, 3); Put(ref n, t, wx, wy, wz, uv, col, 2); }
            Faces++;
        }
        RetainedScene.AddDynamic(_tris.AsSpan(0, n));
        Models++;
    }

    static void Put(ref int n, in RetainedScene.Vertex t, Span<float> x, Span<float> y, Span<float> z,
                    Span<uint> uv, Span<uint> col, int k)
    {
        var v = t;
        v.X = x[k]; v.Y = y[k]; v.Z = z[k];
        v.U = uv[k] & 0xFF; v.V = uv[k] >> 8;
        v.R = col[k] & 0xFF; v.G = (col[k] >> 8) & 0xFF; v.B = (col[k] >> 16) & 0xFF;
        _tris[n++] = v;
    }

    /// <summary>NormalColorCol: the assembler's NormalColorDpq without the cue.</summary>
    static uint Light(PSMemory mem, uint normal, uint rgbc)
    {
        Gte.Write(0, mem.ReadU32(normal));
        Gte.Write(1, mem.ReadU32(normal + 4u));
        Gte.Write(6, rgbc);
        Gte.NccsOp(12, true);
        return Gte.Read(22);
    }
}
