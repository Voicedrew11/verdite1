using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using KingsField2 = Recompiled.KingsField2_game;

namespace Kf2;

/// <summary>
/// `func_80030540`, the polygon assembler, in C#: the same reads and stores in the
/// same order, with the registers in locals. Also its unclipped twin
/// `func_8002FECC`, which draws the far map tiles, the two vertex transforms
/// `func_8002E650` and `func_8002E7CC`, the HUD's transform `func_8002E910`
/// (PolyAssemblerHud.cs), and the models' lit assembler `func_8002F214`
/// with its semi-transparent twin `func_8002EAEC` (PolyAssemblerLit.cs).
///
///     KF2_POLYASM=0             all of them recompiled
///     KF2_POLYASM=verify        run both on every call and compare RAM, registers and the GTE
///     KF2_POLYASM_REJECT=0      send every oversized polygon to the clipper again
///     KF2_POLYASM_REJECT=replay a rejection makes the clipper's scratch writes too
///     KF2_POLYASM_UNCLIPPED=0   func_8002FECC recompiled
///     KF2_POLYASM_TRANSFORM=0   func_8002E650, func_8002E7CC and func_8002E910 recompiled
///     KF2_POLYASM_LIT=0         func_8002F214 and func_8002EAEC recompiled
///     KF2_POLYASM_CLIPPER=0     Clip4FTP and Clip3FTP recompiled
///
/// The libgte leaves they call (`NormalClip`, `NormalColorCol`, `DpqColor`,
/// `AddPrim`, `RotTransPers`) are inlined; the clipped-polygon emitter is still
/// called as recompiled code, and the clippers through their hooks
/// (PolyAssemblerClip.cs). See "The polygon assembler in C#"
/// in docs/PATCHES_AND_MODS.md.
/// </summary>
public static partial class PolyAssembler
{
    const uint Assembler = 0x80030540;
    const uint Unclipped = 0x8002FECC;
    const uint Transform = 0x8002E650;
    const uint NearTransform = 0x8002E7CC;

    const uint FogMode = 0x80192EA8;

    const uint VertexCache = 0x8018EB94;   // {sxy, otz, fog}, 8 bytes a vertex
    const uint VertexBase = 0x8018EAA0;
    const uint ModelTable = 0x8018E19C;
    const uint OtBase = 0x8018E0A8;
    const uint PrimDescriptor = 0x8017E0A4;
    const uint LightColour = 0x8006E604;
    const uint ClipOut = 0x80192A18;

    // The view-space clipper's state (InitClip, func_8005D7CC).
    const uint ClipRecords = 0x8006E7B0;
    const uint ClipXScale = 0x800FC97C;
    const uint ClipYScale = 0x800FC98C;
    const uint ClipFar = 0x8012E99C;
    const uint ClipNear = 0x8017E07C;

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;
    static bool _queued, _queuedUnclipped, _queuedTransform, _queuedNearTransform;
    static bool _reject = true;

    /// <summary>Off hands every call to the recompiled routine.</summary>
    public static bool Enabled { get; set; } = true;

    /// <summary>Video ▸ Fast geometry: the assembler hooks and the GTE fast path together.</summary>
    public const string FastGeometryKey = "kf2.fastgeometry.on";

    /// <summary>KF2_POLYASM or KF2_GTE_FAST set: the env wins over the saved setting.</summary>
    static bool _forced;

    public static bool FastGeometry => Enabled && Gte.FastLighting;

    public static void SetFastGeometry(bool on)
    {
        Enabled = on;
        Gte.FastLighting = on;
        ZBuffer.SyncSource();
    }

    public static bool UnclippedEnabled { get; set; } = true;
    public static bool TransformEnabled { get; set; } = true;

    public static bool RejectEnabled
    {
        get => _reject;
        set => _reject = value;
    }

    /// <summary>A rejection also writes the records and lists the clipper would have
    /// left behind, which nothing reads before the clipper writes them again.</summary>
    public static bool ReplayRejection { get; set; }

    public static bool Verifying => _mode == Mode.Verify;

