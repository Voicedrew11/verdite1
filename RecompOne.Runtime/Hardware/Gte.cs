using System.Runtime.CompilerServices;

namespace RecompOne.Runtime;

public static class Gte
{
    private static readonly short[] V = new short[9];
    private static byte RGBC_R, RGBC_G, RGBC_B, RGBC_CODE;
    private static ushort OTZ;
    private static int IR0, IR1, IR2, IR3;
    private static readonly short[] SX = new short[3];
    private static readonly short[] SY = new short[3];
    private static readonly ushort[] SZ = new ushort[4];
    private static readonly uint[] RGB = new uint[3];
    private static uint RES1;
    private static int MAC0, MAC1, MAC2, MAC3;
    private static uint LZCS, LZCR;

    private static readonly short[] RT = new short[9];
    private static readonly short[] LLM = new short[9];
    private static readonly short[] LCM = new short[9];
    private static readonly int[] TR = new int[3];
    private static readonly int[] BK = new int[3];
    private static readonly int[] FC = new int[3];
    private static int OFX, OFY;
    private static ushort H;
    private static short DQA;
    private static int DQB;
    private static short ZSF3, ZSF4;
    private static uint FLAG;

    // What the GTE knows about a projected vertex and then throws away: the view
    // depth it divided by, and the fraction of a pixel the shift down to SX/SY
    // truncated. One per screen-coordinate FIFO slot, shifted with SX and SY, so a
    // read of SXY0/1/2 can hand out the numbers that belong to *that* slot.
    // 0009-0012; upstream has no equivalent -- PGXP answers the same question from
    // the other side, through the CPU's registers rather than by address.
    internal struct Projected
    {
        public float Z, Fx, Fy;
        public bool Clipped, Valid;
    }

    private static readonly Projected[] SP = new Projected[3];

    private const int SnapshotSlots = 8192;
    
    private static readonly short[] _snapRt = new short[SnapshotSlots * 9];
    private static readonly int[] _snapTr = new int[SnapshotSlots * 3];
    private static readonly int[] _snapView = new int[SnapshotSlots * 3];
    private static int _snapSerial;
    private static bool _snapDirty = true;
    
    public static int TransformSerial
    {
        get
        {
            if (!_snapDirty) return _snapSerial;
            
            _snapSerial++;
            _snapDirty = false;
            
            var slot = (_snapSerial & (SnapshotSlots - 1)) * 9;
            for (var i = 0; i < 9; i++) _snapRt[slot + i] = RT[i];
            
            slot = (_snapSerial & (SnapshotSlots - 1)) * 3;
            for (var i = 0; i < 3; i++) _snapTr[slot + i] = TR[i];
            
            _snapView[slot] = H;
            _snapView[slot + 1] = OFX;
            _snapView[slot + 2] = OFY;
            
            return _snapSerial;
        }
    }
    
    public static bool Snapshot(int serial, Span<short> rotation, Span<int> translation, Span<int> view)
    {
        if (serial <= 0 || _snapSerial - serial >= SnapshotSlots) return false;
        
        var slot = (serial & (SnapshotSlots - 1)) * 9;
        for (var i = 0; i < 9; i++) rotation[i] = _snapRt[slot + i];
        
        slot = (serial & (SnapshotSlots - 1)) * 3;
        for (var i = 0; i < 3; i++) translation[i] = _snapTr[slot + i];
        for (var i = 0; i < 3; i++) view[i] = _snapView[slot + i];
        
        return true;
    }
    
    /// <summary>0047. Every register, for a caller that has to run the same work twice
    /// from one state and compare (KF2_POLYASM=verify).</summary>
    public sealed class State
    {
        internal readonly short[] V = new short[9], SX = new short[3], SY = new short[3];
        internal readonly short[] RT = new short[9], LLM = new short[9], LCM = new short[9];
        internal readonly ushort[] SZ = new ushort[4];
        internal readonly uint[] RGB = new uint[3];
        internal readonly int[] TR = new int[3], BK = new int[3], FC = new int[3];
        internal readonly Projected[] SP = new Projected[3];
        internal readonly int[] Ints = new int[16];
        internal readonly uint[] Uints = new uint[8];
    }

    public static void Save(State s)
    {
        V.CopyTo(s.V, 0); SX.CopyTo(s.SX, 0); SY.CopyTo(s.SY, 0);
        RT.CopyTo(s.RT, 0); LLM.CopyTo(s.LLM, 0); LCM.CopyTo(s.LCM, 0);
        SZ.CopyTo(s.SZ, 0); RGB.CopyTo(s.RGB, 0);
        TR.CopyTo(s.TR, 0); BK.CopyTo(s.BK, 0); FC.CopyTo(s.FC, 0); SP.CopyTo(s.SP, 0);
        var i = s.Ints;
        i[0] = IR0; i[1] = IR1; i[2] = IR2; i[3] = IR3; i[4] = MAC0; i[5] = MAC1; i[6] = MAC2; i[7] = MAC3;
        i[8] = OFX; i[9] = OFY; i[10] = DQA; i[11] = DQB; i[12] = ZSF3; i[13] = ZSF4; i[14] = H; i[15] = OTZ;
        var u = s.Uints;
        u[0] = (uint)(RGBC_R | (RGBC_G << 8) | (RGBC_B << 16) | (RGBC_CODE << 24));
        u[1] = RES1; u[2] = LZCS; u[3] = LZCR; u[4] = FLAG;
    }

    public static void Load(State s)
    {
        s.V.CopyTo(V, 0); s.SX.CopyTo(SX, 0); s.SY.CopyTo(SY, 0);
        s.RT.CopyTo(RT, 0); s.LLM.CopyTo(LLM, 0); s.LCM.CopyTo(LCM, 0);
        s.SZ.CopyTo(SZ, 0); s.RGB.CopyTo(RGB, 0);
        s.TR.CopyTo(TR, 0); s.BK.CopyTo(BK, 0); s.FC.CopyTo(FC, 0); s.SP.CopyTo(SP, 0);
        var i = s.Ints;
        IR0 = i[0]; IR1 = i[1]; IR2 = i[2]; IR3 = i[3]; MAC0 = i[4]; MAC1 = i[5]; MAC2 = i[6]; MAC3 = i[7];
        OFX = i[8]; OFY = i[9]; DQA = (short)i[10]; DQB = i[11]; ZSF3 = (short)i[12]; ZSF4 = (short)i[13];
        H = (ushort)i[14]; OTZ = (ushort)i[15];
        var u = s.Uints;
        RGBC_R = (byte)u[0]; RGBC_G = (byte)(u[0] >> 8); RGBC_B = (byte)(u[0] >> 16); RGBC_CODE = (byte)(u[0] >> 24);
        RES1 = u[1]; LZCS = u[2]; LZCR = u[3]; FLAG = u[4];
        _snapDirty = true;
        if (++_lightGen == 0)
        {
            Array.Clear(_light);
            _lightGen = 1;
        }
    }

