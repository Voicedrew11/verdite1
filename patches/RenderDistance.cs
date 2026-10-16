using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// The retained map and models drawn past the game's reach, and the edge of what is
/// drawn faded in by distance rather than popping. Verdite3's RenderDistance on this
/// game's walk. The main view's only: the guest's stencils are never changed, and the
/// packet path keeps the game's reach.
///
/// <para>**The game's reach**: the map walk draws the cells of one of sixteen 14x14
/// stencils around the eye's cell (CullCone), and the model walk the models standing
/// in them. <see cref="GameEdge"/> is the nearest an undrawn cell inside the 4:3 view's
/// cone comes to the eye, over every stencil, heading in its sector and place in the
/// eye's cell (7.64 cells, measured): a fade that ends there never shows a cell the game did not
/// draw at 4:3. A wider cone sees cells beside the eye no 14x14 stencil holds (1.6
/// cells away at 16:9); those are the render distance's to fill, not the fade's.</para>
///
/// <para>The game's fog is black by 9.17 cells in every area (docs/GPU_RENDERER.md), so
/// past that the render distance adds only black.</para>
///
/// <para>**Render distance** (<see cref="Cells"/>): after the map walk, every cell whose
/// nearest point lies within the distance and whose box meets the view's horizontal
/// cone gets its slot in the frame's main-view gate (runtime 0089,
/// <c>RetainedScene.CurrentMainHalves</c>), as the game's stencil byte 1 draws it
/// (object + 100). The model walk's stencil tests ask <see cref="Reach"/> past the
/// stencil, so the models standing there are drawn too, retained.</para>
///
/// <para>**The fade** (<see cref="FadeCells"/>): runtime 0089's DistanceFade, weight 0
/// at the edge (the render distance, or <see cref="GameEdge"/> with it off) and 1 a
/// band nearer, by horizontal distance, on the map and the models alike.</para>
///
///     KF1_RENDERDIST=&lt;cells&gt;        draw out to this many cells (0/unset: the game's)
///     KF1_RENDERDIST_FADE=&lt;cells&gt;   fade over this band at the edge (0/unset: none)
///     KF1_RENDERDIST_MODELS=0         the models kept at the game's reach
///     KF1_RENDERDIST_PROBE=1          a line every five seconds
///
/// Both act only while the retained renderer draws (KF1_GPU_WORLD=1). Step 4 of
/// docs/RENDERER_PLAN.md; see "Render distance" in docs/GPU_RENDERER.md.
/// </summary>
public static class RenderDistance
{
    const uint MapWalkAt = 0x8001E83C;
    const uint Stencils = 0x80065BE8, EyeCol = 0x8009575C, EyeRow = 0x8009575E;
    const int Stride = 0xCC, Count = 16, Grid = 100, Box = 14;
    const float Cell = 2000f;
    public const int MaxCells = 30, MaxFade = 8;

    /// <summary>The distance drawn to, in cells; 0 is the game's own.</summary>
    public static float Cells { get; private set; }
    /// <summary>The band faded over at the edge, in cells; 0 is no fade.</summary>
    public static float FadeCells { get; private set; }
    public static bool Enabled => Cells > 0 || FadeCells > 0;

    public static void SetCells(float cells) => Cells = float.IsFinite(cells) && cells > 0 ? Math.Clamp(cells, 1, MaxCells) : 0;
    public static void SetFade(float cells) => FadeCells = float.IsFinite(cells) && cells > 0 ? Math.Clamp(cells, 0.25f, MaxFade) : 0;

    public static bool ModelsOn = true;
    static bool _probe;

    static readonly ModInfo _self = new() { Id = "kf1.renderdistance", Name = "Render distance", Version = "1.0" };

    /// <summary>The nearest an undrawn cell in the cone comes to the eye, in world units.</summary>
    public static float GameEdge { get; private set; }
    static ulong _edgeOf;

    // This frame: the eye, the cone, and whether cells past the stencil are drawn.
    static bool _live;
    static float _camX, _camZ, _fx, _fz, _cos, _reach;
    static int _frameSerial = -1;

    static long _frames, _added, _addedMax, _models, _reportAt;