    /// <summary>Running totals; never reset.</summary>
    public static long AssemblerCalls, ClipperCalls, Rejections, UnclippedCalls, TransformCalls;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.polyasm",
        Name = "Polygon assembler",
        Version = "1.0",
        Description = "func_80030540, func_8002FECC, func_8002E650, func_8002E7CC, func_8002E910, func_8002F214, func_8002EAEC and the view-space clipper in C#.",
    };

    public static void Configure(string? mode, string? reject, string? unclipped, string? transform, string? lit,
                                 string? clipper)
    {
        ClipperEnabled = clipper?.Trim() != "0";
        _reject = reject?.Trim() != "0";
        ReplayRejection = reject?.Trim().ToLowerInvariant() == "replay";
        _mode = mode?.Trim().ToLowerInvariant() == "verify" ? Mode.Verify : Mode.On;
        Enabled = mode?.Trim().ToLowerInvariant() is not ("0" or "off");
        UnclippedEnabled = unclipped?.Trim() != "0";
        TransformEnabled = transform?.Trim() != "0";
        LitEnabled = lit?.Trim() != "0";
        _forced = !string.IsNullOrWhiteSpace(mode)
               || !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("KF2_GTE_FAST"));
    }

    // Installed whatever the env says, so the setting can switch it on mid-session.
    public static void Install()
    {
        // The config is only readable from RuntimeReadyEvent; see Subpixel.Install.
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            if (!_forced) SetFastGeometry(RecompOne.Runtime.Runtime.View.GetBool(FastGeometryKey, true));
        });
        HookAttach.OnOverlayLoad("polyasm", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var assembler = SymbolRegistry.Resolve("game", null, Assembler);
        var unclipped = SymbolRegistry.Resolve("game", null, Unclipped);
        var transform = SymbolRegistry.Resolve("game", null, Transform);
        var near = SymbolRegistry.Resolve("game", null, NearTransform);
        var hud = SymbolRegistry.Resolve("game", null, HudTransform);
        var lit = SymbolRegistry.Resolve("game", null, Lit);
        var litBlend = SymbolRegistry.Resolve("game", null, LitBlend);
        var clip4 = SymbolRegistry.Resolve("game", null, Clip4);
        var clip3 = SymbolRegistry.Resolve("game", null, Clip3);
        var nclip = SymbolRegistry.Resolve("game", null, NormalClipAddress);
        if (assembler == null || unclipped == null || transform == null || near == null || hud == null || lit == null || litBlend == null
            || clip4 == null || clip3 == null || nclip == null)
            return false;

        if (!Queue(ref _queued, assembler, nameof(Replace))) return false;
        if (!Queue(ref _queuedUnclipped, unclipped, nameof(ReplaceUnclipped))) return false;
        if (!Queue(ref _queuedTransform, transform, nameof(ReplaceTransform))) return false;
        if (!Queue(ref _queuedNearTransform, near, nameof(ReplaceNearTransform))) return false;
        if (!Queue(ref _queuedHudTransform, hud, nameof(ReplaceHudTransform))) return false;
        if (!Queue(ref _queuedLit, lit, nameof(ReplaceLit))) return false;
        if (!Queue(ref _queuedLitBlend, litBlend, nameof(ReplaceLitBlend))) return false;
        if (!Queue(ref _queuedClip4, clip4, nameof(ReplaceClip4))) return false;
        if (!Queue(ref _queuedClip3, clip3, nameof(ReplaceClip3))) return false;
        if (!Queue(ref _queuedNormalClip, nclip, nameof(ReplaceNormalClip))) return false;

        HookManager.Commit();
        bool ok = HookAttach.Installed(assembler) && HookAttach.Installed(unclipped) && HookAttach.Installed(transform)
               && HookAttach.Installed(near) && HookAttach.Installed(hud) && HookAttach.Installed(lit) && HookAttach.Installed(litBlend)
               && HookAttach.Installed(clip4) && HookAttach.Installed(clip3) && HookAttach.Installed(nclip);
        string State(bool on) => !on ? "off" : _mode.ToString().ToLowerInvariant();
        Console.WriteLine(!ok
            ? "[KF2] polyasm: not installed"
            : $"[KF2] polyasm: assembler {State(Enabled)}" +
              $"{(_reject ? ReplayRejection ? " (clipper rejection on, replayed)" : " (clipper rejection on)" : "")}, " +
              $"unclipped {State(Enabled && UnclippedEnabled)}, transforms {State(Enabled && TransformEnabled)}, " +
              $"lit {State(Enabled && LitEnabled)}, clipper {State(Enabled && ClipperEnabled)}; GTE fast path {(Gte.FastLighting ? "on" : "off")}, " +
              $"light cache {(Gte.LightCache ? "on" : "off")}");
        return ok;
    }

    static bool Queue(ref bool queued, System.Reflection.MethodInfo target, string method)
    {
        if (queued) return true;
        var impl = typeof(PolyAssembler).GetMethod(method,
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
        queued = HookManager.AddReplace(_self, target, impl);
        return queued;
    }

    // PGXP follows values through the registers, which locals do not have.
    static bool Recompiled(bool on) => !on || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking;

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_assemblerCheck, orig, c, mem, Run);
        else Run(c, mem);
    }

    static void ReplaceUnclipped(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && UnclippedEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_unclippedCheck, orig, c, mem, RunUnclipped);
        else RunUnclipped(c, mem);
    }

    static void ReplaceTransform(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && TransformEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_transformCheck, orig, c, mem, RunTransform);
        else RunTransform(c, mem);
    }

    static void ReplaceNearTransform(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && TransformEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_nearTransformCheck, orig, c, mem, RunNearTransform);
        else RunNearTransform(c, mem);
    }

    /// <summary>The registers an epilogue puts back.</summary>
    readonly struct Saved
    {
        public readonly uint SP;
        readonly uint _ra, _fp, _s0, _s1, _s2, _s3, _s4, _s5, _s6, _s7;

        public Saved(CpuContext c)
        {
            SP = c.SP; _ra = c.RA; _fp = c.FP;
            _s0 = c.S0; _s1 = c.S1; _s2 = c.S2; _s3 = c.S3;
            _s4 = c.S4; _s5 = c.S5; _s6 = c.S6; _s7 = c.S7;
        }

        public void Restore(CpuContext c)
        {
            c.RA = _ra; c.SP = SP; c.FP = _fp;
            c.S0 = _s0; c.S1 = _s1; c.S2 = _s2; c.S3 = _s3;
            c.S4 = _s4; c.S5 = _s5; c.S6 = _s6; c.S7 = _s7;
        }
    }

    // ---- What a call may skip ----------------------------------------------

    /// <summary>A 16-bit access below this physical address is inside RAM, so it can
    /// go straight to the array.</summary>
    static uint DirectLimit => RecompOne.Runtime.Runtime.RamSize - 1u;

    /// <summary>
    /// A 16- or 8-bit load or store never reaches GteVertexMap, so while PSMemory
    /// would do nothing else with one (<see cref="PSMemory.DirectRam"/>) it goes
    /// straight into RAM. The words at LightColour, OtBase and PrimDescriptor are
    /// read once rather than per face while the vertex map has nothing bound to them,
    /// since a load of an unbound word has no effect at all. Only an interrupt
    /// handler can change either answer mid-call, so both are taken again whenever a
    /// poll could have run one.
    /// </summary>
    ref struct Frame
    {
        public readonly PSMemory Mem;
        public readonly ref byte Ram;
        public uint Lim, Epoch, Light, Ot, Desc;
        public bool Hoisted;
        // Per-pixel lighting wants this call's packets recorded, and the BK/LCM
        // generation they are lit with.
        public readonly bool Lighting;
        public int LightGen;
        // The depth buffer wants packet depths, and this call's are recorded.
        public readonly bool DepthTable, Depth;

        public Frame(PSMemory mem)
        {
            Mem = mem;
            Ram = ref Unsafe.AsRef(in MemoryMarshal.GetReference(mem.Ram));
            Lighting = LightingOn();
            DepthTable = GtePacketDepth.Active;
            Depth = DepthOn();
            Refresh();
        }

        public void Refresh()
        {
            Epoch = Interrupts.SlowPolls;
            bool direct = Mem.DirectRam;
            Lim = direct ? DirectLimit : 0u;
            Hoisted = direct && !(GteVertexMap.Active &&
                (Bound(LightColour) || Bound(OtBase) || Bound(PrimDescriptor)));
            if (!Hoisted) return;
            Light = Mem.ReadU32(LightColour);
            Ot = Mem.ReadU32(OtBase);
            Desc = Mem.ReadU32(PrimDescriptor);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Check()
        {
            if (Interrupts.SlowPolls != Epoch) Refresh();
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Bound(uint address) => GteVertexMap.MaybeBound(address & 0x1FFFFFFFu);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static ushort R16(ref Frame fr, uint a)
    {
        uint o = a & 0x1FFFFFFFu;
        return o < fr.Lim ? Unsafe.ReadUnaligned<ushort>(ref Unsafe.Add(ref fr.Ram, (nint)o)) : SlowR16(fr.Mem, a);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void W16(ref Frame fr, uint a, ushort v)
    {
        uint o = a & 0x1FFFFFFFu;
        if (o < fr.Lim) Unsafe.WriteUnaligned(ref Unsafe.Add(ref fr.Ram, (nint)o), v);
        else SlowW16(fr.Mem, a, v);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void W8(ref Frame fr, uint a, byte v)
    {
        uint o = a & 0x1FFFFFFFu;
        if (o < fr.Lim) Unsafe.Add(ref fr.Ram, (nint)o) = v;
        else SlowW8(fr.Mem, a, v);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static ushort SlowR16(PSMemory mem, uint a) => mem.ReadU16(a);

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void SlowW16(PSMemory mem, uint a, ushort v) => mem.WriteU16(a, v);

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void SlowW8(PSMemory mem, uint a, byte v) => mem.WriteU8(a, v);

    // ---- func_80030540 -------------------------------------------------------

    static void Run(CpuContext c, PSMemory mem)
    {
        var saved = new Saved(c);
        AssemblerCalls++;
        try { Body(c, mem, saved.SP - 0xA8u); }
        finally { saved.Restore(c); }
    }

    static void Body(CpuContext c, PSMemory mem, uint sp)
    {
        c.SP = sp;
        uint bias = c.A1, mesh = c.A2;
        uint header, normals;

        if (mesh != 0)
        {
            normals = mesh + mem.ReadU32(mesh + 0x14u) + 0xCu;
            mem.WriteU32(VertexBase, mesh + mem.ReadU32(mesh + 0xCu) + 0xCu);
            header = mesh + 0xCu;
        }
        else
        {
            uint index = c.A0 & 0xFFFFu;
            header = index * 28u + 0xCu + mem.ReadU32(ModelTable);
            normals = mem.ReadU32(header + 8u) + 0xCu + mem.ReadU32(ModelTable);
        }

        c.A0 = mem.ReadU32(header + 4u);
        uint verts = mem.ReadU32(VertexBase);
        c.S0 = mesh; c.S2 = header; c.S7 = verts;
        c.RA = 0x800305DCu;
        _cacheSerial++;
        KingsField2.func_8002E7CC(c, mem);

        uint face = mem.ReadU32(header + 0x10u) + 0xCu;
        face += mesh != 0 ? mesh : mem.ReadU32(ModelTable);

        uint count = mem.ReadU32(header + 0x14u);
        uint total = count;
        // 0072. A model the object walk submitted, every face, for the retained scene.
        if (mesh == 0 && RetainedModels.Capturing) RetainedModels.CaptureFlat(mem, header, normals, face, count, true);

        var fr = new Frame(mem);
        for (; count != 0; count--)
        {
            Interrupts.Poll(c, mem);
            fr.Check();
            if (Remaster.Faces.Wanted) Remaster.Faces.Enter(mesh != 0, (int)(total - count));
            uint word = mem.ReadU32(face);
            face += 4u;
            uint cmd = word >> 24;
            uint type = cmd & 0xFDu;

            if (type == 0x2Cu)
            {
                if (!Quad(c, ref fr, sp, face, cmd, word, bias, normals, verts)) return;
            }
            else if (type == 0x24u)
            {
                if (!Triangle(c, ref fr, sp, face, cmd, word, bias, normals, verts)) return;
            }

            face += (word >> 6) & 0x3FCu;
        }
    }

    /// <summary>False when the primitive buffer ran out, which ends the call.</summary>
    static bool Quad(CpuContext c, ref Frame fr, uint sp, uint f, uint cmd, uint word,
                     uint bias, uint normals, uint verts)
    {
        uint i0 = R16(ref fr, f + 0x12u), i1 = R16(ref fr, f + 0x14u);
        uint i2 = R16(ref fr, f + 0x16u), i3 = R16(ref fr, f + 0x18u);
        uint p0 = VertexCache + i0, p1 = VertexCache + i1, p2 = VertexCache + i2, p3 = VertexCache + i3;

        int y0 = (short)R16(ref fr, p0 + 2u), y1 = (short)R16(ref fr, p1 + 2u);
        int y2 = (short)R16(ref fr, p2 + 2u), y3 = (short)R16(ref fr, p3 + 2u);
        int x0 = (short)R16(ref fr, p0), x1 = (short)R16(ref fr, p1);
        int x2 = (short)R16(ref fr, p2), x3 = (short)R16(ref fr, p3);
        short z = (short)(R16(ref fr, p0 + 4u) | R16(ref fr, p1 + 4u) | R16(ref fr, p2 + 4u) | R16(ref fr, p3 + 4u));

        if (z == -1 ||
            !FitsY(y0 - y1) || !FitsY(y1 - y3) || !FitsY(y3 - y2) || !FitsY(y2 - y0) || !FitsY(y1 - y2) ||
            !FitsX(x0 - x1) || !FitsX(x1 - x3) || !FitsX(x3 - x2) || !FitsX(x2 - x0) || !FitsX(x1 - x2))
        {
            var mem = fr.Mem;
            uint a0 = verts + i0, a1 = verts + i1, a2 = verts + i2, a3 = verts + i3;
            // Clip4FT builds its records in the order a0, a1, a3, a2.
            if (_reject && Reject(c, mem, 4, a0, a1, a3, a2, f, f + 4u, f + 0xCu, f + 8u)) return true;

            mem.WriteU32(sp + 0x10u, f);
            mem.WriteU32(sp + 0x14u, f + 4u);
            mem.WriteU32(sp + 0x18u, f + 8u);
            mem.WriteU32(sp + 0x1Cu, f + 0xCu);
            mem.WriteU32(sp + 0x20u, ClipOut);
            c.A0 = a0;
            c.A1 = a1;
            c.A2 = a2;
            c.A3 = a3;
            c.RA = 0x80030948u;
            _quadClips++;
            ClipperCalls++;
            KingsField2.Clip4FTP(c, mem);
            Clipped(c, mem, sp, f, word, bias, normals, mem.ReadU16(f + 0x10u), 4);
            // Recompiled code ran; take nothing it could have moved on trust.
            fr.Refresh();
            return true;
        }

        if (!QuadFaces(fr.Mem, Visible(fr.Mem, p0, p1, p2), p0, p1, p2, p3)) return true;
        if (!Allocate(ref fr, 0x34u, out uint pkt)) { _exhausted++; return false; }

        Link(ref fr, (uint)(FillQuad(ref fr, pkt, f, cmd, normals, p0, p1, p2, p3) >> 2) + bias, pkt);
        return true;
    }

    /// <summary>A POLY_GT4 from four cached vertices, as both assemblers write it.
    /// Returns the sum of the four otz words.</summary>
    static int FillQuad(ref Frame fr, uint pkt, uint f, uint cmd, uint normals, uint p0, uint p1, uint p2, uint p3)
    {
        var mem = fr.Mem;
        W16(ref fr, pkt + 0xEu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x1Au, R16(ref fr, f + 6u));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x14u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x20u, mem.ReadU32(p2));
        mem.WriteU32(pkt + 0x2Cu, mem.ReadU32(p3));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x18u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x24u, R16(ref fr, f + 8u));
        W16(ref fr, pkt + 0x30u, R16(ref fr, f + 0xCu));

        uint normal = normals + R16(ref fr, f + 0x10u);
        uint colour = Light(ref fr, normal);
        uint c0 = colour, c1 = colour, c2 = colour, c3 = colour;
        if (_tileLight) TileColours(mem, normal, colour, 4, p0, p1, p2, p3, out c0, out c1, out c2, out c3);
        Fog(ref fr, c0, p0, pkt + 0x04u);
        Fog(ref fr, c1, p1, pkt + 0x10u);
        Fog(ref fr, c2, p2, pkt + 0x1Cu);
        Fog(ref fr, c3, p3, pkt + 0x28u);

        W8(ref fr, pkt + 3u, 0x0C);
        W8(ref fr, pkt + 7u, (byte)((cmd & 2u) | 0x3Cu));
        if (fr.Lighting) LightTile(mem, pkt, LightWord(ref fr), c0, c1, c2, c3, 4, p0, p1, p2, p3);
        RecordDepth(ref fr, pkt, 0x2Cu, 4, p0, p1, p2, p3);

        return (short)R16(ref fr, p0 + 4u) + (short)R16(ref fr, p1 + 4u)
             + (short)R16(ref fr, p3 + 4u) + (short)R16(ref fr, p2 + 4u);
    }

    static bool Triangle(CpuContext c, ref Frame fr, uint sp, uint f, uint cmd, uint word,
                         uint bias, uint normals, uint verts)
    {
        uint i0 = R16(ref fr, f + 0x0Eu), i2 = R16(ref fr, f + 0x12u), i1 = R16(ref fr, f + 0x10u);
        uint p0 = VertexCache + i0, p2 = VertexCache + i2, p1 = VertexCache + i1;

        int y0 = (short)R16(ref fr, p0 + 2u), y2 = (short)R16(ref fr, p2 + 2u), y1 = (short)R16(ref fr, p1 + 2u);
        int x0 = (short)R16(ref fr, p0), x1 = (short)R16(ref fr, p1), x2 = (short)R16(ref fr, p2);
        short z = (short)(R16(ref fr, p0 + 4u) | R16(ref fr, p1 + 4u) | R16(ref fr, p2 + 4u));

        if (z == -1 ||
            !FitsY(y0 - y1) || !FitsY(y1 - y2) || !FitsY(y2 - y0) ||
            !FitsX(x0 - x1) || !FitsX(x1 - x2) || !FitsX(x2 - x0))
        {
            var mem = fr.Mem;
            uint a0 = verts + i0, a1 = verts + i1, a2 = verts + i2;
            if (_reject && Reject(c, mem, 3, a0, a1, a2, 0u, f, f + 4u, f + 8u, 0u)) return true;

            mem.WriteU32(sp + 0x10u, f + 4u);
            mem.WriteU32(sp + 0x14u, f + 8u);
            mem.WriteU32(sp + 0x18u, ClipOut);
            c.A0 = a0;
            c.A1 = a1;
            c.A2 = a2;
            c.A3 = f;
            c.RA = 0x80030BF4u;
            _triClips++;
            ClipperCalls++;
            KingsField2.Clip3FTP(c, mem);
            Clipped(c, mem, sp, f, word, bias, normals, mem.ReadU16(f + 0x0Cu), 3);
            // Recompiled code ran; take nothing it could have moved on trust.
            fr.Refresh();
            return true;
        }

        if (!Visible(fr.Mem, p0, p1, p2)) return true;
        if (!Allocate(ref fr, 0x28u, out uint pkt)) { _exhausted++; return false; }

        Link(ref fr, (uint)(FillTriangle(ref fr, pkt, f, cmd, normals, p0, p1, p2) / 3) + bias, pkt);
        return true;
    }

    /// <summary>A POLY_GT3, as both assemblers write it. Returns the otz sum.</summary>
    static int FillTriangle(ref Frame fr, uint pkt, uint f, uint cmd, uint normals, uint p0, uint p1, uint p2)
    {
        var mem = fr.Mem;
        W16(ref fr, pkt + 0xEu, R16(ref fr, f + 2u));
        W16(ref fr, pkt + 0x1Au, R16(ref fr, f + 6u));
        mem.WriteU32(pkt + 0x08u, mem.ReadU32(p0));
        mem.WriteU32(pkt + 0x14u, mem.ReadU32(p1));
        mem.WriteU32(pkt + 0x20u, mem.ReadU32(p2));
        W16(ref fr, pkt + 0x0Cu, R16(ref fr, f));
        W16(ref fr, pkt + 0x18u, R16(ref fr, f + 4u));
        W16(ref fr, pkt + 0x24u, R16(ref fr, f + 8u));

        uint normal = normals + R16(ref fr, f + 0x0Cu);
        uint colour = Light(ref fr, normal);
        uint c0 = colour, c1 = colour, c2 = colour;
        if (_tileLight) TileColours(mem, normal, colour, 3, p0, p1, p2, 0u, out c0, out c1, out c2, out _);
        Fog(ref fr, c0, p0, pkt + 0x04u);
        Fog(ref fr, c1, p1, pkt + 0x10u);
        Fog(ref fr, c2, p2, pkt + 0x1Cu);

        W8(ref fr, pkt + 3u, 0x09);
        W8(ref fr, pkt + 7u, (byte)((cmd & 2u) | 0x34u));
        if (fr.Lighting) LightTile(mem, pkt, LightWord(ref fr), c0, c1, c2, c0, 3, p0, p1, p2, 0u);
        RecordDepth(ref fr, pkt, 0x20u, 3, p0, p1, p2, 0u);

        return (short)R16(ref fr, p0 + 4u) + (short)R16(ref fr, p1 + 4u) + (short)R16(ref fr, p2 + 4u);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool FitsY(int d) => (uint)d + 0x1FFu < 0x3FFu;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool FitsX(int d) => (uint)d + 0x3FFu < 0x7FFu;

    /// <summary>NormalClip: backface cull on the three cached screen words.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Visible(PSMemory mem, uint p0, uint p1, uint p2)
    {
        uint w0 = mem.ReadU32(p0), w1 = mem.ReadU32(p1), w2 = mem.ReadU32(p2);
        Gte.Write(12, w0);
        Gte.Write(14, w2);
        Gte.Write(13, w1);
        Gte.Nclip();
        bool whole = (int)Gte.Read(24) > 0;
        return Subpixel.Cull && GteDepth.Subpixel && _mode != Mode.Verify ? FacingFractional(whole, p0, w0, p1, w1, p2, w2) : whole;
    }

    /// <summary>The bump allocator. The game re-reads the descriptor and the pointer it
    /// just stored; while neither is bound that is the value it stored.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Allocate(ref Frame fr, uint size, out uint pkt)
    {
        var mem = fr.Mem;
        uint desc = fr.Hoisted ? fr.Desc : mem.ReadU32(PrimDescriptor);
        pkt = mem.ReadU32(desc + 8u);
        mem.WriteU32(desc + 8u, pkt + size);
        return Bump(ref fr, desc, pkt + size);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Bump(ref Frame fr, uint desc, uint cur)
    {
        var mem = fr.Mem;
        if (!fr.Hoisted || (GteVertexMap.Active && Bound(desc + 8u)))
        {
            desc = mem.ReadU32(PrimDescriptor);
            cur = mem.ReadU32(desc + 8u);
        }
        return mem.ReadU32(desc + 4u) >= cur;
    }

    /// <summary>NormalColorCol. The game stores the result on its stack and reads it
    /// straight back; nothing else sees that word.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static uint Light(ref Frame fr, uint normal)
    {
        var mem = fr.Mem;
        Gte.Write(0, mem.ReadU32(normal));
        Gte.Write(1, mem.ReadU32(normal + 4u));
        Gte.Write(6, fr.Hoisted ? fr.Light : mem.ReadU32(LightColour));
        Gte.NccsOp(12, true);
        return Gte.Read(22);
    }

    /// <summary>DpqColor.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Fog(ref Frame fr, uint colour, uint vertex, uint dest)
    {
        Gte.Write(6, colour);
        Gte.Write(8, (uint)(short)R16(ref fr, vertex + 6u));
        Gte.Dpcs(12, false);
        fr.Mem.WriteU32(dest, Gte.Read(22));
    }

    /// <summary>The ordering-table slot, clamped as the game does, then AddPrim.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Link(ref Frame fr, uint otz, uint pkt)
    {
        if ((int)otz < 16) otz = 16;
        AddPrim(fr.Mem, (fr.Hoisted ? fr.Ot : fr.Mem.ReadU32(OtBase)) + ((otz & 0x1FFFu) << 2), pkt);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void AddPrim(PSMemory mem, uint slot, uint pkt)
    {
        uint tag = mem.ReadU32(pkt);
        uint next = mem.ReadU32(slot);
        mem.WriteU32(pkt, (tag & 0xFF000000u) | (next & 0x00FFFFFFu));
        next = mem.ReadU32(slot);
        mem.WriteU32(slot, (next & 0xFF000000u) | (pkt & 0x00FFFFFFu));
    }

    /// <summary>After a clipper: hand what survived to func_800302E8.</summary>
    static void Clipped(CpuContext c, PSMemory mem, uint sp, uint f, uint word, uint bias, uint normals, uint normal, int corners)
    {
        uint n = c.V0;
        if ((int)n < 3) return;
        _emitted++;

        c.A0 = n;
        c.A2 = mem.ReadU16(f + 2u);
        c.A3 = mem.ReadU16(f + 6u);
        mem.WriteU32(sp + 0x14u, bias);
        mem.WriteU32(sp + 0x10u, (word >> 24) & 2u);
        c.A1 = normals + normal;
        c.RA = 0x80030C38u;
        _clipFacing = _mode == Mode.Verify ? 0 : ClippedFacing(mem, n);
        bool lighting = LightingOn();
        // Verify compares against the recompiled assembler, which fogs at half.
        bool refog = EvenFog.Enabled && _mode != Mode.Verify;
        bool rewrite = refog || _tileLight;
        bool depth = GtePacketDepth.Active;
        bool texRect = GteTexRect.Active;
        uint before = lighting || rewrite || depth || texRect ? Peek32(mem, Peek32(mem, PrimDescriptor) + 8u) : 0u;
        try { KingsField2.func_800302E8(c, mem); }
        finally { _clipFacing = 0; }
        if (rewrite) RewriteClipped(mem, before, normals + normal, refog);
        if (lighting) LightClipped(mem, before, normals + normal, refog);
        if (depth) DepthClipped(mem, before, DepthOn());
        if (texRect) TexRectClipped(mem, before, f, corners);
    }


    // ---- Rejecting what the clipper would clip to nothing ------------------

    /// <summary>
    /// ClipFT runs six Sutherland-Hodgman passes in a fixed order. A pass whose
    /// vertices are all inside passes them through untouched, so the first pass
    /// that is not all-inside sees the original vertices: if they are all outside
    /// it, the polygon clips to nothing. The clipper's records and lists are scratch
    /// it writes before it reads, so only <see cref="ReplayRejection"/> writes them.
    /// </summary>
    static bool Reject(CpuContext c, PSMemory mem, int n, uint v0, uint v1, uint v2, uint v3,
                       uint uv0, uint uv1, uint uv2, uint uv3)
    {
        var ram = mem.Ram;
        uint mask = (uint)ram.Length - 1u;
        Span<uint> src = [v0, v1, v2, v3];
        for (int i = 0; i < n; i++)
            if ((src[i] & 0x1FFFFFFFu) > mask - 8u) return false;

        Span<int> x = stackalloc int[4], y = stackalloc int[4], z = stackalloc int[4];
        Span<int> xb = stackalloc int[4], yb = stackalloc int[4];
        int kx = (int)Word(ram, ClipXScale & mask), ky = (int)Word(ram, ClipYScale & mask);
        int far = (int)Word(ram, ClipFar & mask), near = (int)Word(ram, ClipNear & mask);

        // RotTrans in the clipper's own record order, so the GTE ends where it would.
        // Each vertex's bits say which of the six planes it is inside, in pass order.
        int all = 0x3F, any = 0;
        for (int i = 0; i < n; i++)
        {
            uint a = src[i] & mask;
            Gte.Write(0, Word(ram, a));
            Gte.Write(1, Word(ram, a + 4u));
            Gte.MvmvaOp(12, false, 0, 0, 0);
            int xi = x[i] = (int)Gte.Read(25);
            int yi = y[i] = (int)Gte.Read(26);
            int zi = z[i] = (int)Gte.Read(27);
            int xbi = xb[i] = (int)((long)zi * kx) >> 12;
            int ybi = yb[i] = (int)((long)zi * ky) >> 12;
            int inside = (zi < far ? 1 : 0)
                       | (zi >= near ? 2 : 0)
                       | (yi >= (int)(0u - (uint)ybi) ? 4 : 0)
                       | (ybi >= yi ? 8 : 0)
                       | (xi >= (int)(0u - (uint)xbi) ? 16 : 0)
                       | (xbi >= xi ? 32 : 0);
            all &= inside;
            any |= inside;
        }

        int notAllInside = ~all & 0x3F;
        if (notAllInside == 0) return false;
        int pass = BitOperations.TrailingZeroCount(notAllInside);
        if (((any >> pass) & 1) != 0) return false;

        _rejected++;
        Rejections++;

        if (!ReplayRejection)
        {
            // The same polls, so an interrupt lands where it would have.
            for (int k = n * (pass + 2); k != 0; k--) Interrupts.Poll(c, mem);
            return true;
        }

        Span<uint> uv = [uv0, uv1, uv2, uv3];

        // Clip4FT / Clip3FT: copy the SVECTORs and UVs into records, transform, bound.
        for (int i = 0; i < n; i++)
            Copy8(mem, src[i], mem.ReadU32(ClipRecords) + 0x2Cu * (uint)i);
        uint recs = mem.ReadU32(ClipRecords);
        for (int i = 0; i < n; i++)
            mem.WriteU16(recs + 0x20u + 0x2Cu * (uint)i, mem.ReadU16(uv[i]));

        for (int i = 0; i < n; i++)
        {
            Interrupts.Poll(c, mem);
            uint rec = mem.ReadU32(ClipRecords) + 0x2Cu * (uint)i;
            mem.ReadU32(rec);
            mem.ReadU32(rec + 4u);
            mem.WriteU32(rec + 0x08u, (uint)x[i]);
            mem.WriteU32(rec + 0x0Cu, (uint)y[i]);
            mem.WriteU32(rec + 0x10u, (uint)z[i]);
            mem.ReadU32(ClipRecords);
            mem.ReadU32(ClipXScale);
            mem.ReadU32(rec + 0x10u);
            mem.ReadU32(rec + 0x10u);
            mem.ReadU32(ClipYScale);
            mem.WriteU32(rec + 0x24u, (uint)xb[i]);
            mem.WriteU32(rec + 0x28u, (uint)yb[i]);
            mem.WriteU32(ClipOut + 4u * (uint)i, rec);
        }

        // ClipFT: the passes that let everything through, then the one that drops it.
        for (int p = 0; p <= pass; p++)
        {
            uint list = (p & 1) == 0 ? ClipOut : ClipOut + 0x28u;
            uint outList = (p & 1) == 0 ? ClipOut + 0x28u : ClipOut;
            bool keep = p < pass;
            (uint first, uint second) = p switch
            {
                0 or 1 => (0x10u, 0u),
                2 => (0x28u, 0x0Cu),
                3 => (0x0Cu, 0x28u),
                4 => (0x24u, 0x08u),
                _ => (0x08u, 0x24u),
            };

            for (int j = 0; j < n; j++)
            {
                Interrupts.Poll(c, mem);
                uint t0 = mem.ReadU32(list + 4u * (uint)j);
                uint next = mem.ReadU32(j + 1 == n ? list : list + 4u * (uint)(j + 1));
                mem.ReadU32(t0 + first);
                mem.ReadU32(p switch { 0 => ClipFar, 1 => ClipNear, _ => t0 + second });
                if (keep) mem.WriteU32(outList + 4u * (uint)j, t0);
                mem.ReadU32(next + first);
                if (p >= 2) mem.ReadU32(next + second);
            }
        }
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static uint Word(ReadOnlySpan<byte> ram, uint offset) => MemoryMarshal.Read<uint>(ram[(int)offset..]);

    /// <summary>The lwl/lwr pairs Clip4FT copies an SVECTOR with.</summary>
    static void Copy8(PSMemory mem, uint from, uint to)
    {
        uint w = mem.ReadWordLeft(0u, from + 3u);
        w = mem.ReadWordRight(w, from);
        uint w2 = mem.ReadWordLeft(0u, from + 7u);
        w2 = mem.ReadWordRight(w2, from + 4u);
        mem.WriteWordLeft(to + 3u, w);
        mem.WriteWordRight(to, w);
        mem.WriteWordLeft(to + 7u, w2);
        mem.WriteWordRight(to + 4u, w2);
    }

    // ---- func_8002FECC: the assembler with no clipper ------------------------

    static void RunUnclipped(CpuContext c, PSMemory mem)
    {
        var saved = new Saved(c);
        UnclippedCalls++;
        try { UnclippedBody(c, mem, saved.SP - 0x50u); }
        finally { saved.Restore(c); }
    }

    static void UnclippedBody(CpuContext c, PSMemory mem, uint sp)
    {
        c.SP = sp;
        uint header = (c.A0 & 0xFFFFu) * 28u + 0xCu + mem.ReadU32(ModelTable);
        c.A0 = mem.ReadU32(header + 4u);
        uint normals = mem.ReadU32(header + 8u) + 0xCu + mem.ReadU32(ModelTable);
        c.S0 = header; c.S2 = ModelTable;
        c.RA = 0x8002FF28u;
        _cacheSerial++;
        KingsField2.func_8002E650(c, mem);

        uint count = mem.ReadU32(header + 0x14u);
        uint face = mem.ReadU32(header + 0x10u) + 0xCu + mem.ReadU32(ModelTable);
        uint total = count;
        if (RetainedModels.Capturing) RetainedModels.CaptureFlat(mem, header, normals, face, count, false);

        var fr = new Frame(mem);
        for (; count != 0; count--)
        {
            Interrupts.Poll(c, mem);
            fr.Check();
            if (Remaster.Faces.Wanted) Remaster.Faces.Enter(false, (int)(total - count));
            uint word = mem.ReadU32(face);
            face += 4u;
            uint cmd = word >> 24;
            uint type = cmd & 0xFDu;

            if (type == 0x24u)
            {
                uint p0 = VertexCache + R16(ref fr, face + 0x0Eu);
                uint p2 = VertexCache + R16(ref fr, face + 0x12u);
                uint p1 = VertexCache + R16(ref fr, face + 0x10u);
                if (Facing(mem, p0, p1, p2))
                {
                    if (!Allocate(ref fr, 0x28u, out uint pkt)) { _unclippedExhausted++; return; }
                    Place(ref fr, (uint)(FillTriangle(ref fr, pkt, face, cmd, normals, p0, p1, p2) / 3) + 0xF0u, pkt);
                }
            }
            else if (type == 0x2Cu)
            {
                uint p0 = VertexCache + R16(ref fr, face + 0x12u);
                uint p2 = VertexCache + R16(ref fr, face + 0x16u);
                uint p1 = VertexCache + R16(ref fr, face + 0x14u);
                if (QuadFaces(mem, Facing(mem, p0, p1, p2), p0, p1, p2, VertexCache + R16(ref fr, face + 0x18u)))
                {
                    if (!Allocate(ref fr, 0x34u, out uint pkt)) { _unclippedExhausted++; return; }
                    uint p3 = VertexCache + R16(ref fr, face + 0x18u);
                    Place(ref fr, (uint)(FillQuad(ref fr, pkt, face, cmd, normals, p0, p1, p2, p3) >> 2) + 0xF0u, pkt);
                }
            }

            face += (word >> 6) & 0x3FCu;
        }
    }

    /// <summary>NormalClip as this routine loads it: the third vertex before the second.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool Facing(PSMemory mem, uint p0, uint p1, uint p2)
    {
        uint w0 = mem.ReadU32(p0), w2 = mem.ReadU32(p2), w1 = mem.ReadU32(p1);
        Gte.Write(12, w0);
        Gte.Write(14, w2);
        Gte.Write(13, w1);
        Gte.Nclip();
        bool whole = (int)Gte.Read(24) > 0;
        return Subpixel.Cull && GteDepth.Subpixel && _mode != Mode.Verify ? FacingFractional(whole, p0, w0, p1, w1, p2, w2) : whole;
    }

    /// <summary>Faces culled and kept that the whole-pixel test got the other way round.</summary>
    public static long CullKept, CullDropped;

    /// <summary>0052. The face is drawn at its fractional corners, so it is culled at
    /// them too; a corner without a fraction, or clamped, leaves the game's answer.
    /// See "A thin face was culled on whole pixels" in docs/RENDERING.md.</summary>
    static bool FacingFractional(bool whole, uint p0, uint w0, uint p1, uint w1, uint p2, uint w2)
    {
        if (!GteVertexMap.Peek(p0, w0, out var a0) || a0.Clipped
            || !GteVertexMap.Peek(p1, w1, out var a1) || a1.Clipped
            || !GteVertexMap.Peek(p2, w2, out var a2) || a2.Clipped)
            return whole;
        double x0 = (short)w0 + a0.Fx, y0 = (short)(w0 >> 16) + a0.Fy;
        double x1 = (short)w1 + a1.Fx, y1 = (short)(w1 >> 16) + a1.Fy;
        double x2 = (short)w2 + a2.Fx, y2 = (short)(w2 >> 16) + a2.Fy;
        bool front = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0) > 0;
        if (front != whole) { if (front) CullKept++; else CullDropped++; }
        return front;
    }

    /// <summary>A fixed bias, and a slot out of range is dropped rather than clamped.
    /// The packet is still allocated.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Place(ref Frame fr, uint otz, uint pkt)
    {
        if (otz >= 0x2000u) return;
        AddPrim(fr.Mem, (fr.Hoisted ? fr.Ot : fr.Mem.ReadU32(OtBase)) + (otz << 2), pkt);
    }

    // ---- func_8002E650 and func_8002E7CC: RotTransPers into the vertex cache ---

    static void RunTransform(CpuContext c, PSMemory mem)
    {
        var saved = new Saved(c);
        TransformCalls++;
        try { TransformBody(c, mem, saved.SP - 0x30u); }
        finally { saved.Restore(c); }
    }

    /// <summary>Each vertex's screen word, otz and a fog weight on one of three
    /// curves picked by FogMode. RotTransPers's depth cue and flag go to the stack
    /// and are read back only for the weight, so they stay in locals.</summary>
    static void TransformBody(CpuContext c, PSMemory mem, uint sp)
    {
        c.SP = sp;
        int mode = (int)mem.ReadU32(FogMode);
        uint src = mem.ReadU32(VertexBase);
        uint dst = VertexCache;
        int curve = mode >= 32000 ? 0 : (mode & 0x8000) != 0 ? 1 : 2;

        var fr = new Frame(mem);
        for (uint n = c.A0; n != 0; n--)
        {
            Interrupts.Poll(c, mem);
            fr.Check();
            Gte.Write(0, mem.ReadU32(src));
            Gte.Write(1, mem.ReadU32(src + 4u));
            Gte.Rtps(12, false);
            uint sxy = Gte.Read(14);
            mem.WriteU32(dst, sxy);
            int p = (int)Gte.Read(8);
            W16(ref fr, dst + 4u, (ushort)((int)Gte.Read(19) >> 2));
            int fog = curve switch
            {
                0 => 0,
                1 => Math.Max(p - 0x320, 0) << 1,
                _ => p < 2800 ? p : ((p - 0xAF0) << 1) + p,
            };
            bool blended = TileVertexFog(mem, src, fog, near: false, out fog);
            W16(ref fr, dst + 6u, (ushort)fog);
            if (fr.Lighting) NoteCache(dst, sxy, fog, blended ? GteLightMap.CurveWord : (uint)curve, blended);
            if (fr.DepthTable) NoteDepth(mem, dst, sxy, src);
            src += 8u;
            dst += 8u;
        }
    }

    static void RunNearTransform(CpuContext c, PSMemory mem)
    {
        var saved = new Saved(c);
        TransformCalls++;
        try { NearTransformBody(c, mem, saved.SP - 0x38u); }
        finally { saved.Restore(c); }
    }

    /// <summary>func_80030540's transform: the same cache, but otz is 0xFFFF unless
    /// RotTransPers's flag is exactly 0x1000, and there are only two fog curves.</summary>
    static void NearTransformBody(CpuContext c, PSMemory mem, uint sp)
    {
        c.SP = sp;
        bool far = (int)mem.ReadU32(FogMode) >= 32000;
        uint src = mem.ReadU32(VertexBase);
        uint dst = VertexCache;

        var fr = new Frame(mem);
        for (uint n = c.A0; n != 0; n--)
        {
            Interrupts.Poll(c, mem);
            fr.Check();
            Gte.Write(0, mem.ReadU32(src));
            Gte.Write(1, mem.ReadU32(src + 4u));
            Gte.Rtps(12, false);
            uint sxy = Gte.Read(14);
            mem.WriteU32(dst, sxy);
            int p = (int)Gte.Read(8);
            uint flag = Gte.ReadControl(31);
            ushort otz = (ushort)((int)Gte.Read(19) >> 2);
            W16(ref fr, dst + 4u, flag == 0x1000u ? otz : (ushort)0xFFFF);
            int fog = far ? 0 : p < 2800 ? p : ((p - 0xAF0) << 1) + p;
            bool blended = TileVertexFog(mem, src, fog, near: true, out fog);
            W16(ref fr, dst + 6u, (ushort)fog);
            if (fr.Lighting)
                NoteCache(dst, sxy, fog, blended ? GteLightMap.CurveWord : far ? GteLightMap.CurveNone : GteLightMap.CurveKnee, blended);
            if (fr.DepthTable) NoteDepth(mem, dst, sxy, src);
            src += 8u;
            dst += 8u;
        }
    }

    // ---- KF2_POLYASM=verify -------------------------------------------------

    /// <summary>Below the entry SP: this call's frame and its callees'. The two
    /// versions leave different garbage there, and nothing reads it after.</summary>
    const uint StackWindow = 0x2000;

    /// <summary>One routine's tally and buffers. The buffers are its own because the
    /// transforms are verified again inside both runs of the assemblers.</summary>
    sealed class Check(string name, Func<string> extra)
    {
        public readonly string Name = name;
        public readonly Func<string> Extra = extra;
        public byte[] Before = [], Theirs = [];
        public readonly Gte.State GteEntry = new(), GteTheirs = new(), GteOurs = new();
        public long Calls, Bad, BadReg, BadGte;
        public readonly List<string> Samples = new();
        public double ReportAt;
    }

    static long _quadClips, _triClips, _emitted, _exhausted, _rejected, _unclippedExhausted;

    static readonly Check _assemblerCheck = new("func_80030540", () =>
    {
        string s = $"; clipped {_quadClips} quad(s) and {_triClips} triangle(s), {_emitted} survived, " +
                   $"{_rejected} rejected first, {_exhausted} buffer exhaustion(s)";
        _quadClips = _triClips = _emitted = _exhausted = _rejected = 0;
        return s;
    });

    static readonly Check _unclippedCheck = new("func_8002FECC", () =>
    {
        string s = $"; {_unclippedExhausted} buffer exhaustion(s)";
        _unclippedExhausted = 0;
        return s;
    });

    static readonly Check _transformCheck = new("func_8002E650", () => "");
    static readonly Check _nearTransformCheck = new("func_8002E7CC", () => "");

    static void Verify(Check k, Action<CpuContext, IMemory> orig, CpuContext c, PSMemory mem,
                       Action<CpuContext, PSMemory> run)
    {
        var ro = mem.Ram;
        var ram = MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(ro), ro.Length);
        if (k.Before.Length != ram.Length)
        {
            k.Before = new byte[ram.Length];
            k.Theirs = new byte[ram.Length];
        }

        uint a0 = c.A0, a1 = c.A1, a2 = c.A2;
        ram.CopyTo(k.Before);
        var entry = c.Snapshot();
        Gte.Save(k.GteEntry);

        orig(c, mem);
        ram.CopyTo(k.Theirs);
        var theirs = c.Snapshot();
        Gte.Save(k.GteTheirs);

        k.Before.CopyTo(ram);
        c.Restore(entry);
        Gte.Load(k.GteEntry);
        run(c, mem);
        var ours = c.Snapshot();
        Gte.Save(k.GteOurs);

        // A rejection that is not replayed leaves the clipper's scratch as it was.
        if (k == _assemblerCheck && !ReplayRejection) ExcludeClipScratch(ram, k.Theirs);

        k.Calls++;
        int hi = (int)((entry.SP & (uint)(ram.Length - 1)));
        int lo = Math.Max(0, hi - (int)StackWindow);
        bool same = ram[..lo].SequenceEqual(k.Theirs.AsSpan(0, lo))
                 && ram[hi..].SequenceEqual(k.Theirs.AsSpan(hi));
        if (!same)
        {
            k.Bad++;
            if (k.Samples.Count < 8)
            {
                int i = FirstDiff(ram, k.Theirs, 0, lo);
                if (i < 0) i = FirstDiff(ram, k.Theirs, hi, ram.Length);
                int diffs = CountDiffs(ram, k.Theirs, 0, lo) + CountDiffs(ram, k.Theirs, hi, ram.Length);
                k.Samples.Add($"a0={a0:X} a1={a1:X} a2={a2:X}: {diffs} byte(s), first 0x{0x80000000u + (uint)i:X8} " +
                              $"recompiled {k.Theirs[i]:X2} ours {ram[i]:X2}");
            }
        }

        if (ours.S0 != theirs.S0 || ours.S1 != theirs.S1 || ours.S2 != theirs.S2 || ours.S3 != theirs.S3 ||
            ours.S4 != theirs.S4 || ours.S5 != theirs.S5 || ours.S6 != theirs.S6 || ours.S7 != theirs.S7 ||
            ours.FP != theirs.FP || ours.SP != theirs.SP || ours.RA != theirs.RA)
            k.BadReg++;

        if (Gte.Diff(k.GteTheirs, k.GteOurs) is { } gte)
        {
            k.BadGte++;
            if (k.Samples.Count < 8) k.Samples.Add($"a0={a0:X} a1={a1:X} a2={a2:X}: GTE {gte}");
        }

        // The recompiled result stands, so a mismatch cannot reach the picture.
        k.Theirs.CopyTo(ram);
        c.Restore(theirs);
        Gte.Load(k.GteTheirs);

        double now = Environment.TickCount64 / 1000.0;
        if (now < k.ReportAt) return;
        k.ReportAt = now + 2.0;
        Console.WriteLine($"[polyasm] verify {k.Name}: {k.Calls} call(s), {k.Bad} RAM mismatch(es), " +
                          $"{k.BadReg} register mismatch(es), {k.BadGte} GTE mismatch(es){k.Extra()}");
        foreach (var s in k.Samples) Console.WriteLine($"[polyasm]   {s}");
        k.Samples.Clear();
        k.Calls = k.Bad = k.BadReg = k.BadGte = 0;
    }

    /// <summary>The records at *ClipRecords a rejection would have filled (four at most)
    /// and both of ClipFT's lists at ClipOut.</summary>
    static void ExcludeClipScratch(Span<byte> ours, byte[] theirs)
    {
        uint mask = (uint)ours.Length - 1u;
        uint recs = MemoryMarshal.Read<uint>(theirs.AsSpan((int)(ClipRecords & mask))) & mask;
        CopyRange(theirs, ours, recs, 0x2Cu * 4u);
        CopyRange(theirs, ours, ClipOut & mask, 0x50u);
    }

    static void CopyRange(byte[] from, Span<byte> to, uint offset, uint length)
    {
        if (offset + length > to.Length) return;
        from.AsSpan((int)offset, (int)length).CopyTo(to[(int)offset..]);
    }

    static int FirstDiff(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b, int from, int to)
    {
        int i = a[from..to].CommonPrefixLength(b[from..to]);
        return i == to - from ? -1 : from + i;
    }

    static int CountDiffs(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b, int from, int to)
    {
        int n = 0;
        for (int i = from; i < to; i++) if (a[i] != b[i]) n++;
        return n;
    }
}
