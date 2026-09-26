using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using KingsField2 = Recompiled.KingsField2_game;

namespace Kf2;

/// <summary>What kind of table a submit came out of.</summary>
public enum ModelKind : byte { Creature, Object, Effect, Sprite }

/// <summary>One model handed to <c>func_80032588</c>, as the walk and the submitter
/// between them know it. This is the whole point of the routine being here: no
/// consumer of GP0 can recover which record asked for a polygon.</summary>
public readonly struct ModelDraw
{
    public readonly ModelKind Kind;
    public readonly int Slot;
    public readonly uint Record;
    public readonly ushort Model;
    public readonly int X, Y, Z;
    public readonly byte Assembler;
    public readonly uint Light;

    public ModelDraw(ModelKind kind, int slot, uint record, ushort model,
                     int x, int y, int z, byte assembler, uint light)
    {
        Kind = kind; Slot = slot; Record = record; Model = model;
        X = x; Y = y; Z = z; Assembler = assembler; Light = light;
    }
}

/// <summary>
/// The object and creature walk in C#: `func_800331B4` (the four table walks) and
/// `func_80032588` (the model submitter it dispatches to).
///
///     KF2_MODELWALK=0        both recompiled
///     KF2_MODELWALK=verify   run both on every call and compare RAM, registers and the GTE
///     KF2_MODELWALK_WALK=0   func_800331B4 recompiled, the submitter still C#
///     KF2_MODELWALK_SUBMIT=0 func_80032588 recompiled, the walk still C#
///     KF2_MODELWALK_PROBE=1  a line every two seconds: what each table submitted
///
/// **Why this routine.** `TileWalk` taught the port the static world; this is
/// everything that moves. Its own body is 0.005-0.047 ms a frame at 4-23 calls
/// (see "What the model submitter's time is" in docs/DEVELOPMENT.md), so this is
/// not a performance change and must not be argued as one. It is the point at
/// which the port learns *which creature, at what world position, in which pose,
/// drawn by which assembler* — and the path is already C# at both ends:
/// `PolyAssembler` owns all three assemblers this dispatches to
/// (`func_8002F214`, `func_80030540`, `func_8002EAEC`), and `ObjectSmoothing`
/// already carries the two tables it reads.
///
/// **The four tables**, in the order the walk takes them, all confirmed by the
/// strides and counts read straight out of the routine:
///
/// | table | base | records | stride | liveness |
/// |---|---|---|---|---|
/// | creatures | `0x8016C544` | 200 | `0x7C` | `u8[+0x9] == 1` |
/// | objects | `0x80177714` | 396 | `0x44` | `u16[+0x6] != 0xFF` |
/// | effects | `0x8019CC6C` | 128 | `0x48` | `u8[+0x0] != 0xFF` |
/// | sprites | `0x80195174` | 128 | `0x18` | `u16[+0x0] != 0xFFFF` |
///
/// The libgte leaves and the assemblers are still called as recompiled code, which
/// is what makes `verify` mean something; `HookManager` detours them by method, so
/// a direct call here still lands in `PolyAssembler`.
///
/// See "The map tile walk in C#" in docs/PATCHES_AND_MODS.md for the pattern.
/// </summary>
public static class ModelWalk
{
    const uint Walk = 0x800331B4;    // the four table walks
    const uint Submit = 0x80032588;  // one model: matrices, light, assembler

    // ---- the four tables ----------------------------------------------------

    const uint Creatures = 0x8016C544, CreatureStride = 0x7C; const int CreatureCount = 200;
    const uint Objects = 0x80177714, ObjectStride = 0x44; const int ObjectCount = 396;
    const uint Effects = 0x8019CC6C, EffectStride = 0x48; const int EffectCount = 128;
    const uint Sprites = 0x80195174, SpriteStride = 0x18; const int SpriteCount = 128;

    /// <summary>Per-creature definitions, 120 bytes each; `+7` and `+8` are the two
    /// texture pages the walk marks as wanted whether or not the model drew.</summary>
    const uint CreatureDefs = 0x80172624;

    /// <summary>Per-object definitions, 24 bytes each; `+2` is a page and `+0xC` the
    /// mask the second visibility query is asked with.</summary>
    const uint ObjectDefs = 0x80175914;

    /// <summary>The view matrix, and the second matrix the effects and sprites use.</summary>
    const uint ViewMatrix = 0x80192E18, FlatMatrix = 0x80192E38;

    /// <summary>A fixed matrix in `GAME.EXE`'s own data, used by a creature or effect
    /// that is placed without the camera composed in.</summary>
    const uint RomMatrix = 0x80064B30;

    /// <summary>The camera's world position, and the map and light records
    /// `func_80032588` resolves against them.</summary>
    const uint CamWorldX = 0x80192E78, CamWorldY = 0x80192E7C, CamWorldZ = 0x80192E80;
    const uint MapBase = 0x801C8484, LightBase = 0x801930F0;

    /// <summary>Which half of the camera's own tile the view-space path is lit from:
    /// 0 for the lower, 5 for the upper.</summary>
    const uint HalfSelect = 0x8019953C;

    /// <summary>The player's position, which the effects' ambient sounds are ranged
    /// against, and the clock their retrigger interval is counted on.</summary>
    const uint PlayerPos = 0x801994EC, PlayerY = 0x801994F0, SoundClock = 0x801B6CAC;

    /// <summary>Effect definitions' sound ids, 10 bytes apart, and the object
    /// visibility mask the 0xF0 kind is gated on.</summary>
    const uint EffectSounds = 0x801989D8, ObjectMask = 0x801B69BC;

    /// <summary>The billboard table's shared cel clock, bumped once per walk.</summary>
    const uint SpriteClock = 0x80195170;

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;
    static bool _queuedWalk, _queuedSubmit;

    /// <summary>Off hands every call to the recompiled routine.</summary>
    public static bool Enabled { get; set; } = true;

    public static bool WalkEnabled { get; set; } = true;
    public static bool SubmitEnabled { get; set; } = true;
    public static bool Verifying => _mode == Mode.Verify;

    static bool _probe;

    /// <summary>Running totals; never reset.</summary>
    public static long WalkCalls, SubmitCalls;

    // What the last walk saw, which is the whole point of the routine being here.
    static ModelDraw[] _scene = new ModelDraw[64];
    static int _sceneCount, _lastCount;
    static ModelKind _kind;

    /// <summary>The table the model being submitted came from.</summary>
    public static ModelKind SubmitKind => _kind;
    static int _slot;
    static uint _record;

    /// <summary>The table record of the model being submitted.</summary>
    public static uint SubmitRecord => _record;

    /// <summary>The slot the model being submitted came from.</summary>
    public static int SubmitSlot => _slot;

    static int _model = -1;

