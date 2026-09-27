using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using KingsField2 = Recompiled.KingsField2_game;

namespace Kf2;

/// <summary>
/// The map-tile walk in C#: `func_80031C94` (the 24x24 cell sweep), `func_80031B1C`
/// (one cell's two stacked halves) and `func_80031950` (one half, set up and handed
/// to an assembler).
///
///     KF2_TILEWALK=0        all three recompiled
///     KF2_TILEWALK=verify   run both on every call and compare RAM, registers and the GTE
///     KF2_TILEWALK_CELL=0   func_80031B1C recompiled
///     KF2_TILEWALK_TILE=0   func_80031950 recompiled
///     KF2_TILEWALK_PROBE=1  a line every two seconds: cells walked, halves drawn, by assembler
///
/// **Why this routine and not a faster one.** Its own body is 0.041-0.124 ms a frame
/// (see "What the map walk's time is" in docs/DEVELOPMENT.md), so this is not a
/// performance change and must not be argued as one. It is where the port learns
/// *which tile, at what world position, drawn by which assembler* — the producer of
/// the grid it reads is already C# (`CullGrid` replaces `func_8002D3A8` and writes
/// the 24x24 array back to <see cref="Grid"/>), and two of its three consumers are
/// already C# (`PolyAssembler`). Only the walk between them was still recompiled
/// MIPS. That is the same move `0050` made when the assemblers started recording
/// each packet's depth: the fix was knowing *which routine asked*, which no GP0
/// consumer can recover.
///
/// The libgte leaves (`SetRotMatrix`, `RotTrans`, `SetLightMatrix`, ...) are still
/// called as recompiled code here, deliberately — exactness first, so `verify` has
/// something to mean. Inlining them as direct `Gte` calls is the follow-up, and is
/// where the time actually is.
///
/// See "The assemblers write the depth" in docs/RENDERING.md for the pattern.
/// </summary>
public static class TileWalk
{
    const uint Walk = 0x80031C94;   // the 24x24 cell sweep
    const uint Cell = 0x80031B1C;   // one cell: two stacked halves
    const uint Tile = 0x80031950;   // one half: matrices, light, assembler

    /// <summary>The legacy 24x24 visibility grid, one byte a cell. `CullGrid` writes
    /// it (LegacySpan 24, stride 24); bit 0 gates the lower half, bit 1 the upper,
    /// 0x80 and 0x40 pick the assembler.</summary>
    const uint Grid = 0x80192EAC;

    /// <summary>The grid's origin tile, the negated mirrors `CullGrid` publishes.</summary>
    const uint GridOriginX = 0x80192EA0, GridOriginZ = 0x80192EA4;

    /// <summary>The camera's world position, u16 each.</summary>
    const uint CamWorldX = 0x80192E78, CamWorldY = 0x80192E7C, CamWorldZ = 0x80192E80;

    /// <summary>The view matrix `func_80031950` loads before every half.</summary>
    const uint ViewMatrix = 0x80192E18;

    /// <summary>The map: 80x80 tiles, 10 bytes a tile. +0/+1 the lower half's model
    /// and height, +5/+6 the upper's; a model of 240 or more is not drawn.</summary>
    const uint MapBase = 0x801C8484;

    /// <summary>Per-area light records, 104 bytes each: four 20-byte light matrices
    /// by rotation, a colour matrix at +0x50, the back colour at +0x62 and the depth
    /// cue at +0x66.</summary>
    const uint LightBase = 0x801930F0;

    /// <summary>The far-model gate: when the u16 is 1 and the u8 is set, a model at
    /// or past the table's limit is skipped entirely.</summary>
    const uint FarFlag = 0x8017E05C, FarEnable = 0x8017E072;

    const uint ModelTable = 0x8018E19C;

    /// <summary>Tiles a side, and the loop bound both of the walk's tests use.</summary>
    const uint MapSpan = 0x50;

    /// <summary>Cells a side of the visibility grid.</summary>
    const uint GridSpan = 0x18;

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;
    static bool _queuedWalk, _queuedCell, _queuedTile;

