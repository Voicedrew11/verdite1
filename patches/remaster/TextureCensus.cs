using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;
using RecompOne.Runtime.Assets;
using RecompOne.Runtime.Assets.Textures;
using RecompOne.Runtime.Events;

namespace Kf2.Remaster;

/// <summary>
/// Which texture keys an area draws, and which of them a pack covers: every lookup
/// the replacement resolver makes, bucketed by the settled area. Hashes and
/// rectangles only, never texels, so a report may sit beside a pack.
///
///     KF2_TEXCENSUS=1          on from boot, a line every 5 s
///     KF2_TEXKEY=triangle      key each triangle on its own UVs, as upstream does (the comparison)
///     textures [save|reset]    the shell verb; save writes dump/GAME/census/area-N.json
///
/// See "Phase 4, the first slice" in docs/REMASTER.md.
/// </summary>
public static class TextureCensus
{
    sealed class Seen
    {
        public int TPage, Clut, Bpp, U0, V0, W, H;
        public long Hits, Dirty;
        public bool Replaced;
    }

    sealed class AreaCensus
    {
        public readonly Dictionary<(ulong, ulong), Seen> Keys = [];
        // (tpage, clut, rect) -> every index hash seen there: more than one is art
        // that changes under its key (the scrolling textures, a cycled CLUT).
        public readonly Dictionary<long, HashSet<ulong>> Places = [];
        public readonly Dictionary<(int, int), int> Uploads = [];
        public long Lookups, Invalid, Dirty;
    }

    static readonly Dictionary<int, AreaCensus> _areas = [];
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _reportAt;
    static long _lookupsAt;
    static bool _probe;

    public static bool On { get; private set; }

    static string Keying => TextureResolver.KeyOnUpload ? "upload" : TextureResolver.KeyOnFaceRect ? "face" : "triangle";

    public static void Configure(string? on, string? key)
    {
        _probe = on?.Trim() is not (null or "" or "0");
        switch (key?.Trim().ToLowerInvariant())
        {
            case "face": TextureResolver.KeyOnUpload = false; break;
            case "triangle": TextureResolver.KeyOnUpload = false; TextureResolver.KeyOnFaceRect = false; break;
        }
    }

    public static void Install()
    {
        if (_probe) SetOn(true);
        Event.AddListener<VSyncEvent>(_ => Frame());
    }

    public static void SetOn(bool on)
    {
        if (on == On) return;
        On = on;
        TextureResolver.Observer = on ? Lookup : null;
        TextureKeys.Census = on ? new Dictionary<TexKey, long>() : null;
        VramTracker.Uploaded = on ? Upload : null;
        TextureResolver.Invalidate();
    }

    static AreaCensus Current()
    {
        int area = Identity.Settled ? Identity.Area : -1;
        if (!_areas.TryGetValue(area, out var c)) _areas[area] = c = new AreaCensus();
        return c;
    }

    static void Lookup(int tpage, int clut, in TileRect r, ulong index, ulong clutHash, bool valid, bool dirty, bool hit)
    {
        var c = Current();
        c.Lookups++;
        if (dirty) c.Dirty++;
        if (!valid) { c.Invalid++; return; }
        if (!c.Keys.TryGetValue((index, clutHash), out var s))
        {
            c.Keys[(index, clutHash)] = s = new Seen
            {
                TPage = tpage & 0x1FF, Clut = clut & 0x7FFF, Bpp = r.Bpp, U0 = r.U0, V0 = r.V0, W = r.W, H = r.H,
            };
        }
        s.Hits++;
        if (dirty) s.Dirty++;
        s.Replaced |= hit;
        long place = Place(tpage, clut, r.U0, r.V0, r.W, r.H);
        if (!c.Places.TryGetValue(place, out var set)) c.Places[place] = set = [];
        if (set.Count < 64) set.Add(index);
    }

    static long Place(int tpage, int clut, int u0, int v0, int w, int h)
        => (long)(tpage & 0x1FF) | (long)(clut & 0x7FFF) << 9 | (long)(u0 & 0xFF) << 24 | (long)(v0 & 0xFF) << 32
           | (long)(w & 0x1FF) << 40 | (long)(h & 0x1FF) << 49;

    static void Upload(int x, int y, int w, int h)
    {
        var c = Current();
        c.Uploads[(w, h)] = c.Uploads.GetValueOrDefault((w, h)) + 1;
    }

    static void Frame()
    {
        if (!On) return;
        if (!Host.Enabled && !Editor.Open && RecompOne.Runtime.Runtime.Mem is { } m) Identity.Poll(m);
        if (!_probe) return;
        double now = _clock.Elapsed.TotalSeconds;
        if (now < _reportAt) return;
        double dt = _reportAt == 0 ? 5.0 : now - _reportAt + 5.0;
        _reportAt = now + 5.0;
        var c = Current();
        long d = c.Lookups - _lookupsAt;
        _lookupsAt = c.Lookups;
        var s = Summary(c);
        Console.WriteLine($"[KF2] texcensus: area {(Identity.Settled ? Identity.Area : -1)} " +
                          $"({Keying} keys), {d / dt:F0} lookups/s; " +
                          $"{s["keys"]} keys, {s["art"]} art, {s["places"]} places ({s["dynamicPlaces"]} dynamic), " +
                          $"{s["overlapGroups"]} page+CLUT groups with overlapping rects ({s["overlapPairs"]} pairs, " +
                          $"{s["nestedRects"]} rects inside another), {s["replaced"]} replaced, {s["dirtyKeys"]} GPU-dirty; " +
                          $"uploads {s["uploadsText"]}; replacement filter set {RecompOne.Runtime.GteDepth.RepFilterSets}x");
    }