    /// <summary>The model id being assembled, or -1 outside an assembler call or when
    /// the walk is not the C# one.</summary>
    public static int SubmitModel => _model;

    /// <summary>Name the model about to be submitted from outside the walk, as the
    /// walk names it before each of its own calls: <see cref="PlanarWalk"/> replays
    /// the walk's submits, and the packet records read the kind and record.</summary>
    public static void SetSubmit(ModelKind kind, int slot, uint record)
    {
        _kind = kind;
        _slot = slot;
        _record = record;
    }

    /// <summary>An object's kind: the first byte of its definition. It matches the
    /// record's type byte (`+4`) for every object sampled except the secret door,
    /// whose type reads `0xFF`.</summary>
    public static byte ObjectKind(PSMemory mem, uint rec) => mem.ReadU8(ObjectDefs + mem.ReadU16(rec + 0x6u) * 24u);

    /// <summary>The kinds whose blended models are solid: the doors (`0x02`, models
    /// 130 and 205) and the secret door (`0x0E`, model 230).</summary>
    public static bool SolidKind(byte kind) => kind is 0x02 or 0x0E;

    /// <summary>Is the C# walk the thing calling the submitter? The kind, the slot
    /// and the record are the walk's half of a scene entry, so with the walk still
    /// recompiled — `KF2_MODELWALK_SUBMIT` alone, or the recompiled half of a
    /// `verify` pass — the submitter has nothing true to say and records nothing.
    /// All five of `func_80032588`'s call sites are inside `func_800331B4`.</summary>
    static bool _walkOwns;

    /// <summary>Inside the C# object walk, between its first table and its last.</summary>
    public static bool InWalk => _walkOwns;

    /// <summary>The model being submitted, when it is placed in the world through the
    /// view matrix: its own rotation (times its scale, at 4096) and its world position,
    /// so the retained scene can place its corners without going back through the
    /// camera, whose rotation is only good to 1/4096 of the distance.</summary>
    public static bool Placed { get; private set; }
    public static readonly short[] PlacedRot = new short[9];
    public static int PlacedX, PlacedY, PlacedZ;

    /// <summary>Every model the last completed walk submitted. Valid until the next
    /// walk starts, so a consumer reads it from a post on `func_800331B4` or from
    /// anywhere inside stage 13 after it.</summary>
    public static ReadOnlySpan<ModelDraw> Scene => _scene.AsSpan(0, _lastCount);

