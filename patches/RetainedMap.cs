using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using KingsField2 = Recompiled.KingsField2_game;

namespace Kf2;

/// <summary>
/// The area's map as a world-space mesh on the GPU (<see cref="RetainedScene"/>), so a
/// reflection draws the world again without the game's walks running twice.
///
///     KF2_RETAINED=1          reflections from the retained scene (off: not judged); needs KF2_SSR
///     KF2_RETAINED_PLANAR=0   no planar reflections from it
///     KF2_RETAINED_CUBE=0     no camera cubemap
///     KF2_RETAINED_CUBESIZE=256  a cubemap face's size
///     KF2_RETAINED_PROBE=1    the mesh, the check against the game's own vertices, the planes, GPU time
///
/// **The map is data.** 80x80 tiles of two halves; a half names a mesh of the map's
/// model bank, a height, a quarter-turn and a light record (<see cref="TileWalk"/>).
/// The mesh's corners are placed as `func_80031950` places them -- the tile's centre,
/// the height times -128, the quarter-turn `func_80014B88` gives the view matrix --
/// and each face is lit once, flat, by the record's light matrix, colour matrix and
/// back colour through the GTE, exactly as `NormalColorCol` lights it in the
/// assembler: none of that depends on the camera. The depth cue does, so each corner
/// carries the record's DQA, DQB and curve and the vertex shader fogs it.
///
/// Rebuilt when the map block, the light records, the model bank or the remaster's
/// materials change: a hash of all four once a walk (about 70 KB, a few microseconds).
///
/// See "The retained scene" in docs/RENDERING.md.
/// </summary>
public static class RetainedMap
{
    const uint MapBase = 0x801C8484;
    const uint MapBytes = 80u * 80u * 10u;
    const uint LightBase = 0x801930F0;
    const uint LightBytes = 64u * 104u;
    const uint Banks = 0x8018E18C;
    const uint LightColour = 0x8006E604;
    const uint VertexCache = 0x8018EB94;
    const uint ViewMatrix = CameraBlock.ViewMatrix;

    static bool? _forced;
    static bool _probe;

    public const string OnKey = "kf2.ssr.retained";

    public static bool Enabled => RetainedScene.Enabled;

    public static void Configure(string? on, string? planar, string? cube, string? cubeSize, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on != "0";
        RetainedScene.Planar = planar?.Trim() != "0";
        RetainedScene.Cube = cube?.Trim() != "0";
        if (int.TryParse(cubeSize, out int s) && s >= 16) RetainedScene.CubeSize = Math.Clamp(s, 16, 2048);
        _probe = !string.IsNullOrWhiteSpace(probe) && probe != "0";
        RetainedScene.Probe = _probe;
    }