    public static void Configure()
    {
        _probe = Environment.GetEnvironmentVariable("KF1_RENDERDIST_PROBE") == "1";
        ModelsOn = Environment.GetEnvironmentVariable("KF1_RENDERDIST_MODELS")?.Trim() is not ("0" or "off");
    }

    public static void Install() => HookAttach.OnOverlayLoad("render distance", () =>
    {
        SymbolRegistry.Build();
        var walk = SymbolRegistry.Resolve("game", null, MapWalkAt);
        if (walk == null) return false;
        HookManager.AddPost(_self, walk, typeof(RenderDistance).GetMethod(nameof(AfterMapWalk), BindingFlags.Public | BindingFlags.Static)!,
            order: int.MaxValue);
        HookManager.Commit();
        return HookAttach.Installed(walk);
    });

    /// <summary>After the game's walk: the cells past its reach into the main view's
    /// gate, and the fade for the frame.</summary>
    public static void AfterMapWalk(CpuContext c, IMemory m)
    {
        _live = false;
        if (!Enabled || !GpuWorld.Drawing || m is not PSMemory mem) return;
        if (RetainedScene.Find(RetainedScene.Serial) is not { } frame) return;
        var view = frame.View;
        _camX = (float)view.CamX; _camZ = (float)view.CamZ;
        float len = MathF.Sqrt(view.R20 * view.R20 + view.R22 * view.R22);
        if (len < 1e-3f) return;
        _fx = view.R20 / len; _fz = view.R22 / len;
        float aspect = Widescreen.On ? Widescreen.Aspect : Widescreen.FourThree;
        float half = MathF.Atan(160f * aspect / Widescreen.FourThree / Math.Max(view.H, 1f));
        // A margin for the pitch and the cell's height: a cell too many costs a draw.
        _cos = MathF.Cos(Math.Min(half + 0.25f, MathF.PI * 0.95f));
        EnsureEdge(mem);
        _reach = Cells > 0 ? Math.Max(Cells * Cell, GameEdge) : 0;
        if (FadeCells > 0)
            RetainedScene.SetFade(Cells > 0 ? Cells * Cell : GameEdge, FadeCells * Cell, 0f);
        _frameSerial = RetainedScene.Serial;
        _frames++;
        if (Cells <= 0 || !GpuWorld.MapOn || !RetainedMap.Ready) { Report(); _live = Cells > 0; return; }
        _live = true;

        var gate = RetainedScene.CurrentMainHalves;
        if (gate.IsEmpty) return;
        int added = 0;
        int r = (int)MathF.Ceiling(_reach / Cell) + 1;
        int ec = (int)MathF.Floor(_camX / Cell), er = (int)MathF.Floor(_camZ / Cell);
        for (int row = Math.Max(0, er - r); row <= Math.Min(Grid - 1, er + r); row++)
            for (int col = Math.Max(0, ec - r); col <= Math.Min(Grid - 1, ec + r); col++)
            {
                int slot = RetainedMap.SlotOf(row * Grid + col, high: true);
                if (slot < 0 || (uint)slot >= (uint)gate.Length || gate[slot] != 0) continue;
                if (!Within(col, row)) continue;
                gate[slot] = 255;
                added++;
            }
        _added += added;
        _addedMax = Math.Max(_addedMax, added);
        Report();
    }

    /// <summary>Whether cell (col, row) lies past nothing the frame excludes: its nearest
    /// point within the reach, and some point of it inside the view's cone.</summary>
    static bool Within(int col, int row)
    {
        float x0 = col * Cell, z0 = row * Cell;
        float nx = Math.Clamp(_camX, x0, x0 + Cell) - _camX, nz = Math.Clamp(_camZ, z0, z0 + Cell) - _camZ;
        if (nx * nx + nz * nz > _reach * _reach) return false;
        if (nx == 0 && nz == 0) return true;
        for (int i = 0; i <= 2; i++)
            for (int j = 0; j <= 2; j++)
            {
                float vx = x0 + i * Cell / 2 - _camX, vz = z0 + j * Cell / 2 - _camZ;
                float d = MathF.Sqrt(vx * vx + vz * vz);
                if (d < 1f || (vx * _fx + vz * _fz) / d >= _cos) return true;
            }
        return false;
    }