    static long _creatures, _objects, _effects, _sprites, _lit, _flat, _semi, _ambients;
    // Live slots this frame, so "0 submitted" is told from "nothing there".
    static int _liveCreatures, _liveObjects, _liveEffects, _liveSprites;
    static double _probeAt;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.modelwalk",
        Name = "Object and creature walk",
        Version = "1.0",
        Description = "func_800331B4 and func_80032588 in C#.",
    };

    public static void Configure(string? mode, string? walk, string? submit, string? probe)
    {
        _mode = mode?.Trim().ToLowerInvariant() == "verify" ? Mode.Verify : Mode.On;
        Enabled = mode?.Trim().ToLowerInvariant() is not ("0" or "off");
        WalkEnabled = walk?.Trim() != "0";
        SubmitEnabled = submit?.Trim() != "0";
        _probe = probe?.Trim() is not (null or "" or "0");
    }

    public static void Install() => HookAttach.OnOverlayLoad("modelwalk", Attach);

    static bool Attach()
    {
        var walk = SymbolRegistry.Resolve("game", null, Walk);
        var submit = SymbolRegistry.Resolve("game", null, Submit);
        if (walk == null || submit == null) return false;

        if (!Queue(ref _queuedWalk, walk, nameof(ReplaceWalk))) return false;
        if (!Queue(ref _queuedSubmit, submit, nameof(ReplaceSubmit))) return false;

        HookManager.Commit();
        bool ok = HookAttach.Installed(walk) && HookAttach.Installed(submit);
        string State(bool on) => !on ? "off" : _mode.ToString().ToLowerInvariant();
        Console.WriteLine(!ok
            ? "[KF2] modelwalk: not installed"
            : $"[KF2] modelwalk: walk {State(Enabled && WalkEnabled)}, " +
              $"submit {State(Enabled && SubmitEnabled)}");
        return ok;
    }

    static bool Queue(ref bool queued, System.Reflection.MethodInfo target, string method)
    {
        if (queued) return true;
        var impl = typeof(ModelWalk).GetMethod(method,
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
        queued = HookManager.AddReplace(_self, target, impl);
        return queued;
    }

    // PGXP follows values through the registers, which locals do not have.
    static bool Recompiled(bool on) => !on || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking;

    static void ReplaceWalk(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && WalkEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_walkCheck, orig, c, mem, RunWalk);
        else RunWalk(c, mem);
    }

    static void ReplaceSubmit(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Recompiled(Enabled && SubmitEnabled) || m is not PSMemory mem) { orig(c, m); return; }
        if (_mode == Mode.Verify) Verify(_submitCheck, orig, c, mem, RunSubmit);
        else RunSubmit(c, mem);
    }

    // ---- func_800331B4: the four table walks --------------------------------

    /// <summary>Walks the creature, object, effect and billboard tables in that
    /// order, submitting what is live and visible and doing each table's own
    /// bookkeeping either way. Two scratch bitmaps in the frame — texture pages
    /// wanted at `sp+0x58` and CLUT pages at `sp+0x198` — are filled by every
    /// record that got as far as being considered, and handed to `func_8003309C`
    /// and `func_80032FAC` between the first and second table and again after the
    /// second. The `Interrupts.Poll` calls are the recompiled loop heads' and are
    /// kept.</summary>
    static void RunWalk(CpuContext c, PSMemory mem)
    {
        uint sp = c.SP - 0x300u;
        c.SP = sp;
        mem.WriteU32(sp + 0x2FCu, c.RA);
        mem.WriteU32(sp + 0x2F8u, c.FP);
        mem.WriteU32(sp + 0x2F4u, c.S7);
        mem.WriteU32(sp + 0x2F0u, c.S6);
        mem.WriteU32(sp + 0x2ECu, c.S5);
        mem.WriteU32(sp + 0x2E8u, c.S4);
        mem.WriteU32(sp + 0x2E4u, c.S3);
        mem.WriteU32(sp + 0x2E0u, c.S2);
        mem.WriteU32(sp + 0x2DCu, c.S1);
        mem.WriteU32(sp + 0x2D8u, c.S0);

        _sceneCount = 0;
        _walkOwns = true;
        _liveCreatures = _liveObjects = _liveEffects = _liveSprites = 0;

        Memset(c, mem, sp + 0x58u, 0x20u, 0x800331F0u);
        Memset(c, mem, sp + 0x198u, 0x10u, 0x80033200u);

        WalkCreatures(c, mem, sp);

        // The two page bitmaps, spent and cleared between the tables.
        mem.WriteU32(sp + 0x10u, sp + 0x58u);
        Pages(c, mem, 0u, 0u, 0x80u, 0x80u, 0x8003344Cu);
        mem.WriteU32(sp + 0x10u, sp + 0x198u);
        Cluts(c, mem, 4u, 0x20u, 2u, 0x40u, 0x80033468u);
        Memset(c, mem, sp + 0x58u, 0x50u, 0x80033480u);
        uint soundClock = mem.ReadU32(SoundClock);
        Memset(c, mem, sp + 0x198u, 0x10u, 0x80033490u);

        WalkObjects(c, mem, sp, soundClock);

        mem.WriteU32(sp + 0x10u, sp + 0x58u);
        Pages(c, mem, 0u, 0x80u, 0x100u, 0x140u, 0x80033954u);
        mem.WriteU32(sp + 0x10u, sp + 0x198u);
        Cluts(c, mem, 4u, 0x60u, 0x42u, 0x40u, 0x80033970u);

        WalkEffects(c, mem, sp);
        WalkSprites(c, mem, sp);

        mem.WriteU32(SpriteClock, mem.ReadU32(SpriteClock) + 1u);
        _walkOwns = false;

        c.RA = mem.ReadU32(sp + 0x2FCu);
        c.FP = mem.ReadU32(sp + 0x2F8u);
        c.S7 = mem.ReadU32(sp + 0x2F4u);
        c.S6 = mem.ReadU32(sp + 0x2F0u);
        c.S5 = mem.ReadU32(sp + 0x2ECu);
        c.S4 = mem.ReadU32(sp + 0x2E8u);
        c.S3 = mem.ReadU32(sp + 0x2E4u);
        c.S2 = mem.ReadU32(sp + 0x2E0u);
        c.S1 = mem.ReadU32(sp + 0x2DCu);
        c.S0 = mem.ReadU32(sp + 0x2D8u);
        c.SP = sp + 0x300u;

        WalkCalls++;
        _lastCount = _sceneCount;
        Report();
    }

    /// <summary>200 records of 0x7C at `0x8016C544`, live when `u8[+0x9] == 1`. The
    /// visibility query is the point one unless `u32[+0x28]` has `0x80000`, which
    /// asks the volume one instead; `0x2000` adds `0x20` to the mask the answer is
    /// tested with, and `0x20` places the creature at its own record with no
    /// rotation and a fixed matrix instead of through `func_8003CE44`.</summary>
    static void WalkCreatures(CpuContext c, PSMemory mem, uint sp)
    {
        uint rec = Creatures;
        for (int i = 0; i < CreatureCount; i++, rec += CreatureStride)
        {
            Interrupts.Poll(c, mem);
            if (mem.ReadU8(rec + 0x9u) != 1u) continue;
            _liveCreatures++;

            uint flags = mem.ReadU32(rec + 0x28u);
            uint mask = (flags & 0x2000u) != 0u ? mem.ReadU8(rec + 3u) | 0x20u : mem.ReadU8(rec + 3u);

            if ((mem.ReadU32(rec + 0x28u) & 0x80000u) != 0u)
            {
                c.A0 = rec + 0x2Cu;
                c.A1 = 3u;
                c.RA = 0x800333F8u;
                KingsField2.func_80032DE8(c, mem);
                if ((c.V0 & mem.ReadU8(rec + 3u)) == 0u) continue;
            }
            else
            {
                c.A0 = rec + 0x2Cu;
                c.RA = 0x80033268u;
                KingsField2.func_80032D78(c, mem);
                if ((c.V0 & mask) == 0u) continue;
            }

            Interrupts.Poll(c, mem);
            c.A0 = mem.ReadU8(rec + 1u) + 0x80u;
            c.RA = 0x80033280u;
            KingsField2.func_80032CD8(c, mem);
            if (c.V0 != 0u)
            {
                c.A0 = rec;
                c.A1 = sp + 0x48u;
                c.RA = 0x80033290u;
                KingsField2.func_8003CE44(c, mem);
                uint pos = c.V0;

                if ((mem.ReadU32(rec + 0x28u) & 0x20u) != 0u)
                {
                    mem.WriteU16(sp + 0x3Cu, 0);
                    mem.WriteU16(sp + 0x3Au, 0);
                    mem.WriteU16(sp + 0x38u, 0);
                    mem.WriteU32(sp + 0x18u, RomMatrix);
                    pos = rec + 0x2Cu;
                }
                else
                {
                    mem.WriteU16(sp + 0x38u, mem.ReadU16(rec + 0x40u));
                    mem.WriteU16(sp + 0x3Au, (ushort)(mem.ReadU16(rec + 0x42u) + 0x800u));
                    mem.WriteU16(sp + 0x3Cu, mem.ReadU16(rec + 0x44u));
                    mem.WriteU32(sp + 0x18u, ViewMatrix);
                }

                mem.WriteU32(sp + 0x10u, rec + 0x48u);
                mem.WriteU32(sp + 0x14u, rec + 0x5Cu);
                mem.WriteU32(sp + 0x1Cu, mem.ReadU8(rec + 0xCu));
                mem.WriteU32(sp + 0x20u, mem.ReadU16(rec + 0x18u));
                mem.WriteU32(sp + 0x24u, mem.ReadU8(rec + 0x14u));
                mem.WriteU32(sp + 0x28u, (uint)(short)mem.ReadU16(rec + 0x16u));
                mem.WriteU32(sp + 0x2Cu, mem.ReadU8(rec + 0x13u));
                mem.WriteU32(sp + 0x30u, (uint)(sbyte)mem.ReadU8(rec + 0x15u));

                _kind = ModelKind.Creature; _slot = i; _record = rec;
                _creatures++;
                c.A0 = mem.ReadU8(rec + 3u);
                c.A1 = mem.ReadU8(rec + 1u) + 0x80u;
                c.A2 = pos;
                c.A3 = sp + 0x38u;
                c.RA = 0x8003339Cu;
                KingsField2.func_80032588(c, mem);
            }

            // Bookkeeping, whether or not the model drew: the definition's two
            // pages and the creature's own texture page are wanted this frame.
            uint def = CreatureDefs + ((mem.ReadU8(rec + 2u) * 15u) << 3);
            mem.WriteU8(sp + 0x198u + mem.ReadU8(def + 7u), 1);
            mem.WriteU8(sp + 0x198u + mem.ReadU8(def + 8u), 1);
            mem.WriteU8(sp + 0x58u + mem.ReadU8(rec + 1u), 1);
        }
    }

    /// <summary>396 records of 0x44 at `0x80177714`, free when `u16[+0x6] == 0xFF`.
    /// `u8[+0x4]` is the kind: `0x1F` is an ambient sound source and draws nothing,
    /// `0xF0` is a special submitted through `func_80032AC4`, and everything else
    /// is an ordinary model. `u8[+0x3]` bit 1 asks the volume visibility query
    /// rather than the point one, and bit 0 forces the assembler.</summary>
    static void WalkObjects(CpuContext c, PSMemory mem, uint sp, uint soundClock)
    {
        uint rec = Objects;
        for (int i = 0; i < ObjectCount; i++, rec += ObjectStride)
        {
            Interrupts.Poll(c, mem);
            if (mem.ReadU16(rec + 0x6u) == 0xFFu) continue;
            _liveObjects++;

            uint kindByte = mem.ReadU8(rec + 3u);
            uint kind = mem.ReadU8(rec + 4u);
            mem.WriteU8(rec + 3u, (byte)(kindByte & 0x7Fu));

            if (kind == 0x1Fu) { Ambient(c, mem, sp, rec, soundClock); continue; }

            if (kind == 0xF0u)
            {
                c.A0 = rec + 0x14u;
                c.A1 = mem.ReadU8(rec + 0x38u);
                c.A2 = mem.ReadU8(rec + 0x39u);
                c.RA = 0x800334F4u;
                KingsField2.func_80032EAC(c, mem);
                if (c.V0 == 0u) continue;
                if ((mem.ReadU8(rec) & mem.ReadU8(ObjectMask)) == 0u) continue;

                c.A0 = mem.ReadU16(rec + 0x6u) + 0x100u;
                c.RA = 0x80033524u;
                KingsField2.func_80032CD8(c, mem);
                if (c.V0 != 0u)
                {
                    mem.WriteU32(sp + 0x10u, mem.ReadU16(rec + 0xAu));
                    mem.WriteU32(sp + 0x14u, mem.ReadU8(rec + 0x3Cu));
                    mem.WriteU32(sp + 0x18u, mem.ReadU8(rec + 0x3Bu));
                    mem.WriteU32(sp + 0x1Cu, 0x1FFFu - mem.ReadU8(rec + 0x3Au));
                    c.A0 = mem.ReadU16(rec + 0x6u) + 0x100u;
                    c.A1 = rec + 0x24u;
                    c.A2 = rec + 0x34u;
                    c.A3 = mem.ReadU8(rec + 1u);
                    c.RA = 0x8003356Cu;
                    KingsField2.func_80032AC4(c, mem);
                    mem.WriteU8(rec + 3u, (byte)(mem.ReadU8(rec + 3u) | 0x80u));
                }
                mem.WriteU8(sp + 0x58u + mem.ReadU16(rec + 0x6u), 1);
                continue;
            }

            // The ordinary model. Bit 1 of the kind byte picks the volume query,
            // which is asked with the definition's own mask.
            uint seen;
            if ((kindByte & 2u) != 0u)
            {
                uint def = ObjectDefs + (mem.ReadU16(rec + 0x6u) * 24u);
                c.A0 = rec + 0x14u;
                c.A1 = mem.ReadU8(def + 0xCu);
                c.RA = 0x80033900u;
                KingsField2.func_80032DE8(c, mem);
                seen = c.V0;
            }
            else
            {
                c.A0 = rec + 0x14u;
                c.RA = 0x800337A8u;
                KingsField2.func_80032D78(c, mem);
                seen = c.V0;
            }
            if ((seen & mem.ReadU8(rec)) == 0u) continue;

            Interrupts.Poll(c, mem);
            uint model = mem.ReadU16(rec + 0x6u);
            mem.WriteU8(sp + 0x58u + model, 1);
            mem.WriteU8(sp + 0x198u + mem.ReadU8(ObjectDefs + model * 24u + 2u), 1);

            c.A0 = model + 0x100u;
            c.RA = 0x8003380Cu;
            KingsField2.func_80032CD8(c, mem);
            if (c.V0 == 0u) continue;

            mem.WriteU16(sp + 0x38u, mem.ReadU16(rec + 0x24u));
            mem.WriteU16(sp + 0x3Au, (ushort)(mem.ReadU16(rec + 0x26u) + 0x800u));
            mem.WriteU16(sp + 0x3Cu, mem.ReadU16(rec + 0x28u));

            uint assembler = mem.ReadU8(rec + 2u);
            if ((mem.ReadU8(rec + 3u) & 1u) != 0u)
                assembler = (seen & 0x80u) != 0u ? 0xFEu : 0xFFu;

            mem.WriteU32(sp + 0x10u, rec + 0x2Cu);
            mem.WriteU32(sp + 0x14u, rec + 0x34u);
            mem.WriteU32(sp + 0x18u, ViewMatrix);
            mem.WriteU32(sp + 0x1Cu, mem.ReadU8(rec + 1u));
            mem.WriteU32(sp + 0x20u, mem.ReadU16(rec + 0xAu));
            mem.WriteU32(sp + 0x24u, mem.ReadU8(rec + 5u));
            mem.WriteU32(sp + 0x28u, mem.ReadU16(rec + 0x10u));
            mem.WriteU32(sp + 0x2Cu, assembler);
            mem.WriteU32(sp + 0x30u, (uint)(short)mem.ReadU16(rec + 0xEu));

            _kind = ModelKind.Object; _slot = i; _record = rec;
            _objects++;
            c.A0 = mem.ReadU8(rec);
            c.A1 = model + 0x100u;
            c.A2 = rec + 0x14u;
            c.A3 = sp + 0x38u;
            c.RA = 0x800338C0u;
            KingsField2.func_80032588(c, mem);
            mem.WriteU8(rec + 3u, (byte)(mem.ReadU8(rec + 3u) | 0x80u));
        }
    }

    /// <summary>Kind `0x1F`: an ambient sound source. It draws nothing — it asks
    /// `func_80037810` whether its tile is reachable, ranges itself against the
    /// player on the larger of the two axis distances, attenuates by the record's
    /// own falloff and plays through `func_80014158` when the retrigger interval
    /// at `+0x1C` has come due against the clock at `0x801B6CAC`.</summary>
    static void Ambient(CpuContext c, PSMemory mem, uint sp, uint rec, uint clock)
    {
        mem.WriteU32(sp + 0x10u, 0x8000u);
        c.A0 = (uint)((int)mem.ReadU32(rec + 0x14u) >> 11);
        c.A1 = (uint)((int)mem.ReadU32(rec + 0x1Cu) >> 11);
        c.A2 = mem.ReadU8(rec + 0x38u);
        c.A3 = mem.ReadU8(rec + 0x39u);
        c.RA = 0x800335B4u;
        KingsField2.func_80037810(c, mem);
        if (c.V0 == 0u)
        {
            // Unreachable: park the next due time and leave.
            mem.WriteU32(rec + 0x40u, clock + mem.ReadU16(rec + 0x3Eu) * 6u);
            return;
        }

        uint sound = mem.ReadU16(EffectSounds + mem.ReadU8(rec + 0x3Au) * 10u);
        if (sound - 0x42u < 0x40u) mem.WriteU8(sp + 0x156u + (uint)(short)sound, 1);

        if ((int)(mem.ReadU32(rec + 0x40u) - clock) >= 0) return;
        mem.WriteU32(rec + 0x40u, clock + mem.ReadU16(rec + 0x3Eu) * 6u);

        // The range is the nearer of the two axis distances, each measured from the
        // record's own half-extent rather than from its centre.
        int spanX = (int)mem.ReadU8(rec + 0x38u) << 10;
        int dx = (int)(mem.ReadU32(PlayerPos) - ((uint)spanX + mem.ReadU32(rec + 0x14u)));
        if (dx < 0) dx = -dx;
        dx = spanX - dx;

        int spanZ = (int)mem.ReadU8(rec + 0x39u) << 10;
        int dz = (int)(mem.ReadU32(PlayerPos + 8u) - ((uint)spanZ + mem.ReadU32(rec + 0x1Cu)));
        if (dz < 0) dz = -dz;
        dz = spanZ - dz;
        if (dz < dx) dx = dz;

        // Inside the falloff the level is scaled by how far in it is; the divisor
        // is the falloff itself, and a zero one is the record asking not to play.
        int falloff = (int)mem.ReadU8(rec + 0x3Cu) << 11;
        int level;
        if (dx < falloff)
        {
            if (falloff == 0) return;
            level = (int)mem.ReadU8(rec + 0x3Bu) * dx / falloff;
        }
        else level = mem.ReadU8(rec + 0x3Bu);

        if ((mem.ReadU8(rec + 0x3Du) & 1u) != 0u)
        {
            int dy = (int)(mem.ReadU32(PlayerY) - mem.ReadU32(rec + 0x18u));
            if (dy < 0) dy = -dy;
            level -= (int)mem.ReadU8(rec + 0x3Bu) * dy >> 13;
        }
        if (level < 20) return;

        _ambients++;
        c.A0 = mem.ReadU8(rec + 0x3Au);
        c.RA = 0x80033770u;
        KingsField2.func_80014158(c, mem);
    }

    /// <summary>128 records of 0x48 at `0x8019CC6C`, free when `u8[+0x0] == 0xFF`.
    /// `u8[+0x8]` low two bits gate it (0 skips, 2 skips the visibility query) and
    /// bits 2-3 pick the placement: 0 is a rotated world model, 4 the fixed matrix,
    /// 8 the flat matrix and 0xC no matrix at all with a fixed OT bias of 20.</summary>
    static void WalkEffects(CpuContext c, PSMemory mem, uint sp)
    {
        uint rec = Effects;
        for (int i = 0; i < EffectCount; i++, rec += EffectStride)
        {
            Interrupts.Poll(c, mem);
            if (mem.ReadU8(rec) == 0xFFu) continue;
            _liveEffects++;

            uint gate = mem.ReadU8(rec + 8u);
            uint live = gate & 3u;
            if (live == 0u) continue;
            if (live != 2u)
            {
                c.A0 = rec + 0x14u;
                c.RA = 0x800339C8u;
                KingsField2.func_80032D78(c, mem);
                if ((c.V0 & mem.ReadU8(rec + 0xAu)) == 0u) continue;
            }

            uint place = mem.ReadU8(rec + 8u) & 0xCu;
            if (place != 0u && place != 4u && place != 8u && place != 0xCu) continue;

            uint matrix;
            uint rot;
            uint bias;
            if (place == 0u)
            {
                mem.WriteU16(sp + 0x38u, mem.ReadU16(rec + 0x24u));
                mem.WriteU16(sp + 0x3Au, (ushort)(mem.ReadU16(rec + 0x26u) + 0x800u));
                mem.WriteU16(sp + 0x3Cu, mem.ReadU16(rec + 0x28u));
                matrix = ViewMatrix;
                rot = sp + 0x38u;
                bias = 0xFFFFFFC4u;
            }
            else if (place == 4u) { matrix = RomMatrix; rot = rec + 0x24u; bias = 0xFFFFFFC4u; }
            else if (place == 8u) { matrix = FlatMatrix; rot = rec + 0x24u; bias = 0xFFFFFFC4u; }
            else { matrix = 0u; rot = rec + 0x24u; bias = 0x14u; }

            mem.WriteU32(sp + 0x10u, rec + 0x2Cu);
            mem.WriteU32(sp + 0x14u, rec + 0x3Cu);
            mem.WriteU32(sp + 0x18u, matrix);
            mem.WriteU32(sp + 0x1Cu, mem.ReadU8(rec + 4u));
            mem.WriteU32(sp + 0x20u, mem.ReadU16(rec + 0x12u));
            mem.WriteU32(sp + 0x24u, mem.ReadU8(rec + 0xCu));
            mem.WriteU32(sp + 0x28u, (uint)(short)mem.ReadU16(rec + 0x10u));
            mem.WriteU32(sp + 0x2Cu, mem.ReadU8(rec + 9u));
            mem.WriteU32(sp + 0x30u, bias);

            _kind = ModelKind.Effect; _slot = i; _record = rec;
            _effects++;
            c.A0 = mem.ReadU8(rec + 0xAu);
            c.A1 = mem.ReadU8(rec + 3u) + 0x28u;
            c.A2 = rec + 0x14u;
            c.A3 = rot;
            c.RA = place == 0u ? 0x80033B18u : 0x80033B78u;
            KingsField2.func_80032588(c, mem);
        }
    }

    /// <summary>128 records of 0x18 at `0x80195174`, free when `u16[+0x0] == 0xFFFF`.
    /// Always the flat matrix and no rotation; the cel at `+0x5` steps when the
    /// shared clock divides by the interval at `+0x4` and wraps at the strip length
    /// in `+0x3`. `SpriteAnim` holds that stepping to the world tick.</summary>
    static void WalkSprites(CpuContext c, PSMemory mem, uint sp)
    {
        mem.WriteU16(sp + 0x3Cu, 0);
        mem.WriteU16(sp + 0x3Au, 0);
        mem.WriteU16(sp + 0x38u, 0);

        uint rec = Sprites;
        for (int i = 0; i < SpriteCount; i++, rec += SpriteStride)
        {
            Interrupts.Poll(c, mem);
            if (mem.ReadU16(rec) == 0xFFFFu) continue;
            _liveSprites++;

            c.A0 = rec + 8u;
            c.RA = 0x80033BDCu;
            KingsField2.func_80032D78(c, mem);
            uint mask = mem.ReadU8(rec + 2u);
            if ((c.V0 & mask) != 0u)
            {
                mem.WriteU32(sp + 0x10u, 0u);
                mem.WriteU32(sp + 0x14u, 0u);
                mem.WriteU32(sp + 0x18u, FlatMatrix);
                mem.WriteU32(sp + 0x1Cu, mem.ReadU8(rec + 5u) + 0x80u);
                mem.WriteU32(sp + 0x20u, 0u);
                mem.WriteU32(sp + 0x24u, 0x46u);
                mem.WriteU32(sp + 0x28u, 0x1000u);
                mem.WriteU32(sp + 0x2Cu, 1u);
                mem.WriteU32(sp + 0x30u, 0u);

                _kind = ModelKind.Sprite; _slot = i; _record = rec;
                _sprites++;
                c.A0 = mask;
                c.A1 = mem.ReadU16(rec) + 0x28u;
                c.A2 = rec + 8u;
                c.A3 = sp + 0x38u;
                c.RA = 0x80033C40u;
                KingsField2.func_80032588(c, mem);
            }

            uint interval = mem.ReadU8(rec + 4u);
            if (interval == 0u) continue;
            if ((int)mem.ReadU32(SpriteClock) % (int)interval != 0) continue;
            uint cel = mem.ReadU8(rec + 5u) + 1u;
            mem.WriteU8(rec + 5u, (byte)cel);
            if (mem.ReadU8(rec + 5u) < mem.ReadU8(rec + 3u)) continue;
            mem.WriteU8(rec + 5u, 0);
        }
    }

    static void Memset(CpuContext c, PSMemory mem, uint dst, uint n, uint ra)
    {
        c.A0 = dst;
        c.A1 = 0u;
        c.A2 = n;
        c.RA = ra;
        KingsField2.func_800172A4(c, mem);
    }

    /// <summary>Upload the texture pages the bitmap at `sp+0x58` asked for.</summary>
    static void Pages(CpuContext c, PSMemory mem, uint a0, uint a1, uint a2, uint a3, uint ra)
    {
        c.A0 = a0; c.A1 = a1; c.A2 = a2; c.A3 = a3; c.RA = ra;
        KingsField2.func_8003309C(c, mem);
    }

    /// <summary>The same for the CLUTs the bitmap at `sp+0x198` asked for.</summary>
    static void Cluts(CpuContext c, PSMemory mem, uint a0, uint a1, uint a2, uint a3, uint ra)
    {
        c.A0 = a0; c.A1 = a1; c.A2 = a2; c.A3 = a3; c.RA = ra;
        KingsField2.func_80032FAC(c, mem);
    }

    // ---- func_80032588: one model, set up and assembled ----------------------

    /// <summary>`a0` the visibility byte (1 selects the tile's lower half), `a1` the
    /// model id, `a2` the position, `a3` the rotation triple, and nine more on the
    /// stack: the scale vector, the MO record, a matrix, the clip byte, the MO clip
    /// time, a second light record, the blend weight, the assembler byte and the OT
    /// depth. A matrix of 0 is the view-space placement — the position is taken raw
    /// and the light comes from the camera's own tile instead of the model's.
    /// </summary>
    static void RunSubmit(CpuContext c, PSMemory mem)
    {
        uint arg = c.SP;
        uint sp = arg - 0xD8u;
        c.SP = sp;
        mem.WriteU32(sp + 0xD4u, c.RA);
        mem.WriteU32(sp + 0xD0u, c.FP);
        mem.WriteU32(sp + 0xCCu, c.S7);
        mem.WriteU32(sp + 0xC8u, c.S6);
        mem.WriteU32(sp + 0xC4u, c.S5);
        mem.WriteU32(sp + 0xC0u, c.S4);
        mem.WriteU32(sp + 0xBCu, c.S3);
        mem.WriteU32(sp + 0xB8u, c.S2);
        mem.WriteU32(sp + 0xB4u, c.S1);
        mem.WriteU32(sp + 0xB0u, c.S0);

        uint pos = c.A2;
        uint rotPtr = c.A3;
        uint depth = mem.ReadU32(arg + 0x30u);
        uint assembler = mem.ReadU8(arg + 0x2Cu);
        uint scale = mem.ReadU32(arg + 0x10u);
        uint clipByte = mem.ReadU16(arg + 0x1Cu);
        uint light2 = mem.ReadU8(arg + 0x24u);
        uint weight = mem.ReadU16(arg + 0x28u);
        uint half = c.A0;
        uint matrix = mem.ReadU32(arg + 0x18u);
        uint moRec = mem.ReadU32(arg + 0x14u);
        uint clipTime = mem.ReadU16(arg + 0x20u);
        ushort model = (ushort)c.A1;

        mem.WriteU16(sp + 0xA0u, (ushort)clipByte);
        mem.WriteU16(sp + 0x98u, model);
        mem.WriteU16(sp + 0xA8u, (ushort)clipTime);

        c.A0 = ViewMatrix;
        c.RA = 0x800325F4u;
        KingsField2.SetRotMatrix(c, mem);
        c.A0 = ViewMatrix;
        c.RA = 0x80032604u;
        KingsField2.SetTransMatrix(c, mem);

        uint light;
        if (matrix != 0u)
        {
            // Placed in the world: project it, and light it from its own map tile.
            mem.WriteU16(sp + 0x18u, (ushort)(mem.ReadU16(pos + 0u) - mem.ReadU16(CamWorldX)));
            mem.WriteU16(sp + 0x1Au, (ushort)(mem.ReadU16(pos + 4u) - mem.ReadU16(CamWorldY)));
            mem.WriteU16(sp + 0x1Cu, (ushort)(mem.ReadU16(pos + 8u) - mem.ReadU16(CamWorldZ)));
            c.A0 = sp + 0x18u;
            c.A1 = sp + 0x44u;
            c.A2 = sp + 0x90u;
            c.RA = 0x80032664u;
            KingsField2.RotTrans(c, mem);

            uint tz = (uint)((int)mem.ReadU32(pos + 8u) >> 11);
            uint tx = (uint)((int)mem.ReadU32(pos + 0u) >> 11);
            uint tile = MapBase + ((tz * 25u) << 5) + ((tx * 5u) << 1);
            if ((half & 0xFFu) != 1u) tile += 5u;
            light = LightBase + (mem.ReadU8(tile + 4u) & 0x3Fu) * 104u;
        }
        else
        {
            // View space: the position is already where it will be drawn, and the
            // light is the camera's own tile's.
            mem.WriteU32(sp + 0x44u, mem.ReadU32(pos + 0u));
            mem.WriteU32(sp + 0x48u, mem.ReadU32(pos + 4u));
            mem.WriteU32(sp + 0x4Cu, mem.ReadU32(pos + 8u));

            uint cz = (uint)((int)mem.ReadU32(CamWorldZ) >> 11);
            uint cx = (uint)((int)mem.ReadU32(CamWorldX) >> 11);
            uint off = ((cz * 25u) << 5) + ((cx * 5u) << 1) + mem.ReadU16(HalfSelect);
            light = LightBase + (mem.ReadU8(MapBase + 4u + off) & 0x3Fu) * 104u;
        }

        // 0x80 is the lit assembler with no depth bias; everything else is pushed
        // back 240 entries when the model sits at or above the camera.
        if ((assembler & 0xFFu) == 0x80u) assembler = 0xFFu;
        else if ((short)mem.ReadU16(sp + 0x1Au) <= 0) depth += 0xF0u;

        c.A0 = rotPtr;
        c.A1 = sp + 0x30u;
        c.RA = 0x800327B4u;
        KingsField2.func_80014FE0(c, mem);

        if (scale != 0u)
        {
            mem.WriteU32(sp + 0x20u, (uint)(short)mem.ReadU16(scale + 0u));
            mem.WriteU32(sp + 0x24u, (uint)(short)mem.ReadU16(scale + 2u));
            mem.WriteU32(sp + 0x28u, (uint)(short)mem.ReadU16(scale + 4u));
            c.A0 = sp + 0x30u;
            c.A1 = sp + 0x20u;
            c.RA = 0x800327E4u;
            KingsField2.ScaleMatrix(c, mem);
        }

        Light(c, mem, sp, light, light2 & 0xFFu, (uint)(short)weight);

        c.A0 = sp + 0x50u;
        c.RA = 0x80032964u;
        KingsField2.SetLightMatrix(c, mem);

        Placed = matrix == ViewMatrix;
        if (Placed)
        {
            for (uint i = 0; i < 9; i++) PlacedRot[i] = (short)mem.ReadU16(sp + 0x30u + i * 2u);
            PlacedX = (int)mem.ReadU32(pos + 0u);
            PlacedY = (int)mem.ReadU32(pos + 4u);
            PlacedZ = (int)mem.ReadU32(pos + 8u);
        }
        if (matrix != 0u)
        {
            c.A0 = matrix;
            c.A1 = sp + 0x30u;
            c.RA = 0x80032980u;
            KingsField2.MulMatrix2(c, mem);
        }
        c.A0 = sp + 0x30u;
        c.RA = 0x8003298Cu;
        KingsField2.SetRotMatrix(c, mem);
        c.A0 = sp + 0x30u;
        c.RA = 0x80032994u;
        KingsField2.SetTransMatrix(c, mem);

        uint id = mem.ReadU16(sp + 0x98u);
        c.A0 = id;
        c.RA = 0x800329A0u;
        KingsField2.func_80034834(c, mem);

        uint clip = mem.ReadU16(sp + 0xA0u);
        uint mesh, sub;
        if (clip < 0x80u)
        {
            // An MO-animated mesh: the blender poses model 0 into the scratch mesh.
            sub = 0u;
            c.A0 = 0u;
            c.RA = 0x800329BCu;
            KingsField2.func_8002E1BC(c, mem);
            mesh = c.V0;

            mem.WriteU32(sp + 0x10u, mem.ReadU32(mesh + 4u));
            c.A0 = moRec;
            c.A1 = id;
            c.A2 = clip;
            c.A3 = mem.ReadU16(sp + 0xA8u);
            c.RA = 0x800329E0u;
            KingsField2.func_80034DA8(c, mem);
            if (c.V0 == 0u)
            {
                c.A0 = 0u;
                c.RA = 0x800329F0u;
                KingsField2.func_8002E1F0(c, mem);
            }
        }
        else
        {
            // A rigid mesh, its sub-model index the low seven bits of the clip byte.
            sub = mem.ReadU16(sp + 0xA0u) & 0x7Fu;
            c.A0 = sub & 0xFFFFu;
            c.RA = 0x80032A10u;
            KingsField2.func_8002E1F0(c, mem);
            c.A0 = sub & 0xFFFFu;
            c.RA = 0x80032A18u;
            KingsField2.func_8002E1BC(c, mem);
            mesh = c.V0;
        }

        if (matrix != 0u)
        {
            c.A0 = mem.ReadU32(mesh + 4u);
            c.RA = 0x80032A38u;
            KingsField2.func_8002E650(c, mem);
        }
        else
        {
            c.A0 = mem.ReadU32(mesh + 4u);
            c.A1 = depth;
            c.RA = 0x80032A4Cu;
            KingsField2.func_8002E9B8(c, mem);
        }

        Record(mem, pos, model, (byte)(assembler & 0xFFu), light);

        // The remaster's material for this model, sealed into every packet it makes.
        _model = _walkOwns ? model : -1;
        PolyAssembler.TileMaterial = _walkOwns ? Remaster.Surfaces.EnterModel(_kind, model) : (byte)0;

        uint pick = assembler & 0xFFu;
        if (pick == 0xFFu)
        {
            _lit++;
            c.A0 = sub;
            c.A1 = depth;
            c.RA = 0x80032A68u;
            KingsField2.func_8002F214(c, mem);
        }
        else if (pick == 0xFEu)
        {
            _flat++;
            c.A0 = sub;
            c.A1 = depth;
            c.A2 = 0u;
            c.RA = 0x80032A84u;
            KingsField2.func_80030540(c, mem);
        }
        else
        {
            _semi++;
            c.A0 = sub;
            c.A1 = depth;
            c.A2 = pick;
            c.RA = 0x80032A94u;
            KingsField2.func_8002EAEC(c, mem);
        }
        PolyAssembler.TileMaterial = 0;
        _model = -1;
        Placed = false;

        c.RA = mem.ReadU32(sp + 0xD4u);
        c.FP = mem.ReadU32(sp + 0xD0u);
        c.S7 = mem.ReadU32(sp + 0xCCu);
        c.S6 = mem.ReadU32(sp + 0xC8u);
        c.S5 = mem.ReadU32(sp + 0xC4u);
        c.S4 = mem.ReadU32(sp + 0xC0u);
        c.S3 = mem.ReadU32(sp + 0xBCu);
        c.S2 = mem.ReadU32(sp + 0xB8u);
        c.S1 = mem.ReadU32(sp + 0xB4u);
        c.S0 = mem.ReadU32(sp + 0xB0u);
        c.SP = sp + 0xD8u;
        SubmitCalls++;
    }

    /// <summary>The colour matrix, the depth cue, the back colour and the light
    /// matrix. A second light record blends each of the four against the first on
    /// the caller's weight, and each part opts out on its own sentinel — `-1` for
    /// the matrices and the cue, `0xFF` for the back colour — so a record can carry
    /// only the parts it wants to change.</summary>
    static void Light(CpuContext c, PSMemory mem, uint sp, uint a, uint b, uint weight)
    {
        if (b == 0xFFu)
        {
            c.A0 = a + 0x50u;
            c.RA = 0x8003292Cu;
            KingsField2.SetColorMatrix(c, mem);
            c.A0 = (uint)(short)mem.ReadU16(a + 0x66u);
            c.RA = 0x80032938u;
            KingsField2.func_8002DDDC(c, mem);
            c.A0 = mem.ReadU8(a + 0x62u);
            c.A1 = mem.ReadU8(a + 0x63u);
            c.A2 = mem.ReadU8(a + 0x64u);
            c.RA = 0x8003294Cu;
            KingsField2.SetBackColor_game(c, mem);
            c.A0 = a;
            c.A1 = sp + 0x30u;
            c.A2 = sp + 0x50u;
            c.RA = 0x8003295Cu;
            KingsField2.MulMatrix0(c, mem);
            return;
        }

        uint other = LightBase + b * 104u;

        if ((short)mem.ReadU16(other + 0x50u) == -1)
        {
            c.A0 = a + 0x50u;
            c.RA = 0x80032850u;
            KingsField2.SetColorMatrix(c, mem);
        }
        else
        {
            c.A0 = a + 0x50u;
            c.A1 = other + 0x50u;
            c.A2 = sp + 0x70u;
            c.A3 = weight;
            c.RA = 0x80032838u;
            KingsField2.func_80015930(c, mem);
            c.A0 = sp + 0x70u;
            c.RA = 0x80032840u;
            KingsField2.SetColorMatrix(c, mem);
        }

        c.A0 = (short)mem.ReadU16(other) == -1 ? a : other;
        c.A1 = sp + 0x30u;
        c.A2 = sp + 0x50u;
        c.RA = 0x80032870u;
        KingsField2.MulMatrix0(c, mem);

        uint cue = (uint)(short)mem.ReadU16(other + 0x66u);
        if ((int)cue == -1)
        {
            c.A0 = (uint)(short)mem.ReadU16(a + 0x66u);
            c.RA = 0x800328A8u;
            KingsField2.func_8002DDDC(c, mem);
        }
        else
        {
            c.A0 = (uint)(short)mem.ReadU16(a + 0x66u);
            c.A1 = cue;
            c.A2 = weight;
            c.RA = 0x8003288Cu;
            KingsField2.func_800158C8(c, mem);
            c.A0 = c.V0;
            c.RA = 0x80032894u;
            KingsField2.func_8002DDDC(c, mem);
        }

        if (mem.ReadU8(other + 0x62u) == 0xFFu)
        {
            c.A0 = mem.ReadU8(a + 0x62u);
            c.A1 = mem.ReadU8(a + 0x63u);
            c.A2 = mem.ReadU8(a + 0x64u);
            c.RA = 0x8003291Cu;
            KingsField2.SetBackColor_game(c, mem);
            return;
        }

        c.A0 = mem.ReadU8(a + 0x62u);
        c.A1 = mem.ReadU8(other + 0x62u);
        c.A2 = weight;
        c.RA = 0x800328C8u;
        KingsField2.func_800158C8(c, mem);
        uint r = c.V0;
        c.A0 = mem.ReadU8(a + 0x63u);
        c.A1 = mem.ReadU8(other + 0x63u);
        c.A2 = weight;
        c.RA = 0x800328DCu;
        KingsField2.func_800158C8(c, mem);
        uint g = c.V0;
        c.A0 = mem.ReadU8(a + 0x64u);
        c.A1 = mem.ReadU8(other + 0x64u);
        c.A2 = weight;
        c.RA = 0x800328F0u;
        KingsField2.func_800158C8(c, mem);
        c.A0 = r;
        c.A1 = g;
        c.A2 = c.V0;
        c.RA = 0x80032900u;
        KingsField2.SetBackColor_game(c, mem);
    }

    /// <summary>The scene entry, filled from both halves: the walk left the kind,
    /// the slot and the record, and the submitter adds what it resolved.</summary>
    static void Record(PSMemory mem, uint pos, ushort model, byte assembler, uint light)
    {
        if (!_walkOwns) return;
        if (_sceneCount == _scene.Length) Array.Resize(ref _scene, _scene.Length * 2);
        _scene[_sceneCount++] = new ModelDraw(_kind, _slot, _record, model,
                                              (int)mem.ReadU32(pos + 0u),
                                              (int)mem.ReadU32(pos + 4u),
                                              (int)mem.ReadU32(pos + 8u),
                                              assembler, light);
    }

    static void Report()
    {
        if (!_probe) return;
        double now = Environment.TickCount64 / 1000.0;
        if (now < _probeAt) return;
        double span = _probeAt == 0.0 ? 2.0 : now - (_probeAt - 2.0);
        _probeAt = now + 2.0;
        // The live counts are this frame's; everything below them is a rate,
        // because a submit is counted where it happens and the walk only counts
        // where it looks. The ambient total is not reset -- one key-on a minute is
        // not a rate, and it is the number that explains a verify mismatch.
        Console.WriteLine($"[modelwalk] this frame {_liveCreatures} creature, {_liveObjects} object, " +
                          $"{_liveEffects} effect, {_liveSprites} sprite slot(s) live, " +
                          $"{_lastCount} model(s) submitted; a second: {_creatures / span:F0} creature, " +
                          $"{_objects / span:F0} object, {_effects / span:F0} effect, {_sprites / span:F0} sprite; " +
                          $"{_lit / span:F0} lit, {_flat / span:F0} flat, {_semi / span:F0} semi-transparent; " +
                          $"{_ambients} ambient sound(s) played in all");
        _creatures = _objects = _effects = _sprites = _lit = _flat = _semi = 0;
    }

    // ---- KF2_MODELWALK=verify -----------------------------------------------

    /// <summary>Below the entry SP: this call's frame and its callees'. The walk
    /// takes 0x300 of it and `func_80032588` another 0xD8, and the assemblers below
    /// that add their own, so the window is TileWalk's.</summary>
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

    static readonly Check _walkCheck = new("func_800331B4");
    static readonly Check _submitCheck = new("func_80032588");

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
        Console.WriteLine($"[modelwalk] verify {k.Name}: {k.Calls} call(s), {k.Bad} RAM mismatch(es), " +
                          $"{k.BadReg} register mismatch(es), {k.BadGte} GTE mismatch(es)");
        foreach (var s in k.Samples) Console.WriteLine($"[modelwalk]   {s}");
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
