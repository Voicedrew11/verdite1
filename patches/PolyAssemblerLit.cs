using System.Runtime.CompilerServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// `func_8002F214`, the models' lit assembler, and `func_8002EAEC`, the same routine
/// forced semi-transparent. The unclipped assembler's loop with GTE lighting per face
/// and a caller's bias on the slot. See "The lit model assembler" in
/// docs/PATCHES_AND_MODS.md.
/// </summary>
public static partial class PolyAssembler
{
    const uint Lit = 0x8002F214;
    const uint LitBlend = 0x8002EAEC;

    static bool _queuedLit, _queuedLitBlend;
    static long _litExhausted;

    public static bool LitEnabled { get; set; } = true;

    public static long LitCalls;

    /// <summary>Which of the two routines, as a type so each gets its own code.</summary>
    interface IBlend
    {
        static abstract bool On { get; }
    }

    struct Opaque : IBlend
    {
        public static bool On => false;
    }

    struct Blended : IBlend
    {
        public static bool On => true;
    }

    static void ReplaceLit(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && LitEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_litCheck, orig, c, mem, RunLit);
        else RunLit(c, mem);
    }

    static void ReplaceLitBlend(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && LitEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_litBlendCheck, orig, c, mem, RunLitBlend);
        else RunLitBlend(c, mem);
    }

    static void RunLit(CpuContext c, PSMemory mem) => RunLit<Opaque>(c, mem);
    static void RunLitBlend(CpuContext c, PSMemory mem) => RunLit<Blended>(c, mem);

    static void RunLit<TB>(CpuContext c, PSMemory mem) where TB : struct, IBlend
    {
        var saved = new Saved(c);
        LitCalls++;
        try { LitBody<TB>(c, mem, saved.SP - 0x60u); }
        finally { saved.Restore(c); }
    }

    static readonly Check _litCheck = new("func_8002F214", LitExtra);
    static readonly Check _litBlendCheck = new("func_8002EAEC", LitExtra);

    static string LitExtra()
    {
        string s = $"; {_litExhausted} buffer exhaustion(s); light cache {Gte.LightCacheHits} hit(s), {Gte.LightCacheMisses} miss(es)";
        _litExhausted = 0;
        return s;
    }

    /// <summary>a0 the model, a1 the slot bias, a2 (blend only) the semi-transparency
    /// rate for the tpage word.</summary>
    static void LitBody<TB>(CpuContext c, PSMemory mem, uint sp) where TB : struct, IBlend
    {
        c.SP = sp;
        uint bias = c.A1;
        uint abr = (c.A2 << 5) & 0xFFFFu;

        uint header = (c.A0 & 0xFFFFu) * 28u + 0xCu + mem.ReadU32(ModelTable);
        uint count = mem.ReadU32(header + 0x14u);
        uint faces = mem.ReadU32(header + 0x10u);
        uint table = mem.ReadU32(ModelTable);
        uint normals = mem.ReadU32(header + 8u) + 0xCu + table;
        uint face = faces + 0xCu + table;

        // 0072. Every face, before a single one is culled, for the retained scene.
        if (RetainedModels.Capturing) RetainedModels.Capture(mem, header, normals, face, count, TB.On ? abr : uint.MaxValue);

        var fr = new Frame(mem);
        if (fr.Lighting) fr.LightGen = GteLightMap.NoteConstants();
        for (; count != 0; count--)
        {
            Interrupts.Poll(c, mem);
            fr.Check();
            uint word = mem.ReadU32(face);
            face += 4u;
            uint cmd = word >> 24;

            bool ok = (cmd & 0xFDu) switch
            {
                0x24u => FlatTriangle<TB>(ref fr, face, cmd, bias, normals, abr),
                0x2Cu => FlatQuad<TB>(ref fr, face, cmd, bias, normals, abr),
                0x34u => GouraudTriangle<TB>(ref fr, face, cmd, bias, normals, abr),
                0x3Cu => GouraudQuad<TB>(ref fr, face, cmd, bias, normals, abr),
                _ => true,
            };
            if (!ok) { _litExhausted++; return; }

            face += (word >> 6) & 0x3FCu;
        }
    }

    /// <summary>POLY_FT3, lit at the mean fog weight.</summary>
    static bool FlatTriangle<TB>(ref Frame fr, uint f, uint cmd, uint bias, uint normals, uint abr) where TB : struct, IBlend
    {
        var mem = fr.Mem;
        uint p0 = VertexCache + R16(ref fr, f + 0x0Eu);
        uint p2 = VertexCache + R16(ref fr, f + 0x12u);
        uint p1 = VertexCache + R16(ref fr, f + 0x10u);
        if (!Facing(mem, p0, p1, p2)) return true;
        if (!Allocate(ref fr, 0x20u, out uint pkt)) return false;

        W16(ref fr, pkt + 0x0Eu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x16u, Tpage<TB>(ref fr, f, abr));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x10u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x18u, mem.ReadU32(p2));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x14u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x1Cu, R16(ref fr, f + 8u));

        int fog = ((short)R16(ref fr, p0 + 6u) + (short)R16(ref fr, p1 + 6u) + (short)R16(ref fr, p2 + 6u)) / 3;
        Shade(ref fr, normals + R16(ref fr, f + 0x0Cu), (uint)fog, pkt + 4u);

        W8(ref fr, pkt + 3u, 0x07);
        W8(ref fr, pkt + 7u, (byte)(TB.On ? 0x26u : cmd));
        if (fr.Lighting) LightFlat(mem, pkt, LightWord(ref fr), normals + R16(ref fr, f + 0x0Cu), 3, p0, p1, p2, 0u);
        RecordDepth(ref fr, pkt, 0x18u, 3, p0, p1, p2, 0u);

        Insert(ref fr, ((short)R16(ref fr, p0 + 4u) + (short)R16(ref fr, p1 + 4u) + (short)R16(ref fr, p2 + 4u)) / 3, bias, pkt);
        return true;
    }

    /// <summary>POLY_FT4, lit at the mean fog weight.</summary>
    static bool FlatQuad<TB>(ref Frame fr, uint f, uint cmd, uint bias, uint normals, uint abr) where TB : struct, IBlend
    {
        var mem = fr.Mem;
        uint p0 = VertexCache + R16(ref fr, f + 0x12u);
        uint p2 = VertexCache + R16(ref fr, f + 0x16u);
        uint p1 = VertexCache + R16(ref fr, f + 0x14u);
        if (!Facing(mem, p0, p1, p2)) return true;
        if (!Allocate(ref fr, 0x28u, f + 0x18u, out uint pkt, out uint p3)) return false;

        W16(ref fr, pkt + 0x0Eu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x16u, Tpage<TB>(ref fr, f, abr));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x10u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x18u, mem.ReadU32(p2));
        mem.WriteU32(pkt + 0x20u, mem.ReadU32(p3));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x14u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x1Cu, R16(ref fr, f + 8u));
        W16(ref fr, pkt + 0x24u, R16(ref fr, f + 0x0Cu));

        uint normal = normals + R16(ref fr, f + 0x10u);
        // The game sums these in this order.
        int fog = ((short)R16(ref fr, p0 + 6u) + (short)R16(ref fr, p1 + 6u)
                 + (short)R16(ref fr, p3 + 6u) + (short)R16(ref fr, p2 + 6u)) >> 2;
        Shade(ref fr, normal, (uint)fog, pkt + 4u);

        W8(ref fr, pkt + 3u, 0x09);
        W8(ref fr, pkt + 7u, (byte)(TB.On ? 0x2Eu : cmd));
        if (fr.Lighting) LightFlat(mem, pkt, LightWord(ref fr), normal, 4, p0, p1, p2, p3);
        RecordDepth(ref fr, pkt, 0x20u, 4, p0, p1, p2, p3);

        Insert(ref fr, QuadDepth(ref fr, p0, p1, p2, p3), bias, pkt);
        return true;
    }

    /// <summary>POLY_GT3, each vertex lit by its own normal at the first vertex's fog.</summary>
    static bool GouraudTriangle<TB>(ref Frame fr, uint f, uint cmd, uint bias, uint normals, uint abr) where TB : struct, IBlend
    {
        var mem = fr.Mem;
        uint p0 = VertexCache + R16(ref fr, f + 0x0Eu);
        uint p2 = VertexCache + R16(ref fr, f + 0x16u);
        uint p1 = VertexCache + R16(ref fr, f + 0x12u);
        if (!Facing(mem, p0, p1, p2)) return true;
        if (!Allocate(ref fr, 0x28u, out uint pkt)) return false;

        W16(ref fr, pkt + 0x0Eu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x1Au, Tpage<TB>(ref fr, f, abr));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x14u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x20u, mem.ReadU32(p2));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x18u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x24u, R16(ref fr, f + 8u));

        uint n0 = R16(ref fr, f + 0x0Cu), n1 = R16(ref fr, f + 0x10u), n2 = R16(ref fr, f + 0x14u);
        uint fog = (uint)(short)R16(ref fr, p0 + 6u);
        Shade3(ref fr, normals + n0, normals + n1, normals + n2, fog, pkt + 4u, pkt + 0x10u, pkt + 0x1Cu);

        W8(ref fr, pkt + 3u, 0x09);
        W8(ref fr, pkt + 7u, (byte)(TB.On ? 0x36u : cmd));
        if (fr.Lighting)
            LightGouraud(mem, pkt, LightWord(ref fr), fr.LightGen, 3, normals + n0, normals + n1, normals + n2, 0u, p0, p1, p2, 0u);
        RecordDepth(ref fr, pkt, 0x20u, 3, p0, p1, p2, 0u);

        Insert(ref fr, ((short)R16(ref fr, p0 + 4u) + (short)R16(ref fr, p1 + 4u) + (short)R16(ref fr, p2 + 4u)) / 3, bias, pkt);
        return true;
    }

    /// <summary>POLY_GT4: three normals in one call and the fourth in another.</summary>
    static bool GouraudQuad<TB>(ref Frame fr, uint f, uint cmd, uint bias, uint normals, uint abr) where TB : struct, IBlend
    {
        var mem = fr.Mem;
        uint p0 = VertexCache + R16(ref fr, f + 0x12u);
        uint p2 = VertexCache + R16(ref fr, f + 0x1Au);
        uint p1 = VertexCache + R16(ref fr, f + 0x16u);
        if (!Facing(mem, p0, p1, p2)) return true;
        if (!Allocate(ref fr, 0x34u, f + 0x1Eu, out uint pkt, out uint p3)) return false;

        W16(ref fr, pkt + 0x0Eu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x1Au, Tpage<TB>(ref fr, f, abr));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x14u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x20u, mem.ReadU32(p2));
        mem.WriteU32(pkt + 0x2Cu, mem.ReadU32(p3));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x18u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x24u, R16(ref fr, f + 8u));
        W16(ref fr, pkt + 0x30u, R16(ref fr, f + 0x0Cu));

        uint n0 = R16(ref fr, f + 0x10u), n1 = R16(ref fr, f + 0x14u), n2 = R16(ref fr, f + 0x18u);
        uint fog = (uint)(short)R16(ref fr, p0 + 6u);
        Shade3(ref fr, normals + n0, normals + n1, normals + n2, fog, pkt + 4u, pkt + 0x10u, pkt + 0x1Cu);
        Shade(ref fr, normals + R16(ref fr, f + 0x1Cu), fog, pkt + 0x28u);

        W8(ref fr, pkt + 3u, 0x0C);
        W8(ref fr, pkt + 7u, (byte)(TB.On ? 0x3Eu : cmd));
        if (fr.Lighting)
            LightGouraud(mem, pkt, LightWord(ref fr), fr.LightGen, 4, normals + n0, normals + n1, normals + n2,
                         normals + R16(ref fr, f + 0x1Cu), p0, p1, p2, p3);
        RecordDepth(ref fr, pkt, 0x2Cu, 4, p0, p1, p2, p3);

        Insert(ref fr, QuadDepth(ref fr, p0, p1, p2, p3), bias, pkt);
        return true;
    }

    /// <summary>The light colour the face was lit with, without a load the vertex map sees.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static uint LightWord(ref Frame fr) => fr.Hoisted ? fr.Light : Peek32(fr.Mem, LightColour);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static int QuadDepth(ref Frame fr, uint p0, uint p1, uint p2, uint p3) =>
        ((short)R16(ref fr, p0 + 4u) + (short)R16(ref fr, p1 + 4u)
       + (short)R16(ref fr, p2 + 4u) + (short)R16(ref fr, p3 + 4u)) >> 2;

    /// <summary>The face's +0x6 word; the blend twin puts its rate in bits 5-6.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static ushort Tpage<TB>(ref Frame fr, uint f, uint abr) where TB : struct, IBlend
    {
        uint w = R16(ref fr, f + 6u);
        return (ushort)(TB.On ? (w & 0xFF9Fu) | abr : w);
    }

    /// <summary>The allocator with the fourth vertex's index read where the game reads it.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Allocate(ref Frame fr, uint size, uint index, out uint pkt, out uint vertex)
    {
        var mem = fr.Mem;
        uint desc = fr.Hoisted ? fr.Desc : mem.ReadU32(PrimDescriptor);
        pkt = mem.ReadU32(desc + 8u);
        vertex = VertexCache + R16(ref fr, index);
        mem.WriteU32(desc + 8u, pkt + size);
        return Bump(ref fr, desc, pkt + size);
    }

    /// <summary>NormalColorDpq.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Shade(ref Frame fr, uint normal, uint fog, uint dest)
    {
        var mem = fr.Mem;
        Gte.Write(0, mem.ReadU32(normal));
        Gte.Write(1, mem.ReadU32(normal + 4u));
        Gte.Write(6, fr.Hoisted ? fr.Light : mem.ReadU32(LightColour));
        Gte.Write(8, fog);
        Gte.NcdsOp(12, true);
        mem.WriteU32(dest, Gte.Read(22));
    }

    /// <summary>NormalColorDpq3.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Shade3(ref Frame fr, uint n0, uint n1, uint n2, uint fog, uint d0, uint d1, uint d2)
    {
        var mem = fr.Mem;
        Gte.Write(0, mem.ReadU32(n0));
        Gte.Write(1, mem.ReadU32(n0 + 4u));
        Gte.Write(2, mem.ReadU32(n1));
        Gte.Write(3, mem.ReadU32(n1 + 4u));
        Gte.Write(4, mem.ReadU32(n2));
        Gte.Write(5, mem.ReadU32(n2 + 4u));
        Gte.Write(6, fr.Hoisted ? fr.Light : mem.ReadU32(LightColour));
        Gte.Write(8, fog);
        Gte.NcdtOp(12, true);
        mem.WriteU32(d0, Gte.Read(20));
        mem.WriteU32(d1, Gte.Read(21));
        mem.WriteU32(d2, Gte.Read(22));
    }

    /// <summary>A mean depth of zero or less is dropped, then Place's range test.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Insert(ref Frame fr, int z, uint bias, uint pkt)
    {
        if (z <= 0) return;
        Place(ref fr, (uint)z + bias, pkt);
    }
}