    /// <summary>Off hands every call to the recompiled routine.</summary>
    public static bool Enabled { get; set; } = true;

    public static bool CellEnabled { get; set; } = true;
    public static bool TileEnabled { get; set; } = true;
    public static bool Verifying => _mode == Mode.Verify;

    static bool _probe;
    static long _fansAt, _quadsAt;

    /// <summary>Running totals; never reset.</summary>
    public static long WalkCalls, CellCalls, TileCalls;

    /// <summary>Cells the last completed walk walked and drew. Valid until the next
    /// walk starts, so a consumer reads it from a post on `func_80031C94` or from
    /// anywhere inside stage 13 after it. <see cref="CrossProbe"/> is the reader:
    /// a frame that drew none of them has no map in its ordering table, which is
    /// nameable in a dark room where luminance is not.</summary>
    public static long CellsWalked => _cellsWalked;

    /// <inheritdoc cref="CellsWalked"/>
    public static long CellsDrawn => _cellsDrawn;

    /// <summary>The map record of the half `func_80031950` is assembling, or 0 outside
    /// one -- the tile-side counterpart of <c>ModelWalk.SetSubmit</c>.
    /// <c>Remaster.Identity.FromRecord</c> turns it into a tile and a half.</summary>
    public static uint CurrentRecord { get; private set; }

