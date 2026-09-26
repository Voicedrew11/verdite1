using System.Numerics;
using System.Reflection;
using System.Text.Json.Nodes;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2.Remaster;

/// <summary>
/// Overrides of the area's own light records (Phase 5, the first slice).
///
///     KF2_REMASTER_ATMOS=0   leave the pack's record overrides out (they apply with the remaster by default)
///
/// Stage 1 copies 80 records a frame from <c>0x800679A0</c> (0x2C bytes) into
/// <c>0x801930F0</c> (0x68); a post on it writes the pack's
/// <c>areas/&lt;n&gt;/atmosphere.json</c> over the copy, so stage 10 turns the light
/// matrix into its three quarter-turns and every renderer reads the result through
/// the game's own code. Nothing else reads the records (<see cref="LightCensus"/>).
/// An override is taken only while the record's source bytes hash as they did when
/// it was authored. See "Phase 5, the first slice" in docs/REMASTER.md.
/// </summary>
public sealed class Atmosphere : IRemasterFeature
{
    public string Id => "atmosphere";

    public const uint Src = 0x800679A0, Dst = 0x801930F0;
    public const uint SrcStride = 0x2C, DstStride = 0x68;
    public const int Records = 80;

    // Where each part sits in the source record; the copy puts +0x14.. at +0x50.. of the destination.
    const uint SrcLight = 0x00, SrcColour = 0x14, SrcBack = 0x26, SrcFog = 0x2A;
    const uint DstLight = 0x00, DstColour = 0x50, DstBack = 0x62, DstFog = 0x66;

    const uint Stage1 = 0x8002C944;

    static bool _on = true;
    static Pack.RecordOverride[] _active = [];
    int _version = -1, _settle = -1;

    /// <summary>Why the area's overrides are not applied, or null.</summary>
    public static string? Refused { get; private set; }

    /// <summary>The last frame's overrides written, and refused because the record moved.</summary>
    public static int Applied { get; private set; }
    public static int Stale { get; private set; }

    /// <summary>Stage 1 passes an override was written on, and passes at all; never reset.</summary>
    public static long Passes, Calls;