    /// <summary>The register that differs first, or null.</summary>
    public static string? Diff(State a, State b)
    {
        static string? Arr<T>(string name, T[] x, T[] y) where T : IEquatable<T>
        {
            for (var k = 0; k < x.Length; k++)
                if (!x[k].Equals(y[k])) return $"{name}[{k}] {x[k]} vs {y[k]}";
            return null;
        }

        return Arr("V", a.V, b.V) ?? Arr("SX", a.SX, b.SX) ?? Arr("SY", a.SY, b.SY) ?? Arr("SZ", a.SZ, b.SZ)
            ?? Arr("RGB", a.RGB, b.RGB) ?? Arr("int", a.Ints, b.Ints) ?? Arr("uint", a.Uints, b.Uints)
            ?? Arr("RT", a.RT, b.RT) ?? Arr("LLM", a.LLM, b.LLM) ?? Arr("LCM", a.LCM, b.LCM)
            ?? Arr("TR", a.TR, b.TR) ?? Arr("BK", a.BK, b.BK) ?? Arr("FC", a.FC, b.FC) ?? Projections(a.SP, b.SP);

        static string? Projections(Projected[] x, Projected[] y)
        {
            for (var k = 0; k < x.Length; k++)
                if (x[k].Z != y[k].Z || x[k].Fx != y[k].Fx || x[k].Fy != y[k].Fy ||
                    x[k].Clipped != y[k].Clipped || x[k].Valid != y[k].Valid)
                    return $"SP[{k}]";
            return null;
        }
    }

    private static readonly byte[] Unr = BuildUnr();

