using System.Text;
using RecompOne.Runtime;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;

namespace Kf1;

/// <summary>
/// What the renderer was asked to draw, what was in VRAM to draw it from, and what
/// the retained scene drew, a line a second:
///
///     KF1_TEXPROBE=1   a line a second, on the console and into texprobe.log
///
/// It separates the places a missing texture can go wrong, which a picture cannot:
///
///   * **the game submitted flat polygons.** Textured-ness is a bit in the GP0
///     command the game wrote, decided long before any GL call. A census that reads
///     mostly flat points at the game's own state, not the shader.
///   * **VRAM has no texture in it.** The page census counts the distinct 16-bit
///     words in each of the 32 texture pages, read back out of the GL backend's own
///     VRAM. A page that received its upload holds hundreds; an empty one holds one.
///   * **the retained scene draws something else.** At KF1_GPU_WORLD=1 the map and
///     models are drawn from meshes and their packets are not written, so the packet
///     census only sees what is still packets (HUD, sprites, the arm). The retained
///     counts on the same line are the world's own: draws, triangles and instances
///     a second, the mode, and the frames the world was drawn on.
///
/// Nothing is hooked and nothing is written to game memory. The census is a
/// <c>RenderPrimEvent</c> listener, which both renderers pass through. The page
/// census is a <c>GlCore.ReadVram</c> from a <c>VSyncEvent</c> listener, once a second,
/// with <c>snapshot</c> false, so the diagnostic read does not offer its pixels to
/// the scaled restore copies (the copy a menu is restored from); the read still
/// flushes the backend's pending batch and writes back dirty VRAM, as any game
/// readback does.
/// </summary>
public static class TexProbe
{
    public static bool On { get; private set; }

    const string LogFile = "texprobe.log";

    static long _prims, _tex, _flat, _raw, _gouraud, _semi;
    static readonly HashSet<int> _cluts = [];
    static double _next;
    static bool _banner;

    static ushort[]? _vram;

    // The retained scene's counters are running totals: a second's share is the difference.
    static long _lastDraws, _lastMissed, _lastTris, _lastModelTris, _lastInstances;
    static long _lastFrames, _lastDrawingFrames;

    public static void Configure(string? probe)
    {
        if (probe != "1") return;
        On = true;

        Event.AddListener<RenderPrimEvent>(OnPrim);
        Event.AddListener<VSyncEvent>(_ => Tick());

        Say($"--- texprobe start {DateTime.Now:yyyy-MM-dd HH:mm:ss} ---");
    }

    static void OnPrim(RenderPrimEvent e)
    {
        _prims++;
        if (e.Textured)
        {
            _tex++;
            if (e.Raw) _raw++;
            if (_cluts.Count < 64) _cluts.Add(e.Clut);
        }
        else _flat++;
        if (e.Gouraud) _gouraud++;
        if (e.SemiTransparent) _semi++;
    }

    static void Tick()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_next == 0) { _next = now + 1.0; return; }
        if (now < _next) return;
        _next = now + 1.0;

        if (!_banner) { _banner = true; Banner(); }

        long prims = _prims, tex = _tex, flat = _flat;
        int pct = prims > 0 ? (int)(tex * 100 / prims) : 0;
        Say($"prims/s {prims}  textured {tex} ({pct}%)  flat {flat}  raw {_raw}  " +
            $"gouraud {_gouraud}  semitrans {_semi}  distinct cluts {_cluts.Count}");

        Retained();
        Pages();

        _prims = _tex = _flat = _raw = _gouraud = _semi = 0;
        _cluts.Clear();
    }

    /// <summary>The retained scene's own share of the second: the mode, the frames the
    /// world was drawn on, and what the GPU drew for it.</summary>
    static void Retained()
    {
        long draws = RetainedScene.MainDraws, missed = RetainedScene.MainMissed;
        long tris = RetainedScene.MainTriangles, modelTris = RetainedScene.MainModelTriangles;
        long instances = RetainedScene.InstancesDrawn;
        long frames = GpuWorld.Frames, drawing = GpuWorld.DrawingFrames;

        string mode = GpuWorld.Mode switch { 0 => "reference", 1 => "capture", _ => "retained" };
        Say($"  retained mode {mode}  world frames/s {frames - _lastFrames}  drawing {drawing - _lastDrawingFrames}  " +
            $"draws/s {draws - _lastDraws} (missed {missed - _lastMissed})  " +
            $"triangles/s {tris - _lastTris}  model triangles/s {modelTris - _lastModelTris}  " +
            $"instances/s {instances - _lastInstances}  blocker {GpuWorld.Blocker ?? "none"}");

        _lastDraws = draws; _lastMissed = missed; _lastTris = tris; _lastModelTris = modelTris;
        _lastInstances = instances; _lastFrames = frames; _lastDrawingFrames = drawing;
    }

    /// <summary>
    /// Distinct 16-bit words in each of the 32 texture pages (64 halfwords by 256 lines),
    /// read back out of the GL backend's own VRAM. A page holding one word never received
    /// an upload (or was filled and never written); a page holding art holds hundreds.
    /// </summary>
    static void Pages()
    {
        var be = GpuHle.Backend;
        if (be is not { Ready: true }) { Say("vram: no backend"); return; }

        _vram ??= new ushort[1024 * 512];
        try
        {
            // The non-evicting form: no offer to the scaled restore copies.
            if (be is RecompOne.Runtime.Hle.GlCore gl) gl.ReadVram(0, 0, 1024, 512, _vram, false);
            else be.ReadVram(0, 0, 1024, 512, _vram);
        }
        catch (Exception ex) { Say($"vram: read failed: {ex.Message}"); return; }

        var seen = new HashSet<ushort>();
        for (int py = 0; py < 2; py++)
        {
            var row = new StringBuilder($"vram y{py * 256,-3}:");
            for (int px = 0; px < 16; px++)
            {
                seen.Clear();
                for (int y = py * 256; y < py * 256 + 256; y++)
                {
                    int b = y * 1024 + px * 64;
                    for (int x = 0; x < 64; x++)
                    {
                        seen.Add(_vram[b + x]);
                        if (seen.Count > 999) break;
                    }
                    if (seen.Count > 999) break;
                }
                row.Append(' ').Append(seen.Count.ToString().PadLeft(4));
            }
            Say(row.ToString());
        }
    }

    static void Banner()
    {
        var gl = GpuGlAccess.Gl;
        string vendor = "?", renderer = "?", version = "?";
        if (gl != null)
        {
            try
            {
                vendor = gl.GetStringS(Silk.NET.OpenGL.StringName.Vendor) ?? "?";
                renderer = gl.GetStringS(Silk.NET.OpenGL.StringName.Renderer) ?? "?";
                version = gl.GetStringS(Silk.NET.OpenGL.StringName.Version) ?? "?";
            }
            catch { }
        }
        Say($"gl: {vendor} | {renderer} | {version}");
        Say($"backend {GpuBackendFactory.Selected}  ready {GpuHle.Backend?.Ready}  " +
            $"active {GpuHle.Active}  scale {GlVram.Scale}  truecolor {GteDepth.TrueColor}  " +
            $"perspective {GteDepth.Enabled}  subpixel {GteDepth.Subpixel}  zbuffer {GteDepth.ZBuffer}");
    }

    static void Say(string line)
    {
        Console.WriteLine($"[KF1] texprobe: {line}");
        try { File.AppendAllText(LogFile, line + Environment.NewLine); } catch { }
    }
}
