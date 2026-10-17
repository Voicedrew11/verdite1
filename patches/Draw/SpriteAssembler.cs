using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E230</c>, the sprite writer. <c>KF1_SPRITEASSEMBLER</c>.
/// a0 is the sprite record (its shorts at +4, +6, +8, +A give the position and
/// size), a1 a signed halfword added to the ordering-table index, and a2 == 1
/// scales the size by 3/2. The FT4 packet is written at the cursor of the record
/// at 0x80070EB8 and added to the ordering table at 0x80090EBC.
/// </summary>
public static class SpriteAssembler
{
    public static readonly Literal R = new(typeof(SpriteAssembler), "sprite writer", "SPRITEASSEMBLER", 0x8001E230, Run,
        "Writes one projected FT4 sprite: flipbooks, creatures, the HUD models.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        uint caller = c.RA;
        c.SP = c.SP - 0x88u;
        mem.WriteU32(c.SP + 0x84u, c.RA);
        mem.WriteU32(c.SP + 0x80u, c.S4);
        mem.WriteU32(c.SP + 0x7Cu, c.S3);
        mem.WriteU32(c.SP + 0x78u, c.S2);
        mem.WriteU32(c.SP + 0x74u, c.S1);
        mem.WriteU32(c.SP + 0x70u, c.S0);
        c.S2 = c.A0;
        c.S4 = c.A1;
        c.S3 = c.A2;
        c.A0 = c.SP + 0x48u;
        c.A1 = c.SP + 0x50u;
        c.V0 = mem.ReadU16(c.S2 + 0x4u);
        c.S1 = c.SP + 0x54u;
        mem.WriteU16(c.SP + 0x38u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x28u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x4u);
        c.V1 = mem.ReadU16(c.S2 + 0x8u);
        c.A2 = c.S1;
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.SP + 0x40u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x30u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x6u);
        c.S0 = c.SP + 0x58u;
        mem.WriteU16(c.SP + 0x32u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x2Au, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x6u);
        c.V1 = mem.ReadU16(c.S2 + 0xAu);
        c.A3 = c.S0;
        mem.WriteU16(c.SP + 0x44u, (ushort)0u);
        mem.WriteU16(c.SP + 0x3Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x34u, (ushort)0u);
        mem.WriteU16(c.SP + 0x2Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x4Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x4Au, (ushort)0u);
        mem.WriteU16(c.SP + 0x48u, (ushort)0u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.SP + 0x42u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x3Au, (ushort)c.V0);
        c.RA = 0x8001E2D0u;
        KingsField1_game.func_8004DA4C(c, mem);
        c.V1 = c.SP + 0x5Cu;
        mem.WriteU32(c.SP + 0x10u, c.V1);
        c.V1 = c.SP + 0x60u;
        mem.WriteU32(c.SP + 0x14u, c.V1);
        c.V1 = c.SP + 0x64u;
        mem.WriteU32(c.SP + 0x18u, c.V1);
        c.V1 = c.SP + 0x68u;
        mem.WriteU32(c.SP + 0x1Cu, c.V1);
        mem.WriteU32(c.SP + 0x20u, c.S1);
        mem.WriteU32(c.SP + 0x24u, c.S0);
        c.A0 = c.SP + 0x28u;
        c.A1 = c.SP + 0x30u;
        c.A2 = c.SP + 0x38u;
        c.A3 = c.SP + 0x40u;
        c.S1 = c.V0;
        c.RA = 0x8001E310u;
        KingsField1_game.func_8004E0B4(c, mem);
        // The packet is written at the cursor. A retained sprite writes the same packet
        // into the unlinked space past the cursor (its colour and code come from it),
        // then publishes an instance instead; the cursor stays where it was.
        uint recPtr = mem.ReadU32(0x80070EB8u);
        uint cursor = mem.ReadU32(recPtr + 0x8u), end = mem.ReadU32(recPtr + 0x4u);
        c.S0 = cursor;
        bool outOfSpace = end < cursor + 0x28u;
        if (outOfSpace) mem.WriteU32(recPtr + 0x8u, cursor + 0x28u);
        else
        {
            bool capture = GpuWorld.Capture;
            // The flipbook submitter's sprites and the creatures drawn as sprites.
            bool wantRetained = GpuWorld.SpritesOn && GpuWorld.Drawing && caller is 0x8001EE9Cu or 0x8001F004u;
            c.A0 = c.S0;
            c.RA = 0x8001E358u;
            KingsField1_game.SetPolyFT4_game(c, mem);
            c.A1 = 0x80090000u;
            c.A1 = c.A1 + 0x5058u;
            c.V0 = mem.ReadU16(c.A1);
            mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x5Cu);
            c.V1 = 0x80090000u;
            c.V1 = mem.ReadU16(c.V1 + 0x505Au);
            mem.WriteU32(c.S0 + 0x8u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x60u);
            mem.WriteU32(c.S0 + 0x10u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x64u);
            mem.WriteU32(c.S0 + 0x18u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x68u);
            mem.WriteU16(c.S0 + 0x16u, (ushort)c.V1);
            mem.WriteU32(c.S0 + 0x20u, c.V0);
            c.V0 = mem.ReadU8(c.S2);
            mem.WriteU8(c.S0 + 0x1Cu, (byte)c.V0);
            mem.WriteU8(c.S0 + 0xCu, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2);
            c.V1 = mem.ReadU8(c.S2 + 0x2u);
            c.V0 = c.V0 + c.V1;
            mem.WriteU8(c.S0 + 0x24u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0x14u, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2 + 0x1u);
            mem.WriteU8(c.S0 + 0x15u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0xDu, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2 + 0x1u);
            c.V1 = mem.ReadU8(c.S2 + 0x3u);
            c.V0 = c.V0 + c.V1;
            mem.WriteU8(c.S0 + 0x25u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0x1Du, (byte)c.V0);
            c.V1 = mem.ReadU8(c.S0 + 0x7u);
            c.V0 = 0u | 0x0001u;
            c.At = 0x80090000u;
            mem.WriteU8(c.At + 0x505Fu, (byte)c.V1);
            // a2 == 1: the depth cue at SP+0x54 (IR0 from RTPS, not the size) is scaled by 3/2
            bool notHalf = c.S3 != c.V0;
            c.A1 = c.A1 + 0x4u;
            if (!notHalf)
            {
                c.V1 = mem.ReadU32(c.SP + 0x54u);
                c.V0 = (uint)((int)c.V1 >> 1);
                c.V0 = c.V0 + c.V1;
                mem.WriteU32(c.SP + 0x54u, c.V0);
            }
            c.A0 = 0x80050000u;
            c.A0 = c.A0 + 0x7B64u;
            c.A2 = mem.ReadU32(c.SP + 0x54u);
            c.A3 = c.S0 + 0x4u;
            c.RA = 0x8001E430u;
            KingsField1_game.func_8004DD14(c, mem);
            // the packet's final code and tpage, which the blend mode comes from
            uint code = mem.ReadU8(c.S0 + 0x7u), tpage = mem.ReadU16(c.S0 + 0x16u);
            Kinds.Count(code, tpage);
            bool semi = (code & 2u) != 0u;
            c.V0 = c.S4 << 16;
            c.V0 = (uint)((int)c.V0 >> 16);
            c.A0 = c.S1 + c.V0;
            // the ordering-table index must be inside the table (< 0x4000, and at least 5)
            c.V0 = (int)c.A0 < 5 ? 1u : 0u;
            bool badSlot = c.V0 != 0u;
            c.A1 = c.S0;
            bool retained = wantRetained && !semi;
            if (semi) Semi++;
            if (semi && capture) GpuWorld.Fallback(R.Address, caller, "sprite-semi");
            else if (!wantRetained && capture) GpuWorld.Fallback(R.Address, caller, "sprite-writer");
            if (retained && !badSlot) retained = Publish(c, mem, true);
            else if (!badSlot && capture && GpuWorld.Check) Publish(c, mem, false);
            if (!retained)
            {
                if (capture) Kept++;
                mem.WriteU32(recPtr + 0x8u, cursor + 0x28u);
                if (!badSlot)
                {
                    c.A0 = c.A0 & 0x3FFFu;
                    c.V0 = 0x80090000u;
                    c.V0 = mem.ReadU32(c.V0 + 0xEBCu);
                    c.A0 = c.A0 << 2;
                    c.A0 = c.A0 + c.V0;
                    c.RA = 0x8001E460u;
                    KingsField1_game.AddPrim_game(c, mem);
                }
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x84u);
        c.S4 = mem.ReadU32(c.SP + 0x80u);
        c.S3 = mem.ReadU32(c.SP + 0x7Cu);
        c.S2 = mem.ReadU32(c.SP + 0x78u);
        c.S1 = mem.ReadU32(c.SP + 0x74u);
        c.S0 = mem.ReadU32(c.SP + 0x70u);
        c.SP = c.SP + 0x88u;
        return;
    }

    // ---- the retained sprite ------------------------------------------------------------

    /// <summary>The normal every sprite is lit from (NCDS's V0), and the RGBC word.</summary>
    const uint Normal = 0x80057B64u, RgbcWord = 0x8009505Cu;

    static readonly Dictionary<(byte U, byte V, byte W, byte H, uint Clut, uint Tpage), int> Meshes = new();
    static readonly Dictionary<(short X, short Y, short W, short H), int> Poses = new();
    static int _gen = -1;

    public static long Retained, Kept, Semi, MeshBuilds, MeshHits, PoseBuilds, PoseHits, PoseTexels;
    static long _checked, _cornerBad, _otBad, _colChecked, _colBad;
    static float _maxCorner, _maxCol;

    /// <summary>The packets' final code and tpage, counted per writer call.</summary>
    public static class Kinds
    {
        static readonly Dictionary<string, long> Counts = new();
        public static void Count(uint code, uint tpage)
        {
            string k = $"code {code:X2} tpage {tpage:X4} abr {(tpage >> 5) & 3}";
            Counts.TryGetValue(k, out long n);
            Counts[k] = n + 1;
        }
        public static string Text() => string.Join(", ", Counts.Select(kv => $"{kv.Key} x{kv.Value}"));
    }

    /// <summary>Publishes the sprite as a textured quad: a rigid pose of its four corners
    /// in the plane, placed from the GTE's R and T as the writer leaves them. With
    /// <paramref name="add"/> false it only checks against the packet.</summary>
    static bool Publish(CpuContext c, PSMemory m, bool add)
    {
        uint e = c.S2, s0 = c.S0;
        short x = (short)m.ReadU16(e + 4), y = (short)m.ReadU16(e + 6), w = (short)m.ReadU16(e + 8), h = (short)m.ReadU16(e + 0xA);
        byte u = m.ReadU8(e), v = m.ReadU8(e + 1), bw = m.ReadU8(e + 2), bh = m.ReadU8(e + 3);
        uint clut = m.ReadU16(s0 + 0xEu), tpage = m.ReadU16(s0 + 0x16u);
        if (_gen != RetainedScene.MeshGeneration) { Meshes.Clear(); Poses.Clear(); _gen = RetainedScene.MeshGeneration; }

        if (!Meshes.TryGetValue((u, v, bw, bh, clut, tpage), out int mesh))
        {
            mesh = BuildMesh(u, v, bw, bh, clut, tpage, (short)m.ReadU16(Normal), (short)m.ReadU16(Normal + 2), (short)m.ReadU16(Normal + 4));
            Meshes[(u, v, bw, bh, clut, tpage)] = mesh; MeshBuilds++;
        }
        else MeshHits++;
        if (!Poses.TryGetValue((x, y, w, h), out int pose))
        {
            short[] texels =
            [
                x, y, 0, 0, (short)(x + w), y, 0, 0, x, (short)(y + h), 0, 0, (short)(x + w), (short)(y + h), 0, 0,
            ];
            pose = RetainedScene.AddPose(texels) + 1;
            Poses[(x, y, w, h)] = pose; PoseBuilds++; PoseTexels += 4;
        }
        else PoseHits++;

        int bias = (short)c.S4;
        float ir0 = Math.Clamp((int)m.ReadU32(c.SP + 0x54u), 0, 4096);
        var ins = new RetainedScene.ModelInstance
        {
            MeshStart = mesh, MeshCount = 6, MeshAll = 6, Pose = pose, PoseWeight = 0, PoseMorph = false, ViewSpace = true,
            V00 = Lo(0), V01 = Hi(0), V02 = Lo(1), V10 = Hi(1), V11 = Lo(2), V12 = Hi(2),
            V20 = Lo(3), V21 = Hi(3), V22 = Lo(4), Vtx = Ctl(5), Vty = Ctl(6), Vtz = Ctl(7),
            Llm0 = Lo(8) / 4096f, Llm1 = Hi(8) / 4096f, Llm2 = Lo(9) / 4096f, Llm3 = Hi(9) / 4096f, Llm4 = Lo(10) / 4096f,
            Llm5 = Hi(10) / 4096f, Llm6 = Lo(11) / 4096f, Llm7 = Hi(11) / 4096f, Llm8 = Lo(12) / 4096f,
            Bk0 = Ctl(13), Bk1 = Ctl(14), Bk2 = Ctl(15),
            L0 = Lo(16), L1 = Hi(16), L2 = Lo(17), L3 = Hi(17), L4 = Lo(18), L5 = Hi(18), L6 = Lo(19), L7 = Hi(19), L8 = Lo(20),
            // A flat cue: the offset curve's ir0 is 800 + IR0/2, so its w is IR0 at any depth.
            Dqa = 0f, Dqb = (ir0 * 0.5f + 800f) * 4096f, Curve = 1,
            Near = 5 - bias, Far = float.MaxValue,
            Rgbc = m.ReadU32(RgbcWord) & 0xFFFFFF,
        };
        RetainedModels.PlaceWorld(ref ins, GpuWorld.ReadView(m));
        if (GpuWorld.Check) Check(m, ins, x, y, w, h, s0, c.A0, bias, ir0);
        if (!add) return true;
        RetainedScene.AddInstance(ins);
        Retained++;
        return true;
    }

    static int BuildMesh(byte u, byte v, byte bw, byte bh, uint clut, uint tpage, short nx, short ny, short nz)
    {
        int[] order = [0, 1, 2, 1, 3, 2];
        var corners = new RetainedScene.Vertex[6];
        int u1 = (u + bw) & 0xFF, v1 = (v + bh) & 0xFF;
        for (int j = 0; j < 6; j++)
        {
            int k = order[j];
            int cu = (k & 1) != 0 ? u1 : u, cv = (k & 2) != 0 ? v1 : v;
            corners[j] = new RetainedScene.Vertex
            {
                X = k, R = nx, G = ny, B = nz, Clut = clut, Texpage = tpage, U = cu, V = cv,
                Dqa = 0, Dqb = 1, Curve = 2, Rgbc = 3,
                Rect = (uint)(u | v << 8 | u1 << 16 | v1 << 24),
                Flags = RetainedScene.FlagDots | RetainedScene.FlagRect | (j >= 3 ? RetainedScene.FlagQuadTail : 0u),
            };
        }
        return RetainedScene.AddMesh(corners);
    }

    static int Ctl(int r) => (int)Gte.ReadControl(r);
    static short Lo(int r) => (short)Gte.ReadControl(r);
    static short Hi(int r) => (short)(Gte.ReadControl(r) >> 16);

    /// <summary>The corners and the OT index against the packet the writer built, and the
    /// colour the shader evaluates against the packet's NCDS colour.</summary>
    static void Check(PSMemory m, in RetainedScene.ModelInstance i, short x, short y, short w, short h, uint s0, uint writerSlot, int bias, float ir0)
    {
        float hp = GpuWorld.ProjH, cx = GpuWorld.ProjCx, cy = GpuWorld.ProjCy;
        int[] qx = [x, x + w, x, x + w], qy = [y, y, y + h, y + h];
        for (int k = 0; k < 4; k++)
        {
            long ex = (((long)i.V00 * qx[k] + (long)i.V01 * qy[k]) >> 12) + i.Vtx;
            long ey = (((long)i.V10 * qx[k] + (long)i.V11 * qy[k]) >> 12) + i.Vty;
            long ez = (((long)i.V20 * qx[k] + (long)i.V21 * qy[k]) >> 12) + i.Vtz;
            if (ez < 1) continue;
            float sx = cx + hp * ex / ez, sy = cy + hp * ey / ez;
            uint pk = m.ReadU32(s0 + 8u + (uint)k * 8u);
            float d = Math.Max(Math.Abs(sx - (short)(pk & 0xFFFF)), Math.Abs(sy - (short)(pk >> 16)));
            _maxCorner = Math.Max(_maxCorner, d);
            if (d > 1.5f) _cornerBad++;
            _checked++;
        }
        // The OT index from the origin's eye z (SZ over four) plus the bias, as the writer forms it.
        int mine = (Math.Clamp(i.Vtz, 0, 65535) >> 2) + bias;
        if (mine != (int)writerSlot) _otBad++;
        // The colour the shader evaluates: the NCDS dots (LLM, LCM, BK, RGBC), then the flat cue.
        float[] n = [m.ReadU16(Normal), m.ReadU16(Normal + 2), m.ReadU16(Normal + 4)];
        float[] llm = [i.Llm0, i.Llm1, i.Llm2, i.Llm3, i.Llm4, i.Llm5, i.Llm6, i.Llm7, i.Llm8];
        float[] lcm = [i.L0, i.L1, i.L2, i.L3, i.L4, i.L5, i.L6, i.L7, i.L8];
        float[] bk = [i.Bk0, i.Bk1, i.Bk2];
        float[] a = new float[3];
        for (int r = 0; r < 3; r++) a[r] = llm[r * 3] * n[0] + llm[r * 3 + 1] * n[1] + llm[r * 3 + 2] * n[2];
        for (int ch = 0; ch < 3; ch++)
        {
            float ir = bk[ch] + (lcm[ch * 3] * a[0] + lcm[ch * 3 + 1] * a[1] + lcm[ch * 3 + 2] * a[2]) / 4096f;
            ir = Math.Clamp(ir, 0f, 32767f);
            float lit = ((i.Rgbc >> (8 * ch)) & 255) * ir / 4096f;
            float pred = Math.Clamp(lit * (1f - ir0 / 4096f), 0f, 255f);
            float got = m.ReadU8(s0 + 4u + (uint)ch);
            float dc = Math.Abs(pred - got);
            _maxCol = Math.Max(_maxCol, dc);
            if (dc > 2f) _colBad++;
            _colChecked++;
        }
    }

    public static string Report() =>
        $"retained {Retained}, kept {Kept}, semi {Semi}, meshes {MeshBuilds}/{MeshHits}, " +
        $"poses {PoseBuilds}/{PoseHits} ({PoseTexels} texels), kinds: {Kinds.Text()}";

    public static string CheckReport() =>
        $"sprite corners {_checked}: max {_maxCorner:0.00} px (over 1.5: {_cornerBad}), OT index off {_otBad}, " +
        $"colour channels {_colChecked}: max {_maxCol:0.0} (over 2: {_colBad})";
}