    // What the last walk saw, which is the whole point of the routine being here.
    static long _cellsWalked, _cellsDrawn, _halves, _unclipped, _plain, _subdivided, _skipped;
    static double _probeAt;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.tilewalk",
        Name = "Map tile walk",
        Version = "1.0",
        Description = "func_80031C94, func_80031B1C and func_80031950 in C#.",
    };

    public static void Configure(string? mode, string? cell, string? tile, string? probe)
    {
        _mode = mode?.Trim().ToLowerInvariant() == "verify" ? Mode.Verify : Mode.On;
        Enabled = mode?.Trim().ToLowerInvariant() is not ("0" or "off");
        CellEnabled = cell?.Trim() != "0";
        TileEnabled = tile?.Trim() != "0";
        _probe = probe?.Trim() is not (null or "" or "0");
    }

    public static void Install() => HookAttach.OnOverlayLoad("tilewalk", Attach);

    static bool Attach()
    {
        var walk = SymbolRegistry.Resolve("game", null, Walk);
        var cell = SymbolRegistry.Resolve("game", null, Cell);
        var tile = SymbolRegistry.Resolve("game", null, Tile);
        if (walk == null || cell == null || tile == null) return false;

        if (!Queue(ref _queuedWalk, walk, nameof(ReplaceWalk))) return false;
        if (!Queue(ref _queuedCell, cell, nameof(ReplaceCell))) return false;
        if (!Queue(ref _queuedTile, tile, nameof(ReplaceTile))) return false;

        HookManager.Commit();
        bool ok = HookAttach.Installed(walk) && HookAttach.Installed(cell) && HookAttach.Installed(tile);
        string State(bool on) => !on ? "off" : _mode.ToString().ToLowerInvariant();
        Console.WriteLine(!ok
            ? "[KF2] tilewalk: not installed"
            : $"[KF2] tilewalk: walk {State(Enabled)}, cell {State(Enabled && CellEnabled)}, " +
              $"tile {State(Enabled && TileEnabled)}");
        return ok;
    }

    static bool Queue(ref bool queued, System.Reflection.MethodInfo target, string method)
    {
        if (queued) return true;
        var impl = typeof(TileWalk).GetMethod(method,
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
        queued = HookManager.AddReplace(_self, target, impl);
        return queued;
    }

    // PGXP follows values through the registers, which locals do not have.
    static bool Recompiled(bool on) => !on || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking;

    static void ReplaceWalk(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_walkCheck, orig, c, mem, RunWalk);
        else RunWalk(c, mem);
    }

    static void ReplaceCell(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && CellEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_cellCheck, orig, c, mem, RunCell);
        else RunCell(c, mem);
    }

    static void ReplaceTile(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && TileEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_tileCheck, orig, c, mem, RunTile);
        else RunTile(c, mem);
    }

    // ---- func_80031C94: the 24x24 cell sweep ---------------------------------

    /// <summary>Walks the visibility grid row by row, skipping a row whose world Z is
    /// off the 80x80 map and a column whose world X is, and calls
    /// <see cref="RunCell"/> for every cell with a non-zero flag byte. The two
    /// `Interrupts.Poll` calls are the recompiled loop heads' and are kept: this loop
    /// runs 576 times a frame and is long enough to starve an IRQ without them.
    /// </summary>
    static void RunWalk(CpuContext c, PSMemory mem)
    {
        uint sp = c.SP - 0x30u;
        c.SP = sp;
        mem.WriteU32(sp + 0x28u, c.RA);
        mem.WriteU32(sp + 0x24u, c.S5);
        mem.WriteU32(sp + 0x20u, c.S4);
        mem.WriteU32(sp + 0x1Cu, c.S3);
        mem.WriteU32(sp + 0x18u, c.S2);
        mem.WriteU32(sp + 0x14u, c.S1);
        mem.WriteU32(sp + 0x10u, c.S0);

        if (Remaster.Faces.Recording) Remaster.Faces.FrameStart();
        if (!PlanarWalk.Mirroring) { RetainedMap.AtWalk(c, mem); Waves.AtWalk(c, mem); MurkLevel.AtWalk(mem); }
        c.A0 = 0u;
        c.RA = 0x80031CBCu;
        KingsField2.func_8002E190(c, mem);

        uint cell = Grid;
        uint originX = mem.ReadU32(GridOriginX);
        uint z = mem.ReadU32(GridOriginZ);
        long walked = 0, drawn = 0;

        for (uint row = GridSpan; row != 0u; row--)
        {
            Interrupts.Poll(c, mem);
            if (z < MapSpan)
            {
                uint x = originX;
                for (uint col = GridSpan; col != 0u; col--)
                {
                    Interrupts.Poll(c, mem);
                    uint tx = x & 0xFFu;
                    x++;
                    if (tx < MapSpan)
                    {
                        walked++;
                        uint flags = mem.ReadU8(cell);
                        if (flags != 0u)
                        {
                            drawn++;
                            c.A0 = tx;
                            c.A1 = z;
                            c.A2 = flags;
                            c.RA = 0x80031D14u;
                            KingsField2.func_80031B1C(c, mem);
                        }
                    }
                    cell++;
                }
            }
            else cell += GridSpan;
            z++;
        }

        c.RA = mem.ReadU32(sp + 0x28u);
        c.S5 = mem.ReadU32(sp + 0x24u);
        c.S4 = mem.ReadU32(sp + 0x20u);
        c.S3 = mem.ReadU32(sp + 0x1Cu);
        c.S2 = mem.ReadU32(sp + 0x18u);
        c.S1 = mem.ReadU32(sp + 0x14u);
        c.S0 = mem.ReadU32(sp + 0x10u);
        c.SP = sp + 0x30u;

        WalkCalls++;
        _cellsWalked = walked;
        _cellsDrawn = drawn;
        Report();
    }

    // ---- func_80031B1C: one cell's two stacked halves ------------------------

    /// <summary>`a0` the tile's X, `a1` its Z, `a2` the cell's flag byte. Bit 0 gates
    /// the lower half and bit 1 the upper; each is drawn only while its model byte is
    /// below 240. The position is built in the caller's own frame at `sp+0x10` as
    /// three shorts (X, Y, Z) and that pointer is what `func_80031950` receives — so
    /// the stack arithmetic here is the original's exactly, because `EvenFog` and
    /// `PacketMatch` both read it.</summary>
    static void RunCell(CpuContext c, PSMemory mem)
    {
        uint sp = c.SP - 0x28u;
        c.SP = sp;
        mem.WriteU32(sp + 0x24u, c.RA);
        mem.WriteU32(sp + 0x20u, c.S2);
        mem.WriteU32(sp + 0x1Cu, c.S1);
        mem.WriteU32(sp + 0x18u, c.S0);

        uint tx = c.A0, tz = c.A1, flags = c.A2;
        uint rec = MapBase + tz * 800u + tx * 10u;
        uint pos = sp + 0x10u;
        uint half = flags & 0xFFu;
        bool placed = false;

        if ((flags & 1u) != 0u && mem.ReadU8(rec) < 240)
        {
            Place(mem, pos, tx, tz);
            placed = true;
            Elevate(mem, pos, mem.ReadU8(rec + 1u));
            c.A0 = rec;
            c.A1 = pos;
            c.A2 = half;
            c.RA = 0x80031BE4u;
            KingsField2.func_80031950(c, mem);
        }

        if ((flags & 2u) != 0u && mem.ReadU8(rec + 5u) < 240)
        {
            if (!placed) Place(mem, pos, tx, tz);
            Elevate(mem, pos, mem.ReadU8(rec + 6u));
            c.A0 = rec + 5u;
            c.A1 = pos;
            c.A2 = half;
            c.RA = 0x80031C7Cu;
            KingsField2.func_80031950(c, mem);
        }

        c.RA = mem.ReadU32(sp + 0x24u);
        c.S2 = mem.ReadU32(sp + 0x20u);
        c.S1 = mem.ReadU32(sp + 0x1Cu);
        c.S0 = mem.ReadU32(sp + 0x18u);
        c.SP = sp + 0x28u;
        CellCalls++;
    }

    /// <summary>X and Z, tile-centred and relative to the camera. Written once per
    /// cell: the upper half reuses the lower's when the lower drew.</summary>
    static void Place(PSMemory mem, uint pos, uint tx, uint tz)
    {
        mem.WriteU16(pos + 0x0u, (ushort)((tx << 11) - mem.ReadU16(CamWorldX) + 0x400u));
        mem.WriteU16(pos + 0x4u, (ushort)((tz << 11) - mem.ReadU16(CamWorldZ) + 0x400u));
    }

    /// <summary>Y, from the half's height byte. Written per half.</summary>
    static void Elevate(PSMemory mem, uint pos, uint height)
        => mem.WriteU16(pos + 0x2u, (ushort)(((0u - height) << 7) - mem.ReadU16(CamWorldY)));

    // ---- func_80031950: one half, set up and assembled -----------------------

    /// <summary>`a0` the half's record (the tile's, or the tile's plus 5), `a1` the
    /// position, `a2` the cell's flag byte. Loads the view matrix, rotates by the
    /// record's low two bits, sets the light and colour matrices from the area's
    /// record, then picks the assembler by the flag byte: no `0x80` is the far bulk
    /// (`func_8002FECC`, no clipper), `0x80` is `func_80030540`, and `0xC0` on a mesh
    /// of fewer than 16 faces is subdivided by `func_80030C94` first.</summary>
    static void RunTile(CpuContext c, PSMemory mem)
    {
        uint sp = c.SP - 0x1050u;
        c.SP = sp;
        mem.WriteU32(sp + 0x1048u, c.RA);
        mem.WriteU32(sp + 0x1044u, c.S3);
        mem.WriteU32(sp + 0x1040u, c.S2);
        mem.WriteU32(sp + 0x103Cu, c.S1);
        mem.WriteU32(sp + 0x1038u, c.S0);

        uint rec = c.A0, pos = c.A1, flags = c.A2;
        uint rot = mem.ReadU8(rec + 2u) & 3u;

        c.A0 = ViewMatrix;
        c.RA = 0x80031988u;
        KingsField2.SetRotMatrix(c, mem);
        c.A0 = ViewMatrix;
        c.RA = 0x80031998u;
        KingsField2.SetTransMatrix(c, mem);
        c.A0 = pos;
        c.A1 = sp + 0x24u;
        c.A2 = sp + 0x30u;
        c.RA = 0x800319A8u;
        KingsField2.RotTrans(c, mem);
        c.A0 = ViewMatrix;
        c.A1 = sp + 0x10u;
        c.A2 = rot;
        c.RA = 0x800319BCu;
        KingsField2.func_80014B88(c, mem);
        c.A0 = sp + 0x10u;
        c.RA = 0x800319C4u;
        KingsField2.SetRotMatrix(c, mem);
        c.A0 = sp + 0x10u;
        c.RA = 0x800319CCu;
        KingsField2.SetTransMatrix(c, mem);

        uint light = LightBase + (mem.ReadU8(rec + 4u) & 0x3Fu) * 104u;
        c.A0 = light + rot * 20u;
        c.RA = 0x80031A08u;
        KingsField2.SetLightMatrix(c, mem);
        c.A0 = light + 0x50u;
        c.RA = 0x80031A10u;
        KingsField2.SetColorMatrix(c, mem);
        c.A0 = (uint)(short)mem.ReadU16(light + 0x66u);
        c.RA = 0x80031A1Cu;
        KingsField2.func_8002DDDC(c, mem);
        c.A0 = mem.ReadU8(light + 0x62u);
        c.A1 = mem.ReadU8(light + 0x63u);
        c.A2 = mem.ReadU8(light + 0x64u);
        c.RA = 0x80031A30u;
        KingsField2.SetBackColor_game(c, mem);

        uint model = mem.ReadU8(rec);
        if (Beyond(mem, model)) { _skipped++; Epilogue(c, mem, sp); return; }
        // What the frame drew is what its reflections may show (RetainedScene.HalfGate).
        if (RetainedMap.Ready && !PlanarWalk.Mirroring)
        {
            uint off = rec - MapBase;
            RetainedScene.NoteHalf((int)(off % 800u / 10u), (int)(off / 800u), (int)(off % 10u / 5u));
        }

        // The half being assembled, for whatever the assemblers record per packet.
        CurrentRecord = rec;
        PolyAssembler.TileMaterial = Remaster.Surfaces.EnterHalf(rec, (int)model);

        c.A0 = model;
        c.RA = 0x80031A84u;
        KingsField2.func_8002E1F0(c, mem);
        Remaster.Faces.NoteTable(mem.ReadU32(ModelTable));
        // The water's moved copy, for every reader of the mesh below; put back after.
        WaterSwell.Enter(mem, rec, model);

        // A subdivided mesh leaves the subdivider's corners in the vertex cache.
        bool whole = true;
        try
        {
            if ((flags & 0x80u) == 0u)
            {
                _unclipped++;
                c.A0 = model;
                c.RA = 0x80031B00u;
                KingsField2.func_8002FECC(c, mem);
            }
            else if ((flags & 0x40u) == 0u) Plain(c, mem, model);
            else
            {
                c.A0 = model;
                c.RA = 0x80031AA0u;
                KingsField2.func_8002E1BC(c, mem);
                if (mem.ReadU32(c.V0 + 0x14u) < 0x10u)
                {
                    _subdivided++;
                    whole = false;
                    uint srcVerts = mem.ReadU32(0x8018EAA0u);
                    c.A0 = mem.ReadU32(ModelTable);
                    c.A1 = model;
                    c.A2 = sp + 0x38u;
                    c.RA = 0x80031AC8u;
                    KingsField2.func_80030C94(c, mem);
                    if (Remaster.Faces.Wanted) Remaster.Faces.Subdivided(mem, model, sp + 0x38u);
                    if (Remaster.FaceProbe.On) Remaster.FaceProbe.Subdivided(mem, model, sp + 0x38u, srcVerts);
                    c.A0 = model;
                    c.A1 = 0xF0u;
                    c.A2 = sp + 0x38u;
                    c.RA = 0x80031AD8u;
                    KingsField2.func_80030540(c, mem);
                    if (Remaster.Faces.Wanted) Remaster.Faces.SubdividedDone();
                }
                else Plain(c, mem, model);
            }
        }
        finally { WaterSwell.Leave(mem); }

        if (RetainedMap.Checking && !PlanarWalk.Mirroring && whole) RetainedMap.CheckHalf(mem, rec, model);
        Epilogue(c, mem, sp);
    }

    static void Plain(CpuContext c, PSMemory mem, uint model)
    {
        _plain++;
        c.A0 = model;
        c.A1 = 0xF0u;
        c.A2 = 0u;
        c.RA = 0x80031AF0u;
        KingsField2.func_80030540(c, mem);
    }

    /// <summary>The far-model gate: only while the flag is 1 and the enable is set is
    /// a model at or past the table's limit dropped.</summary>
    static bool Beyond(PSMemory mem, uint model)
    {
        if ((short)mem.ReadU16(FarFlag) != 1) return false;
        if (mem.ReadU8(FarEnable) == 0) return false;
        return model >= mem.ReadU32(mem.ReadU32(ModelTable) + 4u);
    }

    static void Epilogue(CpuContext c, PSMemory mem, uint sp)
    {
        if (Remaster.Faces.Wanted) Remaster.Faces.LeaveHalf();
        CurrentRecord = 0;
        PolyAssembler.TileMaterial = 0;
        c.RA = mem.ReadU32(sp + 0x1048u);
        c.S3 = mem.ReadU32(sp + 0x1044u);
        c.S2 = mem.ReadU32(sp + 0x1040u);
        c.S1 = mem.ReadU32(sp + 0x103Cu);
        c.S0 = mem.ReadU32(sp + 0x1038u);
        c.SP = sp + 0x1050u;
        TileCalls++;
        _halves++;
    }

    static void Report()
    {
        if (!_probe) return;
        double now = Environment.TickCount64 / 1000.0;
        if (now < _probeAt) return;
        double span = _probeAt == 0.0 ? 2.0 : now - (_probeAt - 2.0);
        _probeAt = now + 2.0;
        // The cells are this frame's; everything below them is a rate, because a
        // half is counted where it is drawn and the walk only counts where it looks.
        Console.WriteLine($"[tilewalk] this frame {_cellsWalked} cell(s) on the map, {_cellsDrawn} with a flag; " +
                          $"{_halves / span:F0} half/halves a second: {_unclipped / span:F0} unclipped, " +
                          $"{_plain / span:F0} plain, {_subdivided / span:F0} subdivided, " +
                          $"{_skipped / span:F0} past the model limit; " +
                          $"facing on the whole polygon changed {(PolyAssembler.ClippedChanged - _fansAt) / span:F1} clipped " +
                          $"and {(PolyAssembler.QuadsChanged - _quadsAt) / span:F1} unclipped quad(s) a second");
        _fansAt = PolyAssembler.ClippedChanged;
        _quadsAt = PolyAssembler.QuadsChanged;
        _halves = _unclipped = _plain = _subdivided = _skipped = 0;
    }

    // ---- KF2_TILEWALK=verify -------------------------------------------------

    /// <summary>Below the entry SP: this call's frame and its callees'. `func_80031950`
    /// alone takes 0x1050 of it for the subdivider's scratch, and the assemblers add
    /// their own, so the window is wider than PolyAssembler's.</summary>
    const uint StackWindow = 0x4000;

    sealed class Check(string name)
    {
        public readonly string Name = name;
        public byte[] Before = [], Theirs = [];
        public readonly Gte.State GteEntry = new(), GteTheirs = new(), GteOurs = new();
        public long Calls, Bad, BadReg, BadGte;
        public readonly List<string> Samples = new();
        public double ReportAt;
    }

    static readonly Check _walkCheck = new("func_80031C94");
    static readonly Check _cellCheck = new("func_80031B1C");
    static readonly Check _tileCheck = new("func_80031950");

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

        k.Calls++;
        int hi = (int)(entry.SP & (uint)(ram.Length - 1));
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
        Console.WriteLine($"[tilewalk] verify {k.Name}: {k.Calls} call(s), {k.Bad} RAM mismatch(es), " +
                          $"{k.BadReg} register mismatch(es), {k.BadGte} GTE mismatch(es)");
        foreach (var s in k.Samples) Console.WriteLine($"[tilewalk]   {s}");
        k.Samples.Clear();
        k.Calls = k.Bad = k.BadReg = k.BadGte = 0;
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