    private static byte[] BuildUnr()
    {
        var t = new byte[0x101];
        for (var i = 0; i < 0x101; i++)
        {
            var v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            t[i] = (byte)(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
        }

        return t;
    }

    private static void Flag(int bit)
    {
        FLAG |= 1u << bit;
    }

    private static int SatIR(int n, int v, bool lm)
    {
        var min = lm ? 0 : -0x8000;
        if (v < min)
        {
            v = min;
            Flag(25 - n);
        }
        else if (v > 0x7FFF)
        {
            v = 0x7FFF;
            Flag(25 - n);
        }

        return v;
    }

    private static int SatIR0(int v)
    {
        if (v < 0)
        {
            Flag(12);
            return 0;
        }

        if (v > 0x1000)
        {
            Flag(12);
            return 0x1000;
        }

        return v;
    }

    private static int SatColor(int n, int v)
    {
        if (v < 0)
        {
            Flag(21 - n);
            return 0;
        }

        if (v > 0xFF)
        {
            Flag(21 - n);
            return 0xFF;
        }

        return v;
    }

    private static int SatSZ(int v)
    {
        if (v < 0)
        {
            Flag(18);
            return 0;
        }

        if (v > 0xFFFF)
        {
            Flag(18);
            return 0xFFFF;
        }

        return v;
    }

    private static int SatX(int v)
    {
        if (v < -0x400)
        {
            Flag(14);
            return -0x400;
        }

        if (v > 0x3FF)
        {
            Flag(14);
            return 0x3FF;
        }

        return v;
    }

    private static int SatY(int v)
    {
        if (v < -0x400)
        {
            Flag(13);
            return -0x400;
        }

        if (v > 0x3FF)
        {
            Flag(13);
            return 0x3FF;
        }

        return v;
    }

    private static long CheckMac0(long v)
    {
        if (v > 0x7FFFFFFFL) Flag(16);
        else if (v < -0x80000000L) Flag(15);
        return v;
    }

    private static void CheckMac(int n, long v)
    {
        if (v >= 1L << 43) Flag(31 - n);
        else if (v < -(1L << 43)) Flag(28 - n);
    }

    private static void SetMac(int n, long v, int sf, bool lm)
    {
        CheckMac(n, v);
        var m = (int)(v >> sf);
        if (n == 1)
        {
            MAC1 = m;
            IR1 = SatIR(1, m, lm);
        }
        else if (n == 2)
        {
            MAC2 = m;
            IR2 = SatIR(2, m, lm);
        }
        else
        {
            MAC3 = m;
            IR3 = SatIR(3, m, lm);
        }
    }

    private static void MatVec(short[] mx, int t0, int t1, int t2, int vx, int vy, int vz, int sf, bool lm)
    {
        SetMac(1, ((long)t0 << 12) + (long)mx[0] * vx + (long)mx[1] * vy + (long)mx[2] * vz, sf, lm);
        SetMac(2, ((long)t1 << 12) + (long)mx[3] * vx + (long)mx[4] * vy + (long)mx[5] * vz, sf, lm);
        SetMac(3, ((long)t2 << 12) + (long)mx[6] * vx + (long)mx[7] * vy + (long)mx[8] * vz, sf, lm);
    }

    private static void PushColor()
    {
        var r = SatColor(0, MAC1 >> 4);
        var g = SatColor(1, MAC2 >> 4);
        var b = SatColor(2, MAC3 >> 4);
        RGB[0] = RGB[1];
        RGB[1] = RGB[2];
        RGB[2] = (uint)(r | (g << 8) | (b << 16) | (RGBC_CODE << 24));
    }

    private static void Interp(long in1, long in2, long in3, int sf, bool lm)
    {
        IR1 = SatIR(1, (int)((((long)FC[0] << 12) - in1) >> sf), false);
        IR2 = SatIR(2, (int)((((long)FC[1] << 12) - in2) >> sf), false);
        IR3 = SatIR(3, (int)((((long)FC[2] << 12) - in3) >> sf), false);
        SetMac(1, (long)IR1 * IR0 + in1, sf, lm);
        SetMac(2, (long)IR2 * IR0 + in2, sf, lm);
        SetMac(3, (long)IR3 * IR0 + in3, sf, lm);
        PushColor();
    }

    private static void Modulate(int sf, bool lm)
    {
        SetMac(1, ((long)RGBC_R * IR1) << 4, sf, lm);
        SetMac(2, ((long)RGBC_G * IR2) << 4, sf, lm);
        SetMac(3, ((long)RGBC_B * IR3) << 4, sf, lm);
        PushColor();
    }

    private static int Clz16(uint v)
    {
        return System.Numerics.BitOperations.LeadingZeroCount(v | 1u) - 16;
    }

    private static uint Divide(uint h, uint sz3)
    {
        if (h >= sz3 * 2)
        {
            Flag(17);
            return 0x1FFFF;
        }

        return Quotient(h, sz3);
    }

    /// <summary>0049. H over SZ3 as Rtp divides it for the depth cue, with no flag
    /// raised and no register touched.</summary>
    public static uint DepthQuotient(uint sz3) => H >= sz3 * 2 ? 0x1FFFFu : Quotient(H, sz3);

    private static uint Quotient(uint h, uint sz3)
    {
        var z = Clz16(sz3);
        var n = (ulong)h << z;
        var d = (ulong)sz3 << z;
        var idx = (int)((d - 0x7FC0) >> 7);
        if (idx < 0) idx = 0;
        else if (idx > 0x100) idx = 0x100;
        var u = (ulong)Unr[idx] + 0x101;
        d = (0x2000080UL - d * u) >> 8;
        d = (0x0000080UL + d * u) >> 8;
        var res = (n * d + 0x8000) >> 16;
        return res > 0x1FFFF ? 0x1FFFFu : (uint)res;
    }

    private static void Rtp(int vx, int vy, int vz, int sf, bool lm, bool last)
    {
        var m1 = ((long)TR[0] << 12) + (long)RT[0] * vx + (long)RT[1] * vy + (long)RT[2] * vz;
        var m2 = ((long)TR[1] << 12) + (long)RT[3] * vx + (long)RT[4] * vy + (long)RT[5] * vz;
        var m3 = ((long)TR[2] << 12) + (long)RT[6] * vx + (long)RT[7] * vy + (long)RT[8] * vz;
        CheckMac(1, m1);
        CheckMac(2, m2);
        CheckMac(3, m3);
        MAC1 = (int)(m1 >> sf);
        MAC2 = (int)(m2 >> sf);
        MAC3 = (int)(m3 >> sf);
        IR1 = SatIR(1, MAC1, lm);
        IR2 = SatIR(2, MAC2, lm);
        var ir3flag = (int)(m3 >> 12);
        if (ir3flag < -0x8000 || ir3flag > 0x7FFF) Flag(22);
        IR3 = MAC3 < (lm ? 0 : -0x8000) ? lm ? 0 : -0x8000 : MAC3 > 0x7FFF ? 0x7FFF : MAC3;

        var sz = SatSZ((int)(m3 >> 12));
        SZ[0] = SZ[1];
        SZ[1] = SZ[2];
        SZ[2] = SZ[3];
        SZ[3] = (ushort)sz;

        var div = Divide(H, SZ[3]);
        var sx = CheckMac0((long)div * IR1 + OFX);
        MAC0 = (int)sx;
        var sy = CheckMac0((long)div * IR2 + OFY);
        MAC0 = (int)sy;
        var rx = (int)(sx >> 16);
        var ry = (int)(sy >> 16);
        var nx = SatX(rx);
        var ny = SatY(ry);
        SX[0] = SX[1];
        SX[1] = SX[2];
        SX[2] = (short)nx;
        SY[0] = SY[1];
        SY[1] = SY[2];
        SY[2] = (short)ny;

        ProjectTail(m3, sx, sy, sz, rx, ry, nx, ny);

        if (last)
        {
            var dp = CheckMac0((long)div * DQA + DQB);
            MAC0 = (int)dp;
            IR0 = SatIR0((int)(dp >> 12));
        }
    }

    // 0047. The forms every caller in this game uses -- NormalColorDpq(3) and
    // NormalColorCol at sf=12 lm=1, DpqColor and RotTrans at sf=12 lm=0 -- with the
    // shift, the saturation floor and the flag bits constant and the flags gathered in
    // a local. Bit-identical to the general path, which KF2_GTE_FAST=0 keeps. The two
    // matrix products of a lighting op depend only on the normal and on LLM, BK and
    // LCM, so they are remembered per normal until one of those is written
    // (KF2_GTE_LIGHTCACHE=0 turns that off).
    public static bool FastLighting = Environment.GetEnvironmentVariable("KF2_GTE_FAST") != "0";
    public static bool LightCache = Environment.GetEnvironmentVariable("KF2_GTE_LIGHTCACHE") != "0";
    public static long LightCacheHits, LightCacheMisses;

    private struct LightEntry
    {
        public ulong Key;
        public uint Gen, Flags;
        public int Ir1, Ir2, Ir3;
    }

    private const int LightSlotBits = 10;
    private static readonly LightEntry[] _light = new LightEntry[1 << LightSlotBits];
    private static uint _lightGen = 1;

    /// <summary>SetMac and SatIR at sf=12: returns MAC, gives IR.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int MacFast(long v, int n, bool lm, ref uint f, out int ir)
    {
        if (v >= 1L << 43) f |= 1u << (31 - n);
        else if (v < -(1L << 43)) f |= 1u << (28 - n);
        var m = (int)(v >> 12);
        var min = lm ? 0 : -0x8000;
        if (m < min)
        {
            ir = min;
            f |= 1u << (25 - n);
        }
        else if (m > 0x7FFF)
        {
            ir = 0x7FFF;
            f |= 1u << (25 - n);
        }
        else ir = m;
        return m;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int SatIrFast(int v, int n, bool lm, ref uint f)
    {
        var min = lm ? 0 : -0x8000;
        if (v < min)
        {
            f |= 1u << (25 - n);
            return min;
        }
        if (v > 0x7FFF)
        {
            f |= 1u << (25 - n);
            return 0x7FFF;
        }
        return v;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int SatColorFast(int v, int n, ref uint f)
    {
        if (v < 0)
        {
            f |= 1u << (21 - n);
            return 0;
        }
        if (v > 0xFF)
        {
            f |= 1u << (21 - n);
            return 0xFF;
        }
        return v;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void PushColorFast(ref uint f)
    {
        var r = SatColorFast(MAC1 >> 4, 0, ref f);
        var g = SatColorFast(MAC2 >> 4, 1, ref f);
        var b = SatColorFast(MAC3 >> 4, 2, ref f);
        RGB[0] = RGB[1];
        RGB[1] = RGB[2];
        RGB[2] = (uint)(r | (g << 8) | (b << 16) | (RGBC_CODE << 24));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void InterpFast(long in1, long in2, long in3, bool lm, ref uint f)
    {
        var d1 = SatIrFast((int)((((long)FC[0] << 12) - in1) >> 12), 1, false, ref f);
        var d2 = SatIrFast((int)((((long)FC[1] << 12) - in2) >> 12), 2, false, ref f);
        var d3 = SatIrFast((int)((((long)FC[2] << 12) - in3) >> 12), 3, false, ref f);
        long ir0 = IR0;
        MAC1 = MacFast(d1 * ir0 + in1, 1, lm, ref f, out IR1);
        MAC2 = MacFast(d2 * ir0 + in2, 2, lm, ref f, out IR2);
        MAC3 = MacFast(d3 * ir0 + in3, 3, lm, ref f, out IR3);
        PushColorFast(ref f);
    }

    /// <summary>Both matrix products of Ncs/Ncds/Nccs (lm=1), as the final IR1-3 and
    /// the flag bits they raised. MAC1-3 and IR1-3 are overwritten by every op's last
    /// stage, so nothing else of them is kept.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void LightStage(int vec, out int i1, out int i2, out int i3, ref uint f) =>
        LightStage(V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], out i1, out i2, out i3, ref f);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void LightStage(long vx, long vy, long vz, out int i1, out int i2, out int i3, ref uint f)
    {
        var key = (ulong)(ushort)vx | ((ulong)(ushort)vy << 16) | ((ulong)(ushort)vz << 32);
        ref var e = ref _light[(int)((key * 0x9E3779B97F4A7C15UL) >> (64 - LightSlotBits))];
        if (LightCache && e.Gen == _lightGen && e.Key == key)
        {
            LightCacheHits++;
            i1 = e.Ir1;
            i2 = e.Ir2;
            i3 = e.Ir3;
            f |= e.Flags;
            return;
        }

        uint g = 0;
        MacFast(LLM[0] * vx + LLM[1] * vy + LLM[2] * vz, 1, true, ref g, out var a1);
        MacFast(LLM[3] * vx + LLM[4] * vy + LLM[5] * vz, 2, true, ref g, out var a2);
        MacFast(LLM[6] * vx + LLM[7] * vy + LLM[8] * vz, 3, true, ref g, out var a3);
        MacFast(((long)BK[0] << 12) + (long)LCM[0] * a1 + (long)LCM[1] * a2 + (long)LCM[2] * a3, 1, true, ref g, out i1);
        MacFast(((long)BK[1] << 12) + (long)LCM[3] * a1 + (long)LCM[4] * a2 + (long)LCM[5] * a3, 2, true, ref g, out i2);
        MacFast(((long)BK[2] << 12) + (long)LCM[6] * a1 + (long)LCM[7] * a2 + (long)LCM[8] * a3, 3, true, ref g, out i3);
        f |= g;

        if (!LightCache) return;
        LightCacheMisses++;
        e.Key = key;
        e.Gen = _lightGen;
        e.Flags = g;
        e.Ir1 = i1;
        e.Ir2 = i2;
        e.Ir3 = i3;
    }

    /// <summary>0048. IR1-3 a lighting op would reach for this normal under the current
    /// LLM, LCM and BK, without touching any register. Per-pixel lighting reads it.</summary>
    public static void LightProducts(short vx, short vy, short vz, out int i1, out int i2, out int i3)
    {
        uint f = 0;
        LightStage(vx, vy, vz, out i1, out i2, out i3, ref f);
    }

    /// <summary>0048. LLM times the normal over 4096, before the clamp at zero.</summary>
    public static void LightDots(short vx, short vy, short vz, out float a1, out float a2, out float a3)
    {
        a1 = ((long)LLM[0] * vx + (long)LLM[1] * vy + (long)LLM[2] * vz) / 4096f;
        a2 = ((long)LLM[3] * vx + (long)LLM[4] * vy + (long)LLM[5] * vz) / 4096f;
        a3 = ((long)LLM[6] * vx + (long)LLM[7] * vy + (long)LLM[8] * vz) / 4096f;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void NcdsFast(int vec, ref uint f)
    {
        LightStage(vec, out var i1, out var i2, out var i3, ref f);
        InterpFast(((long)RGBC_R * i1) << 4, ((long)RGBC_G * i2) << 4, ((long)RGBC_B * i3) << 4, true, ref f);
    }

    /// <summary>Rtps at sf=12 lm=0: Rtp with the flags in a local.</summary>
    private static void RtpsFast()
    {
        uint f = 0;
        long vx = V[0], vy = V[1], vz = V[2];
        var m1 = ((long)TR[0] << 12) + RT[0] * vx + RT[1] * vy + RT[2] * vz;
        var m2 = ((long)TR[1] << 12) + RT[3] * vx + RT[4] * vy + RT[5] * vz;
        var m3 = ((long)TR[2] << 12) + RT[6] * vx + RT[7] * vy + RT[8] * vz;
        MAC1 = MacFast(m1, 1, false, ref f, out IR1);
        MAC2 = MacFast(m2, 2, false, ref f, out IR2);
        if (m3 >= 1L << 43) f |= 1u << 28;
        else if (m3 < -(1L << 43)) f |= 1u << 25;
        MAC3 = (int)(m3 >> 12);
        var ir3flag = (int)(m3 >> 12);
        if (ir3flag < -0x8000 || ir3flag > 0x7FFF) f |= 1u << 22;
        IR3 = MAC3 < -0x8000 ? -0x8000 : MAC3 > 0x7FFF ? 0x7FFF : MAC3;

        var sz = (int)(m3 >> 12);
        if (sz < 0)
        {
            f |= 1u << 18;
            sz = 0;
        }
        else if (sz > 0xFFFF)
        {
            f |= 1u << 18;
            sz = 0xFFFF;
        }
        SZ[0] = SZ[1];
        SZ[1] = SZ[2];
        SZ[2] = SZ[3];
        SZ[3] = (ushort)sz;

        uint div;
        if (H >= (uint)sz * 2)
        {
            f |= 1u << 17;
            div = 0x1FFFF;
        }
        else
        {
            var z = Clz16((uint)sz);
            var n = (ulong)H << z;
            var d = (ulong)sz << z;
            var idx = (int)((d - 0x7FC0) >> 7);
            if (idx < 0) idx = 0;
            else if (idx > 0x100) idx = 0x100;
            var u = (ulong)Unr[idx] + 0x101;
            d = (0x2000080UL - d * u) >> 8;
            d = (0x0000080UL + d * u) >> 8;
            var res = (n * d + 0x8000) >> 16;
            div = res > 0x1FFFF ? 0x1FFFFu : (uint)res;
        }

        var sx = (long)div * IR1 + OFX;
        if (sx > 0x7FFFFFFFL) f |= 1u << 16;
        else if (sx < -0x80000000L) f |= 1u << 15;
        var sy = (long)div * IR2 + OFY;
        if (sy > 0x7FFFFFFFL) f |= 1u << 16;
        else if (sy < -0x80000000L) f |= 1u << 15;
        var rx = (int)(sx >> 16);
        var ry = (int)(sy >> 16);
        var nx = rx;
        if (rx < -0x400)
        {
            f |= 1u << 14;
            nx = -0x400;
        }
        else if (rx > 0x3FF)
        {
            f |= 1u << 14;
            nx = 0x3FF;
        }
        var ny = ry;
        if (ry < -0x400)
        {
            f |= 1u << 13;
            ny = -0x400;
        }
        else if (ry > 0x3FF)
        {
            f |= 1u << 13;
            ny = 0x3FF;
        }
        SX[0] = SX[1];
        SX[1] = SX[2];
        SX[2] = (short)nx;
        SY[0] = SY[1];
        SY[1] = SY[2];
        SY[2] = (short)ny;

        ProjectTail(m3, sx, sy, sz, rx, ry, nx, ny);

        var dp = (long)div * DQA + DQB;
        if (dp > 0x7FFFFFFFL) f |= 1u << 16;
        else if (dp < -0x80000000L) f |= 1u << 15;
        MAC0 = (int)dp;
        var ir0 = (int)(dp >> 12);
        if (ir0 < 0)
        {
            f |= 1u << 12;
            ir0 = 0;
        }
        else if (ir0 > 0x1000)
        {
            f |= 1u << 12;
            ir0 = 0x1000;
        }
        IR0 = ir0;
        FLAG = f;
        EndFlag();
    }

    private static void RotTransFast()
    {
        uint f = 0;
        long vx = V[0], vy = V[1], vz = V[2];
        MAC1 = MacFast(((long)TR[0] << 12) + RT[0] * vx + RT[1] * vy + RT[2] * vz, 1, false, ref f, out IR1);
        MAC2 = MacFast(((long)TR[1] << 12) + RT[3] * vx + RT[4] * vy + RT[5] * vz, 2, false, ref f, out IR2);
        MAC3 = MacFast(((long)TR[2] << 12) + RT[6] * vx + RT[7] * vy + RT[8] * vz, 3, false, ref f, out IR3);
        FLAG = f;
        EndFlag();
    }

    /// <summary>Everything Rtp does with a projected vertex besides the registers.</summary>
    private static void ProjectTail(long m3, long sx, long sy, int sz, int rx, int ry, int nx, int ny)
    {
        // PGXP takes the same moment from the other side: not the truncated packet
        // coordinate and the fraction it lost, but the 16.16 projection recomputed
        // in floating point off the un-truncated view depth. The two mechanisms sit
        // side by side here on purpose -- one switch downstream decides which of
        // them DrawPolygon asks.
        if (Pgxp.Pgxp.Enabled) PushPrecise(m3, nx, ny);

        // The depth and the truncated fraction travel with the coordinate, in the
        // same FIFO, so that whichever slot the game stores later carries its own
        // numbers rather than the newest ones. sx and sy are 16.16 and the shift
        // above floors them, so trueX - nx is exactly what SX2 lost -- and it is
        // only meaningful when the coordinate did not clamp.
        SP[0] = SP[1]; SP[1] = SP[2];
        var clipped = rx != nx || ry != ny;
        SP[2] = new Projected
        {
            Z = sz,
            Fx = clipped ? 0f : (sx & 0xFFFF) * (1f / 65536f),
            Fy = clipped ? 0f : (sy & 0xFFFF) * (1f / 65536f),
            Clipped = clipped,
            Valid = sz > 0,
        };

        // The one place the screen position, the depth that made it and the true
        // 16.16 coordinate are all in hand. GteDepth keys on the (possibly
        // clamped) packet position because that is what survives into the GP0
        // packet. A saturated vertex is recorded for its depth so the polygon
        // can stay perspective-correct, but it is not moved off the clamp wall
        // — that opened holes along shared edges. Several vertices can share a
        // clamped key; Apply picks a depth per primitive and a fraction per key.
        //
        // sx and sy are 16.16 and the shift above floors them, so trueX - nx is
        // the fraction SX2 lost when the coordinate did not clamp.
        if (GteDepth.Active)
        {
            // The probes measure what the GTE projected, not what either mechanism
            // chose to keep, so this is counted whichever of the two is answering.
            GteDepth.NoteProjected(SP[2].Fx, SP[2].Fy, clipped, sz > 0);
            // The projection itself, for the ambient-occlusion pass: it has to undo
            // this divide to get a view position back out of a depth texel, and H
            // and the OFX/OFY centre are that projection rather than a guess at it.
            // OFX/OFY are 16.16 here, as SX/SY are before the shift.
            if (GteDepth.SurfacesWanted)
            {
                GteDepth.NoteProjection(H, OFX * (1f / 65536f), OFY * (1f / 65536f));
                GteDepth.NoteDepthCue(DQA, DQB);
            }
            if (GteDepth.PositionFallback)
                GteDepth.Record(nx, ny, sz, sx * (1f / 65536f), sy * (1f / 65536f), clipped);
        }
    }

    private static void EndFlag()
    {
        if ((FLAG & 0x7F87E000u) != 0) FLAG |= 0x80000000u;
    }

    private static void PushPrecise(long zAcc, int nx, int ny)
    {
        var z = zAcc * (1.0 / 4096.0);
        var w = Math.Max(z, H / 2.0);
        var hDivSz = H / w;

        var px = (float)(OFX / 65536.0 + IR1 * hDivSz);
        var py = (float)(OFY / 65536.0 + IR2 * hDivSz);

        px = Math.Clamp(px, -0x400, 0x3FF);
        py = Math.Clamp(py, -0x400, 0x3FF);

        Pgxp.PgxpGte.PushVertex(px, py, (float)w, Pgxp.PgxpGte.PackXy(nx, ny), TransformSerial);
    }

    public static void Rtps(int sf, bool lm)
    {
        if (FastLighting && sf == 12 && !lm)
        {
            RtpsFast();
            return;
        }

        FLAG = 0;
        Rtp(V[0], V[1], V[2], sf, lm, true);
        EndFlag();
    }

    public static void Rtpt(int sf, bool lm)
    {
        FLAG = 0;
        Rtp(V[0], V[1], V[2], sf, lm, false);
        Rtp(V[3], V[4], V[5], sf, lm, false);
        Rtp(V[6], V[7], V[8], sf, lm, true);
        EndFlag();
    }

    public static void Nclip()
    {
        FLAG = 0;

        if (Pgxp.Pgxp.CullingCorrection &&
            Pgxp.PgxpGte.TryNclip(SX[0], SY[0], SX[1], SY[1], SX[2], SY[2], out var precise))
        {
            MAC0 = (int)CheckMac0((long)precise);
            EndFlag();
            return;
        }

        MAC0 = (int)CheckMac0((long)SX[0] * (SY[1] - SY[2]) + (long)SX[1] * (SY[2] - SY[0]) +
                              (long)SX[2] * (SY[0] - SY[1]));
        EndFlag();
    }

    public static void Avsz3()
    {
        FLAG = 0;
        MAC0 = (int)CheckMac0((long)ZSF3 * (SZ[1] + SZ[2] + SZ[3]));
        OTZ = (ushort)SatSZ(MAC0 >> 12);
        EndFlag();
    }

    public static void Avsz4()
    {
        FLAG = 0;
        MAC0 = (int)CheckMac0((long)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]));
        OTZ = (ushort)SatSZ(MAC0 >> 12);
        EndFlag();
    }

    public static void MvmvaOp(int sf, bool lm, int mx, int vn, int cv)
    {
        if (FastLighting && sf == 12 && !lm && mx == 0 && vn == 0 && cv == 0)
        {
            RotTransFast();
            return;
        }

        FLAG = 0;
        Mvmva(sf, lm, mx, vn, cv);
        EndFlag();
    }

    public static void Sqr(int sf, bool lm)
    {
        FLAG = 0;
        SetMac(1, (long)IR1 * IR1, sf, lm);
        SetMac(2, (long)IR2 * IR2, sf, lm);
        SetMac(3, (long)IR3 * IR3, sf, lm);
        EndFlag();
    }

    public static void Cross(int sf, bool lm)
    {
        FLAG = 0;
        int ir1 = IR1, ir2 = IR2, ir3 = IR3;
        SetMac(1, (long)RT[4] * ir3 - (long)RT[8] * ir2, sf, lm);
        SetMac(2, (long)RT[8] * ir1 - (long)RT[0] * ir3, sf, lm);
        SetMac(3, (long)RT[0] * ir2 - (long)RT[4] * ir1, sf, lm);
        EndFlag();
    }

    public static void Gpf(int sf, bool lm)
    {
        FLAG = 0;
        SetMac(1, (long)IR0 * IR1, sf, lm);
        SetMac(2, (long)IR0 * IR2, sf, lm);
        SetMac(3, (long)IR0 * IR3, sf, lm);
        PushColor();
        EndFlag();
    }

    public static void Gpl(int sf, bool lm)
    {
        FLAG = 0;
        SetMac(1, ((long)MAC1 << sf) + (long)IR0 * IR1, sf, lm);
        SetMac(2, ((long)MAC2 << sf) + (long)IR0 * IR2, sf, lm);
        SetMac(3, ((long)MAC3 << sf) + (long)IR0 * IR3, sf, lm);
        PushColor();
        EndFlag();
    }

    public static void Dpcs(int sf, bool lm)
    {
        if (FastLighting && sf == 12 && !lm)
        {
            uint f = 0;
            InterpFast((long)RGBC_R << 16, (long)RGBC_G << 16, (long)RGBC_B << 16, false, ref f);
            FLAG = f;
            EndFlag();
            return;
        }

        FLAG = 0;
        Interp((long)RGBC_R << 16, (long)RGBC_G << 16, (long)RGBC_B << 16, sf, lm);
        EndFlag();
    }

    public static void Dpct(int sf, bool lm)
    {
        FLAG = 0;
        for (var i = 0; i < 3; i++)
            Interp((long)(RGB[0] & 0xFF) << 16, (long)((RGB[0] >> 8) & 0xFF) << 16, (long)((RGB[0] >> 16) & 0xFF) << 16,
                sf, lm);

        EndFlag();
    }

    public static void Intpl(int sf, bool lm)
    {
        FLAG = 0;
        Interp((long)IR1 << 12, (long)IR2 << 12, (long)IR3 << 12, sf, lm);
        EndFlag();
    }

    public static void Dcpl(int sf, bool lm)
    {
        FLAG = 0;
        Interp(((long)RGBC_R * IR1) << 4, ((long)RGBC_G * IR2) << 4, ((long)RGBC_B * IR3) << 4, sf, lm);
        EndFlag();
    }

    public static void NcsOp(int sf, bool lm)
    {
        FLAG = 0;
        Ncs(0, sf, lm);
        EndFlag();
    }

    public static void NctOp(int sf, bool lm)
    {
        FLAG = 0;
        Ncs(0, sf, lm);
        Ncs(1, sf, lm);
        Ncs(2, sf, lm);
        EndFlag();
    }

    public static void NcdsOp(int sf, bool lm)
    {
        if (FastLighting && sf == 12 && lm)
        {
            uint f = 0;
            NcdsFast(0, ref f);
            FLAG = f;
            EndFlag();
            return;
        }

        FLAG = 0;
        Ncds(0, sf, lm);
        EndFlag();
    }

    public static void NcdtOp(int sf, bool lm)
    {
        if (FastLighting && sf == 12 && lm)
        {
            uint f = 0;
            NcdsFast(0, ref f);
            NcdsFast(1, ref f);
            NcdsFast(2, ref f);
            FLAG = f;
            EndFlag();
            return;
        }

        FLAG = 0;
        Ncds(0, sf, lm);
        Ncds(1, sf, lm);
        Ncds(2, sf, lm);
        EndFlag();
    }

    public static void NccsOp(int sf, bool lm)
    {
        if (FastLighting && sf == 12 && lm)
        {
            uint f = 0;
            LightStage(0, out var i1, out var i2, out var i3, ref f);
            MAC1 = MacFast(((long)RGBC_R * i1) << 4, 1, true, ref f, out IR1);
            MAC2 = MacFast(((long)RGBC_G * i2) << 4, 2, true, ref f, out IR2);
            MAC3 = MacFast(((long)RGBC_B * i3) << 4, 3, true, ref f, out IR3);
            PushColorFast(ref f);
            FLAG = f;
            EndFlag();
            return;
        }

        FLAG = 0;
        Nccs(0, sf, lm);
        EndFlag();
    }

    public static void NcctOp(int sf, bool lm)
    {
        FLAG = 0;
        Nccs(0, sf, lm);
        Nccs(1, sf, lm);
        Nccs(2, sf, lm);
        EndFlag();
    }

    public static void Cc(int sf, bool lm)
    {
        FLAG = 0;
        MatVec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        Modulate(sf, lm);
        EndFlag();
    }

    public static void Cdp(int sf, bool lm)
    {
        FLAG = 0;
        MatVec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        Interp(((long)RGBC_R * IR1) << 4, ((long)RGBC_G * IR2) << 4, ((long)RGBC_B * IR3) << 4, sf, lm);
        EndFlag();
    }
    //should rmeove?
    public static void Execute(uint cmd)
    {
        FLAG = 0;
        var sf = (cmd & (1u << 19)) != 0 ? 12 : 0;
        var lm = (cmd & (1u << 10)) != 0;
        var mx = (int)((cmd >> 17) & 3);
        var vn = (int)((cmd >> 15) & 3);
        var cv = (int)((cmd >> 13) & 3);

        switch (cmd & 0x3F)
        {
            case 0x01:
                Rtps(sf, lm);
                return;
            case 0x06:
                Nclip();
                return;
            case 0x0C:
                Cross(sf, lm);
                return;
            case 0x10:
                Dpcs(sf, lm);
                return;
            case 0x11:
                Intpl(sf, lm);
                return;
            case 0x12:
                MvmvaOp(sf, lm, mx, vn, cv);
                return;
            case 0x13:
                NcdsOp(sf, lm);
                return;
            case 0x14:
                Cdp(sf, lm);
                return;
            case 0x16:
                NcdtOp(sf, lm);
                return;
            case 0x1B:
                NccsOp(sf, lm);
                return;
            case 0x1C:
                Cc(sf, lm);
                return;
            case 0x1E:
                NcsOp(sf, lm);
                return;
            case 0x20:
                NctOp(sf, lm);
                return;
            case 0x28:
                Sqr(sf, lm);
                return;
            case 0x29:
                Dcpl(sf, lm);
                return;
            case 0x2A:
                Dpct(sf, lm);
                return;
            case 0x2D:
                Avsz3();
                return;
            case 0x2E:
                Avsz4();
                return;
            case 0x30:
                Rtpt(sf, lm);
                return;
            case 0x3D:
                Gpf(sf, lm);
                return;
            case 0x3E:
                Gpl(sf, lm);
                return;
            case 0x3F:
                NcctOp(sf, lm);
                return;
        }

        if ((FLAG & 0x7F87E000u) != 0) FLAG |= 0x80000000u;
    }

    private static void Ncs(int vec, int sf, bool lm)
    {
        MatVec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
        MatVec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        PushColor();
    }

    private static void Ncds(int vec, int sf, bool lm)
    {
        MatVec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
        MatVec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        Interp(((long)RGBC_R * IR1) << 4, ((long)RGBC_G * IR2) << 4, ((long)RGBC_B * IR3) << 4, sf, lm);
    }

    private static void Nccs(int vec, int sf, bool lm)
    {
        MatVec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
        MatVec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        Modulate(sf, lm);
    }

    private static void Mvmva(int sf, bool lm, int mx, int vn, int cv)
    {
        var mat = mx == 0 ? RT : mx == 1 ? LLM : mx == 2 ? LCM : RT;
        int vx, vy, vz;
        if (vn < 3)
        {
            vx = V[vn * 3];
            vy = V[vn * 3 + 1];
            vz = V[vn * 3 + 2];
        }
        else
        {
            vx = IR1;
            vy = IR2;
            vz = IR3;
        }

        if (cv == 2)
        {
            SatIR(1, (int)((((long)FC[0] << 12) + (long)mat[0] * vx) >> sf), lm);
            SatIR(2, (int)((((long)FC[1] << 12) + (long)mat[3] * vx) >> sf), lm);
            SatIR(3, (int)((((long)FC[2] << 12) + (long)mat[6] * vx) >> sf), lm);
            SetMac(1, (long)mat[1] * vy + (long)mat[2] * vz, sf, lm);
            SetMac(2, (long)mat[4] * vy + (long)mat[5] * vz, sf, lm);
            SetMac(3, (long)mat[7] * vy + (long)mat[8] * vz, sf, lm);
            return;
        }

        int t0 = 0, t1 = 0, t2 = 0;
        if (cv == 0)
        {
            t0 = TR[0];
            t1 = TR[1];
            t2 = TR[2];
        }
        else if (cv == 1)
        {
            t0 = BK[0];
            t1 = BK[1];
            t2 = BK[2];
        }

        MatVec(mat, t0, t1, t2, vx, vy, vz, sf, lm);
    }

    // A screen coordinate can only leave the GTE through one of these registers, and
    // it leaves as a whole word, so this is the one place an address-keyed map can
    // pick the attributes up. Both `swc2 $14, off(base)` (which is StoreWord, and so
    // comes through here) and `mfc2 rt, $14` are covered: the value is offered to the
    // store that copies it, and GteVertexMap ties it to the address it lands at.
    // w is the view depth the divide actually used, in GTE units, floored at H/2 the
    // way the hardware's divider is: below that the projection is meaningless and a
    // W of nearly zero detonates the perspective divide downstream.
    private static void PublishSxy(int reg, uint value)
    {
        ref readonly var p = ref SP[reg == 15 ? 2 : reg - 12];
        if (!p.Valid) return;
        GteVertexMap.Publish(value, p.Z, p.Fx, p.Fy, p.Clipped);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static uint Read(int reg)
    {
        if (GteVertexMap.Active && (uint)(reg - 12) <= 3u)
            PublishSxy(reg, reg == 12 ? (uint)((ushort)SX[0] | (SY[0] << 16))
                         : reg == 13 ? (uint)((ushort)SX[1] | (SY[1] << 16))
                                     : (uint)((ushort)SX[2] | (SY[2] << 16)));

        switch (reg)
        {
            case 0: return (uint)((ushort)V[0] | (V[1] << 16));
            case 1: return (uint)V[2];
            case 2: return (uint)((ushort)V[3] | (V[4] << 16));
            case 3: return (uint)V[5];
            case 4: return (uint)((ushort)V[6] | (V[7] << 16));
            case 5: return (uint)V[8];
            case 6: return (uint)(RGBC_R | (RGBC_G << 8) | (RGBC_B << 16) | (RGBC_CODE << 24));
            case 7: return OTZ;
            case 8: return (uint)IR0;
            case 9: return (uint)IR1;
            case 10: return (uint)IR2;
            case 11: return (uint)IR3;
            case 12: return (uint)((ushort)SX[0] | (SY[0] << 16));
            case 13: return (uint)((ushort)SX[1] | (SY[1] << 16));
            case 14:
            case 15: return (uint)((ushort)SX[2] | (SY[2] << 16));
            case 16: return SZ[0];
            case 17: return SZ[1];
            case 18: return SZ[2];
            case 19: return SZ[3];
            case 20: return RGB[0];
            case 21: return RGB[1];
            case 22: return RGB[2];
            case 23: return RES1;
            case 24: return (uint)MAC0;
            case 25: return (uint)MAC1;
            case 26: return (uint)MAC2;
            case 27: return (uint)MAC3;
            case 28:
            case 29:
                var r = Math.Clamp(IR1 >> 7, 0, 0x1F);
                var g = Math.Clamp(IR2 >> 7, 0, 0x1F);
                var b = Math.Clamp(IR3 >> 7, 0, 0x1F);
                return (uint)(r | (g << 5) | (b << 10));
            case 30: return LZCS;
            case 31: return LZCR;
            default: return 0;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Write(int reg, uint val)
    {
        switch (reg)
        {
            case 0:
                V[0] = (short)val;
                V[1] = (short)(val >> 16);
                break;
            case 1: V[2] = (short)val; break;
            case 2:
                V[3] = (short)val;
                V[4] = (short)(val >> 16);
                break;
            case 3: V[5] = (short)val; break;
            case 4:
                V[6] = (short)val;
                V[7] = (short)(val >> 16);
                break;
            case 5: V[8] = (short)val; break;
            case 6:
                RGBC_R = (byte)val;
                RGBC_G = (byte)(val >> 8);
                RGBC_B = (byte)(val >> 16);
                RGBC_CODE = (byte)(val >> 24);
                break;
            case 7: OTZ = (ushort)val; break;
            case 8: IR0 = (short)val; break;
            case 9: IR1 = (short)val; break;
            case 10: IR2 = (short)val; break;
            case 11: IR3 = (short)val; break;
            // A coordinate the game loads back in is not one this GTE projected --
            // NormalClip hands all three vertices of a polygon back for the cross
            // product -- so the slot's depth and fraction stop being about it. They
            // are dropped rather than left to be published against a value that
            // happens to match.
            case 12:
                SX[0] = (short)val;
                SY[0] = (short)(val >> 16);
                SP[0] = default;
                break;
            case 13:
                SX[1] = (short)val;
                SY[1] = (short)(val >> 16);
                SP[1] = default;
                break;
            case 14:
                SX[2] = (short)val;
                SY[2] = (short)(val >> 16);
                SP[2] = default;
                break;
            case 15:
                SX[0] = SX[1];
                SY[0] = SY[1];
                SX[1] = SX[2];
                SY[1] = SY[2];
                SX[2] = (short)val;
                SY[2] = (short)(val >> 16);
                SP[0] = SP[1];
                SP[1] = SP[2];
                SP[2] = default;
                break;
            case 16: SZ[0] = (ushort)val; break;
            case 17: SZ[1] = (ushort)val; break;
            case 18: SZ[2] = (ushort)val; break;
            case 19: SZ[3] = (ushort)val; break;
            case 20: RGB[0] = val; break;
            case 21: RGB[1] = val; break;
            case 22: RGB[2] = val; break;
            case 23: RES1 = val; break;
            case 24: MAC0 = (int)val; break;
            case 25: MAC1 = (int)val; break;
            case 26: MAC2 = (int)val; break;
            case 27: MAC3 = (int)val; break;
            case 28:
                IR1 = (int)((val & 0x1F) << 7);
                IR2 = (int)(((val >> 5) & 0x1F) << 7);
                IR3 = (int)(((val >> 10) & 0x1F) << 7);
                break;
            case 29: break;
            case 30:
                LZCS = val;
                var test = (val & 0x80000000u) != 0 ? ~val : val;
                LZCR = (uint)(test == 0 ? 32 : System.Numerics.BitOperations.LeadingZeroCount(test));
                break;
            case 31: break;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static uint ReadControl(int reg)
    {
        switch (reg)
        {
            case 0: return (uint)((ushort)RT[0] | (RT[1] << 16));
            case 1: return (uint)((ushort)RT[2] | (RT[3] << 16));
            case 2: return (uint)((ushort)RT[4] | (RT[5] << 16));
            case 3: return (uint)((ushort)RT[6] | (RT[7] << 16));
            case 4: return (uint)RT[8];
            case 5: return (uint)TR[0];
            case 6: return (uint)TR[1];
            case 7: return (uint)TR[2];
            case 8: return (uint)((ushort)LLM[0] | (LLM[1] << 16));
            case 9: return (uint)((ushort)LLM[2] | (LLM[3] << 16));
            case 10: return (uint)((ushort)LLM[4] | (LLM[5] << 16));
            case 11: return (uint)((ushort)LLM[6] | (LLM[7] << 16));
            case 12: return (uint)LLM[8];
            case 13: return (uint)BK[0];
            case 14: return (uint)BK[1];
            case 15: return (uint)BK[2];
            case 16: return (uint)((ushort)LCM[0] | (LCM[1] << 16));
            case 17: return (uint)((ushort)LCM[2] | (LCM[3] << 16));
            case 18: return (uint)((ushort)LCM[4] | (LCM[5] << 16));
            case 19: return (uint)((ushort)LCM[6] | (LCM[7] << 16));
            case 20: return (uint)LCM[8];
            case 21: return (uint)FC[0];
            case 22: return (uint)FC[1];
            case 23: return (uint)FC[2];
            case 24: return (uint)OFX;
            case 25: return (uint)OFY;
            case 26: return (uint)(short)H;
            case 27: return (uint)DQA;
            case 28: return (uint)DQB;
            case 29: return (uint)ZSF3;
            case 30: return (uint)ZSF4;
            case 31: return FLAG;
            default: return 0;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void WriteControl(int reg, uint val)
    {
        // The rotation matrix, the translation and the screen offsets are what a
        // transform serial identifies; anything else leaves it alone.
        if (reg <= 7 || reg is 24 or 25 or 26) _snapDirty = true;
        if (reg is >= 8 and <= 20 && ++_lightGen == 0)
        {
            Array.Clear(_light);
            _lightGen = 1;
        }

        switch (reg)
        {
            case 0:
                RT[0] = (short)val;
                RT[1] = (short)(val >> 16);
                break;
            case 1:
                RT[2] = (short)val;
                RT[3] = (short)(val >> 16);
                break;
            case 2:
                RT[4] = (short)val;
                RT[5] = (short)(val >> 16);
                break;
            case 3:
                RT[6] = (short)val;
                RT[7] = (short)(val >> 16);
                break;
            case 4: RT[8] = (short)val; break;
            case 5: TR[0] = (int)val; break;
            case 6: TR[1] = (int)val; break;
            case 7: TR[2] = (int)val; break;
            case 8:
                LLM[0] = (short)val;
                LLM[1] = (short)(val >> 16);
                break;
            case 9:
                LLM[2] = (short)val;
                LLM[3] = (short)(val >> 16);
                break;
            case 10:
                LLM[4] = (short)val;
                LLM[5] = (short)(val >> 16);
                break;
            case 11:
                LLM[6] = (short)val;
                LLM[7] = (short)(val >> 16);
                break;
            case 12: LLM[8] = (short)val; break;
            case 13: BK[0] = (int)val; break;
            case 14: BK[1] = (int)val; break;
            case 15: BK[2] = (int)val; break;
            case 16:
                LCM[0] = (short)val;
                LCM[1] = (short)(val >> 16);
                break;
            case 17:
                LCM[2] = (short)val;
                LCM[3] = (short)(val >> 16);
                break;
            case 18:
                LCM[4] = (short)val;
                LCM[5] = (short)(val >> 16);
                break;
            case 19:
                LCM[6] = (short)val;
                LCM[7] = (short)(val >> 16);
                break;
            case 20: LCM[8] = (short)val; break;
            case 21: FC[0] = (int)val; break;
            case 22: FC[1] = (int)val; break;
            case 23: FC[2] = (int)val; break;
            case 24: OFX = (int)val; break;
            case 25: OFY = (int)val; break;
            case 26: H = (ushort)val; break;
            case 27: DQA = (short)val; break;
            case 28: DQB = (int)val; break;
            case 29: ZSF3 = (short)val; break;
            case 30: ZSF4 = (short)val; break;
            case 31:
                FLAG = val & 0x7FFFF000u;
                if ((FLAG & 0x7F87E000u) != 0) FLAG |= 0x80000000u;
                break;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void LoadWord(int reg, uint val)
    {
        Write(reg, val);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static uint StoreWord(int reg)
    {
        return Read(reg);
    }

    public static bool GetCondition()
    {
        return false;
    }
}