    static JsonObject Summary(AreaCensus c)
    {
        var art = new HashSet<ulong>();
        int replaced = 0, dirtyKeys = 0;
        var groups = new Dictionary<int, List<Seen>>();
        foreach (var ((index, _), s) in c.Keys)
        {
            art.Add(index);
            if (s.Replaced) replaced++;
            if (s.Dirty > 0) dirtyKeys++;
            int g = s.TPage | s.Clut << 9;
            if (!groups.TryGetValue(g, out var l)) groups[g] = l = [];
            l.Add(s);
        }
        // One piece of art under several keys shows as rectangles of the same page
        // and CLUT that overlap without being the same rectangle.
        int overlapGroups = 0, overlapPairs = 0, nested = 0;
        foreach (var l in groups.Values)
        {
            var rects = l.Select(s => (s.U0, s.V0, s.W, s.H)).Distinct().ToArray();
            int pairs = 0;
            var inside = new HashSet<int>();
            for (int i = 0; i < rects.Length; i++)
            for (int j = i + 1; j < rects.Length; j++)
            {
                var (a, b) = (rects[i], rects[j]);
                if (a.U0 >= b.U0 + b.W || b.U0 >= a.U0 + a.W || a.V0 >= b.V0 + b.H || b.V0 >= a.V0 + a.H) continue;
                pairs++;
                if (Contains(b, a)) inside.Add(i);
                else if (Contains(a, b)) inside.Add(j);
            }
            if (pairs > 0) overlapGroups++;
            overlapPairs += pairs;
            nested += inside.Count;
        }
        int dynamicPlaces = c.Places.Values.Count(v => v.Count > 1);
        var uploads = c.Uploads.OrderByDescending(p => p.Value).Take(6)
            .Select(p => $"{p.Key.Item1}x{p.Key.Item2}:{p.Value}");
        return new JsonObject
        {
            ["keys"] = c.Keys.Count, ["art"] = art.Count, ["places"] = c.Places.Count, ["dynamicPlaces"] = dynamicPlaces,
            ["overlapGroups"] = overlapGroups, ["overlapPairs"] = overlapPairs, ["nestedRects"] = nested,
            ["replaced"] = replaced, ["dirtyKeys"] = dirtyKeys, ["lookups"] = c.Lookups, ["invalid"] = c.Invalid,
            ["dirtyLookups"] = c.Dirty, ["uploadsText"] = string.Join(" ", uploads),
        };
    }

    static bool Contains((int U0, int V0, int W, int H) o, (int U0, int V0, int W, int H) i)
        => i.U0 >= o.U0 && i.V0 >= o.V0 && i.U0 + i.W <= o.U0 + o.W && i.V0 + i.H <= o.V0 + o.H;

    /// <summary>The <c>textures</c> shell verb.</summary>
    public static JsonObject Verb(string[] a)
    {
        string sub = a.Length > 0 ? a[0] : "";
        if (sub == "on") SetOn(true);
        else if (sub == "off") SetOn(false);
        else if (sub == "reset") { _areas.Clear(); _lookupsAt = 0; TextureResolver.Invalidate(); TextureKeys.Census?.Clear(); }
        int area = Identity.Settled ? Identity.Area : -1;
        var body = new JsonObject
        {
            ["on"] = On, ["area"] = area, ["keying"] = Keying,
        };
        if (TextureKeys.Census is { } cells)
        {
            body["cellKeys"] = cells.Count;
            body["noUpload"] = TextureKeys.NoUpload;
            body["fluidLookups"] = TextureKeys.FluidLookups;
        }
        if (_areas.TryGetValue(area, out var c))
        {
            foreach (var (k, v) in Summary(c)) body[k] = v?.DeepClone();
            if (sub == "save") body["file"] = Save(area, c);
        }
        return body;
    }

    static string Save(int area, AreaCensus c)
    {
        var game = AssetReplacerManager.Instance.GameId;
        var dir = Path.GetFullPath(Path.Combine("dump", game, "census"));
        Directory.CreateDirectory(dir);
        var keys = new JsonArray();
        foreach (var ((index, clut), s) in c.Keys.OrderByDescending(p => p.Value.Hits))
        {
            long place = Place(s.TPage, s.Clut, s.U0, s.V0, s.W, s.H);
            keys.Add(new JsonObject
            {
                // The name TextureDumper writes, and the name a pack's textures/ file takes.
                ["file"] = $"{index:x16}_{clut:x16}.png",
                ["index"] = index.ToString("x16"), ["clut"] = clut.ToString("x16"),
                ["tpage"] = s.TPage, ["clutWord"] = s.Clut, ["bpp"] = s.Bpp,
                ["rect"] = new JsonArray(s.U0, s.V0, s.W, s.H),
                ["hits"] = s.Hits, ["covered"] = s.Replaced,
                ["dynamic"] = c.Places.TryGetValue(place, out var set) && set.Count > 1,
                ["gpuDirty"] = s.Dirty > 0,
            });
        }
        var doc = new JsonObject
        {
            ["formatVersion"] = 1, ["area"] = area,
            ["fingerprint"] = area >= 0 && Identity.Settled ? Identity.FingerprintText : null,
            ["keying"] = Keying,
            ["summary"] = Summary(c), ["keys"] = keys,
            // The material keys (TextureKeys) the editor or a texture rule looked up since
            // the last reset: the art each face draws, cut to its cell.
            ["cells"] = new JsonArray((TextureKeys.Census ?? new()).OrderByDescending(p => p.Value)
                .Select(p => (JsonNode)new JsonObject { ["key"] = p.Key.ToString(), ["lookups"] = p.Value }).ToArray()),
        };
        var path = Path.Combine(dir, $"area-{area}.json");
        File.WriteAllText(path, doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        return path;
    }
}