    public static bool Active => _active.Length > 0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.remaster.atmosphere",
        Name = "Remaster atmosphere",
        Version = "1.0",
        Description = "Writes a pack's light-record overrides after stage 1's copy.",
    };

    public static void Configure(string? on)
    {
        if (!string.IsNullOrWhiteSpace(on)) _on = on.Trim() != "0";
    }

    public static void Install() => HookAttach.OnOverlayLoad("remaster atmosphere", Attach);

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, Stage1);
        if (target == null) return false;
        if (!_queued)
        {
            var post = typeof(Atmosphere).GetMethod(nameof(AfterCopy), BindingFlags.Public | BindingFlags.Static)!;
            _queued = HookManager.AddPost(_self, target, post);
        }
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static bool _queued;

    public void OnFrame()
    {
        bool want = _on && Host.Enabled && Identity.Settled;
        if (!want)
        {
            if (_active.Length > 0 || Refused != null) Clear();
            _version = _settle = -1;
            return;
        }
        if (_version == Pack.Version && _settle == Identity.Settles) return;
        _version = Pack.Version;
        _settle = Identity.Settles;
        int area = Identity.Area;
        string? fp = Pack.AreaFingerprint(area);
        if (fp != null && fp != Identity.FingerprintText)
        {
            Clear();
            Refused = $"area {area} was authored against fingerprint {fp}; this one is {Identity.FingerprintText}";
            return;
        }
        Refused = null;
        _active = Pack.Records(area).Where(r => (uint)r.Record < Records).ToArray();
    }

    public void Detach()
    {
        Clear();
        _version = _settle = -1;
    }

    /// <summary>The next stage 1 copies the game's records back, so there is nothing to undo.</summary>
    static void Clear()
    {
        _active = [];
        Refused = null;
        Applied = Stale = 0;
    }

    public string Probe()
        => Refused != null ? $"atmosphere refused ({Refused})"
         : $"{Applied} of {_active.Length} record override(s) written{(Stale > 0 ? $", {Stale} refused (record changed)" : "")} ({Calls} stage 1 passes)";

    /// <summary>Post on stage 1: the copy is done; write the overrides over it.</summary>
    public static void AfterCopy(CpuContext c, IMemory m)
    {
        Calls++;
        var list = _active;
        if (list.Length == 0) return;
        int applied = 0, stale = 0;
        foreach (var r in list)
        {
            if (r.Hash != null && r.Hash != SourceHash(m, r.Record)) { stale++; continue; }
            Write(m, r);
            applied++;
        }
        Applied = applied;
        Stale = stale;
        if (applied > 0) Passes++;
    }

    static void Write(IMemory m, Pack.RecordOverride r)
    {
        uint d = Dst + (uint)r.Record * DstStride;
        for (int j = 0; j < 3; j++)
        {
            if (r.Direction[j] is { } v)
            {
                m.WriteU16(d + DstLight + (uint)(6 * j), Fixed(v.X));
                m.WriteU16(d + DstLight + (uint)(6 * j + 2), Fixed(v.Y));
                m.WriteU16(d + DstLight + (uint)(6 * j + 4), Fixed(v.Z));
            }
            // The colour matrix's rows are R, G, B; light j is its column j.
            if (r.Colour[j] is { } col)
            {
                m.WriteU16(d + DstColour + (uint)(2 * j), Fixed(col.X));
                m.WriteU16(d + DstColour + (uint)(6 + 2 * j), Fixed(col.Y));
                m.WriteU16(d + DstColour + (uint)(12 + 2 * j), Fixed(col.Z));
            }
        }
        if (r.Back is { } b)
            for (uint i = 0; i < 3; i++) m.WriteU8(d + DstBack + i, (byte)b[i]);
        if (r.Fog is int fog) m.WriteU16(d + DstFog, (ushort)fog);
    }

    static ushort Fixed(float v) => (ushort)(short)Math.Clamp((int)MathF.Round(v * 4096f), short.MinValue, short.MaxValue);

    // ---- the game's own records, read from the source ------------------------

    /// <summary>FNV-1a 64 over a record's 0x2C source bytes: what an override is keyed on.</summary>
    public static string SourceHash(IMemory m, int record)
    {
        ulong h = 0xCBF29CE484222325UL;
        uint s = Src + (uint)record * SrcStride;
        for (uint i = 0; i < SrcStride; i++)
        {
            h ^= m.ReadU8(s + i);
            h *= 0x100000001B3UL;
        }
        return h.ToString("x16");
    }

    /// <summary>A record as the game holds it: light j's direction and colour, the back
    /// colour and the fog word.</summary>
    public readonly record struct Record(Vector3[] Direction, Vector3[] Colour, int[] Back, int Fog);

    public static Record Game(IMemory m, int record)
    {
        uint s = Src + (uint)record * SrcStride;
        float F(uint a) => (short)m.ReadU16(a) / 4096f;
        var dir = new Vector3[3];
        var col = new Vector3[3];
        for (uint j = 0; j < 3; j++)
        {
            dir[j] = new Vector3(F(s + SrcLight + 6 * j), F(s + SrcLight + 6 * j + 2), F(s + SrcLight + 6 * j + 4));
            col[j] = new Vector3(F(s + SrcColour + 2 * j), F(s + SrcColour + 6 + 2 * j), F(s + SrcColour + 12 + 2 * j));
        }
        int[] back = [m.ReadU8(s + SrcBack), m.ReadU8(s + SrcBack + 1), m.ReadU8(s + SrcBack + 2)];
        return new Record(dir, col, back, m.ReadU16(s + SrcFog));
    }

    /// <summary>What the record is drawn with once the override is over it.</summary>
    public static Record Effective(IMemory m, int record, Pack.RecordOverride? o)
    {
        var g = Game(m, record);
        if (o is not { } r) return g;
        var dir = (Vector3[])g.Direction.Clone();
        var col = (Vector3[])g.Colour.Clone();
        for (int j = 0; j < 3; j++)
        {
            if (r.Direction[j] is { } d) dir[j] = d;
            if (r.Colour[j] is { } c) col[j] = c;
        }
        return new Record(dir, col, r.Back ?? g.Back, r.Fog ?? g.Fog);
    }

    /// <summary>Drawn tile halves per record in the live tile block, by the half's
    /// <c>+4 &amp; 0x3F</c>.</summary>
    public static int[] Usage(IMemory m)
    {
        var n = new int[Records];
        for (int z = 0; z < Identity.Span; z++)
        for (int x = 0; x < Identity.Span; x++)
        for (int h = 0; h < 2; h++)
        {
            uint rec = Identity.HalfRecord(x, z, h);
            if (m.ReadU8(rec) >= 240) continue;
            n[m.ReadU8(rec + 4) & 0x3F]++;
        }
        return n;
    }

    /// <summary>The record the half under the player takes, or -1.</summary>
    public static int UnderPlayer(IMemory m)
        => Identity.PlayerTile(m) is { } k ? m.ReadU8(Identity.HalfRecord(k.X, k.Z, k.Half) + 4) & 0x3F : -1;

    /// <summary>Fog starts at half the word's low fifteen bits, in view units.</summary>
    public static string DescribeFog(int word)
        => (word & 0x7FFF) >= 32000 && (word & 0x8000) == 0 ? "none"
         : $"from {(word & 0x7FFF) >> 1}{((word & 0x8000) != 0 ? ", linear" : "")}";

    public static JsonObject Json(Record r)
    {
        var lights = new JsonArray();
        for (int j = 0; j < 3; j++)
            lights.Add(new JsonObject
            {
                ["direction"] = new JsonArray(Math.Round(r.Direction[j].X, 4), Math.Round(r.Direction[j].Y, 4), Math.Round(r.Direction[j].Z, 4)),
                ["colour"] = new JsonArray(Math.Round(r.Colour[j].X, 4), Math.Round(r.Colour[j].Y, 4), Math.Round(r.Colour[j].Z, 4)),
            });
        return new JsonObject
        {
            ["back"] = new JsonArray(r.Back[0], r.Back[1], r.Back[2]),
            ["lights"] = lights,
            ["fog"] = r.Fog,
            ["fogIs"] = DescribeFog(r.Fog),
        };
    }
}