    public static void Install()
    {
        RetainedScene.Enabled = _forced ?? false;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            RetainedScene.Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, false);
            Console.WriteLine($"[KF2] retained scene: {(Enabled ? "on" : "off")}" +
                              (Enabled ? $", planar {(RetainedScene.Planar ? "on" : "off")}, " +
                                         $"cubemap {(RetainedScene.Cube ? $"{RetainedScene.CubeSize}px" : "off")}" : ""));
        });
        Event.AddListener<OverlayLoadedEvent>(_ => _hash = 0);
    }

    public static void SetEnabled(bool on)
    {
        RetainedScene.Enabled = on;
        _hash = 0;
    }

    /// <summary>Whether reflections are drawn from it: the setting, and the reflection
    /// pass that reads what is drawn. The backend draws only once its program built
    /// (<see cref="RetainedScene.Supported"/>).</summary>
    public static bool ReflectionsReady => RetainedScene.Enabled && ScreenReflections.Enabled;

    /// <summary>Whether to build the static map: for reflections, or for the authored
    /// lights' shadows (0077), which need nothing else of it.</summary>
    public static bool Ready => ReflectionsReady || RetainedScene.ShadowsWanted;

    // ---- once a walk -------------------------------------------------------------

    static ulong _hash;
    static uint _table;

    /// <summary>From the start of <see cref="TileWalk"/>'s sweep, on the frame's own
    /// walk (not a mirrored one): rebuild if the map changed, and start the frame
    /// with the camera the walk is about to draw with.</summary>
    public static void AtWalk(CpuContext c, PSMemory mem)
    {
        if (!Ready) return;
        ulong h = Hash(mem);
        if (h != _hash)
        {
            _hash = h;
            var start = System.Diagnostics.Stopwatch.GetTimestamp();
            Build(c, mem);
            _buildMs = System.Diagnostics.Stopwatch.GetElapsedTime(start).TotalMilliseconds;
            _builds++;
            _lastWhy = _why;
        }
        if (!ReflectionsReady) return;
        RetainedScene.BeginFrame(ReadView(mem));
        RetainedPlanes.Choose(mem);
        if (_probe) Report();
    }

    static ulong Hash(PSMemory mem)
    {
        var ram = mem.Ram;
        uint mask = (uint)ram.Length - 1u;
        ulong hm = MeshBytes(ram.Slice((int)(MapBase & mask), (int)MapBytes));
        ulong hl = Mix(0xCBF29CE484222325ul, ram.Slice((int)(LightBase & mask), (int)LightBytes));
        Span<uint> extra = [mem.ReadU32(Banks), (uint)Remaster.Surfaces.Serial, (uint)SurfaceMaterial.RectN, 0u];
        ulong he = Mix(0xCBF29CE484222325ul, MemoryMarshal.AsBytes(extra));
        _why = (hm != _hm ? "map " : "") + (hl != _hl ? "lights " : "") + (he != _he ? $"bank/materials/rects({extra[0]:X},{extra[1]},{extra[2]}) " : "");
        _hm = hm; _hl = hl; _he = he;
        return (hm * 31 + hl) * 31 + he;
    }

    /// <summary>Only what a half's mesh is built from -- its model, height, the low
    /// two bits of the turn and the low six of the light record -- since the game
    /// writes other bits of the map as it runs.</summary>
    static ulong MeshBytes(ReadOnlySpan<byte> map)
    {
        ulong h = 0xCBF29CE484222325ul;
        for (int i = 0; i + 10 <= map.Length; i += 10)
        {
            ulong w = map[i] | (ulong)map[i + 1] << 8 | (ulong)(map[i + 2] & 3) << 16 | (ulong)(map[i + 4] & 0x3F) << 24
                    | (ulong)map[i + 5] << 32 | (ulong)map[i + 6] << 40 | (ulong)(map[i + 7] & 3) << 48 | (ulong)(map[i + 9] & 0x3F) << 56;
            h = (h ^ w) * 0x100000001B3ul;
        }
        return h;
    }

    static ulong _hm, _hl, _he;
    static string _why = "";
    static string _lastWhy = "";

    static ulong Mix(ulong h, ReadOnlySpan<byte> bytes)
    {
        foreach (ulong w in MemoryMarshal.Cast<byte, ulong>(bytes)) h = (h ^ w) * 0x100000001B3ul;
        return h;
    }

    /// <summary>The camera block as the frame is drawn from: the GTE rotation and
    /// translation the view matrix holds, and the camera's world position.</summary>
    public static RetainedScene.View ReadView(IMemory mem)
    {
        var cam = Camera.Read(mem);
        Span<float> r = stackalloc float[9];
        for (int i = 0; i < 9; i++) r[i] = (short)mem.ReadU16(ViewMatrix + (uint)i * 2u) / 4096f;
        return new RetainedScene.View
        {
            R00 = r[0], R01 = r[1], R02 = r[2],
            R10 = r[3], R11 = r[4], R12 = r[5],
            R20 = r[6], R21 = r[7], R22 = r[8],
            CamX = cam.X, CamY = cam.Y, CamZ = cam.Z,
            Tx = (int)mem.ReadU32(ViewMatrix + 0x14u),
            Ty = (int)mem.ReadU32(ViewMatrix + 0x18u),
            Tz = (int)mem.ReadU32(ViewMatrix + 0x1Cu),
            H = GteDepth.ProjH, Cx = GteDepth.ProjCx, Cy = GteDepth.ProjCy,
        };
    }

    // ---- the mesh ------------------------------------------------------------------

    static RetainedScene.Vertex[] _tris = new RetainedScene.Vertex[1 << 16];
    static int _n;
    static readonly Gte.State _gteSaved = new();
    static readonly Dictionary<int, (float Dqa, float Dqb)> _cue = new();

    // What the last build made, for the probe.
    static int _halves, _faces, _skippedModels;
    static double _buildMs;
    static long _builds;

    static void Build(CpuContext c, PSMemory mem)
    {
        _n = 0;
        _halves = _faces = _skippedModels = 0;
        _cue.Clear();
        RetainedPlanes.Clear();
        _table = mem.ReadU32(Banks);
        if (_table == 0) { RetainedScene.SetStatic([]); return; }
        uint rgbc = mem.ReadU32(LightColour);

        var snap = c.Snapshot();
        Gte.Save(_gteSaved);
        try
        {
            for (int tz = 0; tz < 80; tz++)
            for (int tx = 0; tx < 80; tx++)
            for (int half = 0; half < 2; half++)
            {
                uint rec = MapBase + (uint)(tz * 800 + tx * 10 + half * 5);
                uint model = mem.ReadU8(rec);
                if (model >= 240) continue;
                // The table's +4 is the far-model gate's limit (TileWalk.Beyond), not a
                // count: models past it are drawn unless that gate is set.
                if (!Sane(mem, model)) { _skippedModels++; continue; }
                Half(c, mem, rec, model, tx, tz, half, rgbc);
            }
        }
        finally
        {
            Gte.Load(_gteSaved);
            c.Restore(snap);
        }
        RetainedScene.SetStatic(_tris.AsSpan(0, _n));
        RetainedPlanes.Finish();
    }

    /// <summary>A mesh whose header points inside RAM and has a believable count.</summary>
    static bool Sane(PSMemory mem, uint model)
    {
        uint header = _table + model * 28u + 0xCu;
        uint ram = 0x80000000u + (uint)mem.Ram.Length;
        uint verts = _table + mem.ReadU32(header) + 0xCu, faces = _table + mem.ReadU32(header + 0x10u) + 0xCu;
        return verts >= 0x80000000u && verts < ram && faces >= 0x80000000u && faces < ram
            && mem.ReadU32(header + 4u) <= 4096u && mem.ReadU32(header + 0x14u) <= 4096u;
    }

    static void Half(CpuContext c, PSMemory mem, uint rec, uint model, int tx, int tz, int half, uint rgbc)
    {
        _halves++;
        uint rot = mem.ReadU8(rec + 2u) & 3u;
        uint light = LightBase + (mem.ReadU8(rec + 4u) & 0x3Fu) * 104u;
        for (int i = 0; i < 5; i++) Gte.WriteControl(8 + i, mem.ReadU32(light + rot * 20u + (uint)i * 4u));
        for (int i = 0; i < 5; i++) Gte.WriteControl(16 + i, mem.ReadU32(light + 0x50u + (uint)i * 4u));
        Gte.WriteControl(13, (uint)mem.ReadU8(light + 0x62u) << 4);
        Gte.WriteControl(14, (uint)mem.ReadU8(light + 0x63u) << 4);
        Gte.WriteControl(15, (uint)mem.ReadU8(light + 0x64u) << 4);
        int fog = (short)mem.ReadU16(light + 0x66u);
        var (dqa, dqb) = Cue(c, mem, fog);
        float curve = fog >= 32000 ? 0f : fog < 0 ? 1f : 2f;

        double wx = tx * 2048 + 1024, wz = tz * 2048 + 1024;
        double wy = -(int)mem.ReadU8(rec + 1u) * 128;

        uint header = _table + model * 28u + 0xCu;
        uint verts = _table + mem.ReadU32(header) + 0xCu;
        uint normals = _table + mem.ReadU32(header + 8u) + 0xCu;
        uint face = _table + mem.ReadU32(header + 0x10u) + 0xCu;
        uint count = mem.ReadU32(header + 0x14u);
        if (count > 4096) return;

        Span<float> px = stackalloc float[4], py = stackalloc float[4], pz = stackalloc float[4];
        Span<uint> uv = stackalloc uint[4];
        for (int f = 0; f < (int)count; f++)
        {
            uint word = mem.ReadU32(face);
            uint at = face + 4u;
            uint type = (word >> 24) & 0xFDu;
            bool semi = ((word >> 24) & 2u) != 0;
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            if (corners != 0)
            {
                uint idx = at + (corners == 4 ? 0x12u : 0x0Eu);
                uint normalOff = mem.ReadU16(at + (corners == 4 ? 0x10u : 0x0Cu));
                for (int k = 0; k < corners; k++)
                {
                    uint v = verts + mem.ReadU16(idx + (uint)k * 2u);
                    Rotate(rot, (short)mem.ReadU16(v), (short)mem.ReadU16(v + 2u), (short)mem.ReadU16(v + 4u),
                           out float ox, out float oy, out float oz);
                    px[k] = (float)(wx + ox); py[k] = (float)(wy + oy); pz[k] = (float)(wz + oz);
                }
                uv[0] = mem.ReadU16(at);
                uv[1] = mem.ReadU16(at + 4u);
                uv[2] = mem.ReadU16(at + 8u);
                uv[3] = corners == 4 ? mem.ReadU16(at + 0xCu) : 0u;
                uint clut = mem.ReadU16(at + 2u), tpage = mem.ReadU16(at + 6u) & 0x1FFu;

                uint normal = normals + normalOff;
                Gte.Write(0, mem.ReadU32(normal));
                Gte.Write(1, mem.ReadU32(normal + 4u));
                Gte.Write(6, rgbc);
                Gte.NccsOp(12, true);
                uint lit = Gte.Read(22);

                int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
                for (int k = 0; k < corners; k++)
                {
                    int u = (int)(uv[k] & 0xFF), vv = (int)(uv[k] >> 8);
                    u0 = Math.Min(u0, u); v0 = Math.Min(v0, vv); u1 = Math.Max(u1, u); v1 = Math.Max(v1, vv);
                }
                uint rect = (uint)u0 | (uint)v0 << 8 | (uint)u1 << 16 | (uint)v1 << 24;
                byte mat = Remaster.Surfaces.FaceOf(tx, tz, half, (int)model, f);
                if (mat == 0 && Remaster.Surfaces.ByTexture
                    && Remaster.TextureKeys.Of((int)tpage, (int)clut, u0, v0, u1, v1, out var tex))
                    mat = Remaster.Surfaces.TextureId(tex);
                uint flags = RetainedScene.FlagRect | mat
                           | (semi ? RetainedScene.FlagSemi | ((tpage >> 5) & 3u) << 8 : 0u);

                var t = new RetainedScene.Vertex
                {
                    R = lit & 0xFF, G = (lit >> 8) & 0xFF, B = (lit >> 16) & 0xFF,
                    Clut = clut & 0x7FFF, Texpage = tpage,
                    Dqa = dqa, Dqb = dqb, Curve = curve,
                    Rect = rect, Flags = flags,
                };
                // A quad is the strip 0,1,2 then 1,2,3, as the GPU draws it.
                Emit(t, px, py, pz, uv, 0, 1, 2);
                if (corners == 4) Emit(t, px, py, pz, uv, 1, 3, 2);
                RetainedPlanes.Note(px, py, pz, corners, semi, mat, tpage, rect);
                _faces++;
            }
            face = at + ((word >> 6) & 0x3FCu);
        }
    }

    static void Emit(in RetainedScene.Vertex t, Span<float> px, Span<float> py, Span<float> pz, Span<uint> uv,
                     int a, int b, int c)
    {
        if (_n + 3 > _tris.Length) Array.Resize(ref _tris, _tris.Length * 2);
        Put(t, px, py, pz, uv, a);
        Put(t, px, py, pz, uv, b);
        Put(t, px, py, pz, uv, c);
    }

    static void Put(in RetainedScene.Vertex t, Span<float> px, Span<float> py, Span<float> pz, Span<uint> uv, int k)
    {
        var v = t;
        v.X = px[k]; v.Y = py[k]; v.Z = pz[k];
        v.U = uv[k] & 0xFF; v.V = uv[k] >> 8;
        _tris[_n++] = v;
    }

    /// <summary>A mesh corner turned as `func_80014B88` turns the view matrix for the
    /// half's quarter-turn: its columns, so the corner by the inverse.</summary>
    static void Rotate(uint rot, int x, int y, int z, out float ox, out float oy, out float oz)
    {
        switch (rot)
        {
            case 1: ox = z; oy = y; oz = -x; break;
            case 2: ox = -x; oy = y; oz = -z; break;
            case 3: ox = -z; oy = y; oz = x; break;
            default: ox = x; oy = y; oz = z; break;
        }
    }

    /// <summary>A light record's fog word as the GTE's DQA and DQB, from the game's
    /// own `SetFogNear` (`func_8002DDDC` hands it half the word's low fifteen bits
    /// and an H of 200).</summary>
    static (float, float) Cue(CpuContext c, PSMemory mem, int fog)
    {
        if (_cue.TryGetValue(fog, out var hit)) return hit;
        c.A0 = (uint)((fog & 0x7FFF) >> 1);
        c.A1 = 200u;
        c.RA = 0x8002DDFCu;
        KingsField2.SetFogNear(c, mem);
        var cue = ((float)(short)Gte.ReadControl(27), (float)(int)Gte.ReadControl(28));
        _cue[fog] = cue;
        return cue;
    }

    // ---- the check against the game's own vertices ----------------------------------

    public static bool Checking => _probe && Ready;

    static long _checked, _within, _worst;
    static double _sumErr;

    /// <summary>After the assembler has drawn a half, compare the vertex cache it
    /// left -- the GTE's own screen words for every corner of the mesh -- with the
    /// retained corners projected through the frame's camera. A subdivided mesh's
    /// cache holds the subdivider's corners, so it is left out.</summary>
    public static void CheckHalf(PSMemory mem, uint rec, uint model)
    {
        uint off = rec - MapBase;
        int tile = (int)(off / 10u), half = off % 10u >= 5u ? 1 : 0;
        int tx = tile % 80, tz = tile / 80;
        uint rot = mem.ReadU8(rec + 2u) & 3u;
        var v = ReadView(mem);
        double wx = tx * 2048 + 1024, wz = tz * 2048 + 1024, wy = -(int)mem.ReadU8(rec + 1u) * 128;
        uint header = _table + model * 28u + 0xCu;
        uint verts = _table + mem.ReadU32(header) + 0xCu;
        uint n = mem.ReadU32(header + 4u);
        if (n > 512) return;
        for (uint i = 0; i < n; i++)
        {
            uint p = verts + i * 8u;
            Rotate(rot, (short)mem.ReadU16(p), (short)mem.ReadU16(p + 2u), (short)mem.ReadU16(p + 4u),
                   out float ox, out float oy, out float oz);
            double dx = wx + ox - v.CamX, dy = wy + oy - v.CamY, dz = wz + oz - v.CamZ;
            double vx = v.R00 * dx + v.R01 * dy + v.R02 * dz + v.Tx;
            double vy = v.R10 * dx + v.R11 * dy + v.R12 * dz + v.Ty;
            double vz = v.R20 * dx + v.R21 * dy + v.R22 * dz + v.Tz;
            if (vz < 64.0) continue;
            double sx = v.Cx + v.H * vx / vz, sy = v.Cy + v.H * vy / vz;
            if (Math.Abs(sx) > 1000 || Math.Abs(sy) > 1000) continue;
            uint word = mem.ReadU32(VertexCache + i * 8u);
            // The GTE keeps the whole part of its 16.16 result, so compare with that.
            double ex = Math.Abs((short)(word & 0xFFFF) - Math.Floor(sx)), ey = Math.Abs((short)(word >> 16) - Math.Floor(sy));
            double e = Math.Max(ex, ey);
            _checked++;
            _sumErr += e;
            if (e <= 1.0) _within++;
            if ((long)e > _worst) _worst = (long)e;
        }
    }

    static long _mChecked, _mWithin, _mWorst;

    /// <summary>A model corner taken back to world space, projected through the
    /// frame's camera, against the screen word the transform left for it.</summary>
    public static void CheckCorner(in RetainedScene.View v, double x, double y, double z, uint word)
    {
        double dx = x - v.CamX, dy = y - v.CamY, dz = z - v.CamZ;
        double vx = v.R00 * dx + v.R01 * dy + v.R02 * dz + v.Tx;
        double vy = v.R10 * dx + v.R11 * dy + v.R12 * dz + v.Ty;
        double vz = v.R20 * dx + v.R21 * dy + v.R22 * dz + v.Tz;
        if (vz < 64.0) return;
        double sx = v.Cx + v.H * vx / vz, sy = v.Cy + v.H * vy / vz;
        if (Math.Abs(sx) > 1000 || Math.Abs(sy) > 1000) return;
        double e = Math.Max(Math.Abs((short)(word & 0xFFFF) - Math.Floor(sx)), Math.Abs((short)(word >> 16) - Math.Floor(sy)));
        _mChecked++;
        if (e <= 1.0) _mWithin++;
        if ((long)e > _mWorst) _mWorst = (long)e;
    }

    // ---- the probe ---------------------------------------------------------------

    static double _reportedAt;

    static void Report()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (now < _reportedAt) return;
        _reportedAt = now + 2.0;
        int tris = RetainedScene.Static.Length / 3;
        Console.WriteLine($"[KF2] retained: {tris} static triangle(s) from {_faces} face(s) of {_halves} half/halves " +
                          $"({_skippedModels} refused as not a mesh), {_builds} build(s), last {_buildMs:F2} ms for {_lastWhy}; " +
                          $"ranges opaque {RetainedScene.StaticCount[0] / 3}, semi {RetainedScene.StaticCount[1] / 3}/" +
                          $"{RetainedScene.StaticCount[2] / 3}/{RetainedScene.StaticCount[3] / 3}/{RetainedScene.StaticCount[4] / 3}");
        Console.WriteLine($"[KF2] retained: check {_checked} corner(s), {(_checked == 0 ? 0 : 100.0 * _within / _checked):F2}% within 1 px, " +
                          $"mean {(_checked == 0 ? 0 : _sumErr / _checked):F3} px, worst {_worst} px; models {_mChecked} corner(s), " +
                          $"{(_mChecked == 0 ? 0 : 100.0 * _mWithin / _mChecked):F2}% within 1 px, worst {_mWorst} px");
        _mChecked = _mWithin = _mWorst = 0;
        Console.WriteLine($"[KF2] retained: {RetainedPlanes.Describe()}");
        // The reflection pass's readback (KF2_SSR_PROBE=1): what each reflective pixel
        // took, and where the planes and the cubemap both answered, how far apart.
        Console.WriteLine($"[KF2] retained: last readback {PlanarReflections.PlanarPct:F1}% of reflective pixels planar, " +
                          $"{ScreenReflections.HitPct:F1}% from the cubemap; {PlanarReflections.ComparedPct:F1}% of planar pixels " +
                          $"found by the cubemap too, brightness {PlanarReflections.MirrorDiff:F1} apart (read mirrored {PlanarReflections.ControlDiff:F1})");
        Console.WriteLine($"[KF2] retained: {RetainedScene.Found} frame(s) found at present, {RetainedScene.Missed} missed; " +
                          $"{RetainedScene.PlanarDraws} planar draw(s) at {RetainedScene.PlanarGpuNs / 1e6:F3} ms GPU, " +
                          $"{RetainedScene.CubeDraws} cubemap(s) at {RetainedScene.CubeGpuNs / 1e6:F3} ms GPU, " +
                          $"{RetainedScene.Triangles} triangle(s) submitted, chunks {RetainedScene.ChunksDrawn}/{RetainedScene.ChunksTested} drawn; " +
                          $"{RetainedModels.Models} model(s) and {RetainedModels.Faces} face(s) captured");
        RetainedModels.Models = RetainedModels.Faces = 0;
        _checked = _within = _worst = 0;
        _sumErr = 0;
        RetainedScene.ResetCounters();
    }
}