    /// <summary>From the model walk's stencil tests: a model at cell (col, row) that the
    /// stencil leaves out is drawn when the frame's reach takes it.</summary>
    public static bool Reach(uint col, uint row)
    {
        if (!_live || !ModelsOn || _frameSerial != RetainedScene.Serial || !GpuWorld.Drawing || !GpuWorld.ModelsOn) return false;
        if (col >= Grid || row >= Grid || !Within((int)col, (int)row)) return false;
        _models++;
        return true;
    }

    // ---- the game's edge ---------------------------------------------------------

    static void EnsureEdge(PSMemory m)
    {
        var span = m.Ram.Slice((int)(Stencils & 0x1FFFFF), Stride * Count);
        ulong h = 0xCBF29CE484222325UL;
        foreach (byte b in span) h = (h ^ b) * 0x100000001B3UL;
        if (h == _edgeOf) return;
        _edgeOf = h;
        double half = Math.Atan(160.0 / 200.0);   // SetGeomScreen(200), 320 wide
        double cos = Math.Cos(half), best = double.MaxValue;
        ReadOnlySpan<int> headings = [0, 64, 128, 192, 255];
        ReadOnlySpan<double> cams = [0.05, 0.5, 0.95];
        for (int s = 0; s < Count; s++)
        {
            int o = s * Stride;
            int w = span[o] | span[o + 1] << 8, hgt = span[o + 2] | span[o + 3] << 8;
            int ox = span[o + 4] | span[o + 5] << 8, oz = span[o + 6] | span[o + 7] << 8;
            if (w != Box || hgt != Box) continue;
            int k = 15 - s;
            for (int dz = -Box; dz <= Box; dz++)
                for (int dx = -Box; dx <= Box; dx++)
                {
                    int bc = dx + ox, br = dz + oz;
                    bool drawn = bc >= 0 && bc < Box && br >= 0 && br < Box && span[o + 8 + br * Box + bc] != 0;
                    if (drawn) continue;
                    foreach (int t in headings)
                    {
                        double phi = (256 * k + t) * 2.0 * Math.PI / 4096.0;
                        double fx = -Math.Sin(phi), fz = Math.Cos(phi);
                        foreach (double cx in cams)
                            foreach (double cz in cams)
                            {
                                // The cell's nearest point to this eye, if any of it is in the cone.
                                double nx = Math.Clamp(cx, dx, dx + 1) - cx, nz = Math.Clamp(cz, dz, dz + 1) - cz;
                                double near = Math.Sqrt(nx * nx + nz * nz);
                                if (near >= best) continue;
                                if (InCone(dx - cx, dz - cz, fx, fz, cos)) best = near;
                            }
                    }
                }
        }
        GameEdge = (float)(best == double.MaxValue ? 12 * Cell : best * Cell);
        if (_probe) Console.WriteLine($"[KF1] render distance: the game's edge is {GameEdge:0} units ({GameEdge / Cell:0.00} cells)");
    }

    /// <summary>Whether any of a cell's sampled points, (vx, vz) from the eye to its
    /// corner, lies in the cone.</summary>
    static bool InCone(double vx, double vz, double fx, double fz, double cos)
    {
        for (int i = 0; i <= 3; i++)
            for (int j = 0; j <= 3; j++)
            {
                double px = vx + i / 3.0, pz = vz + j / 3.0, d = Math.Sqrt(px * px + pz * pz);
                if (d < 1e-6 || (px * fx + pz * fz) / d >= cos) return true;
            }
        return false;
    }

    static void Report()
    {
        if (!_probe || Environment.TickCount64 < _reportAt) return;
        _reportAt = Environment.TickCount64 + 5000;
        Console.WriteLine($"[KF1] render distance: cells={Cells:0.#} fade={FadeCells:0.#} edge={GameEdge:0} frames={_frames} " +
            $"cells added={_added} ({(_frames == 0 ? 0 : _added / (double)_frames):0.0} a frame, max {_addedMax}) models past the stencil={_models}");
    }
}
