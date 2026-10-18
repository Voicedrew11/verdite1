using System.Reflection;
using System.Text.Json;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// The retained world's switch, its frame and its census, Verdite3's GpuWorld on
/// this game's renderer. The map (<see cref="RetainedMap"/>) and the models
/// (<see cref="RetainedModels"/>) are published to the shared runtime's
/// <see cref="RetainedScene"/>, which draws them at the ordering table's far end
/// under the packets the game still writes.
///
///     KF1_GPU_WORLD=0|reference  the packets only
///     KF1_GPU_WORLD=capture      the scene published beside the packets, nothing suppressed
///     KF1_GPU_WORLD=1|retained   the scene drawn; the world's packets suppressed (the default)
///     KF1_GPU_MAP=0              the map left to its packets
///     KF1_GPU_MODELS=0           the models left to their packets
///     KF1_GPU_ARM=0              the arm left to its packets
///     KF1_GPU_SPRITES=0          the flipbook sprites left to their packets
///     KF1_GPU_CHECK=1            the map's and models' corners checked against the GTE (capture)
///     KF1_GPU_CENSUS=1           a line every 5 s
///     KF1_GPU_CENSUS_FILE=path   the census as JSON, every 5 s
///
/// A fallback (a face left to the packets) is counted by routine, caller and reason,
/// never silent. Step 3 of docs/RENDERER_PLAN.md; see "The retained scene" in
/// docs/GPU_RENDERER.md.
/// </summary>
public static class GpuWorld
{
    /// <summary>0 reference, 1 capture beside the packets, 2 retained.</summary>
    public static int Mode { get; private set; }

    public static int Setting
    {
        get => Mode;
        set
        {
            Mode = Math.Clamp(value, 0, 2);
            _frame = false;
            RetainedScene.MainSerial = RetainedScene.ArmSerial = 0;
            RetainedScene.MainView = false;
            if (Mode == 2 && !GteDepth.ZBuffer) ZBuffer.Enabled = true;
        }
    }

    public static bool MapOn = true, ModelsOn = true, ArmOn = true, SpritesOn = true;
    public static bool Check, CensusOn, SurfaceProbe;
    static string? _censusFile;

    // Nesting counts of the renderer's domains, kept by hooks on their routines.
    static int _scene, _map, _models, _arm;
    static bool _attached, _frame;
    static long _reportAt;

    public static long Frames, MapCells, MapRetained, ModelSubmits, ModelRetained, CachedRetained, ArmRetained;

    /// <summary>Bytes of the packet buffer each domain spent this run, while drawing
    /// retained: what the census means by "world packets".</summary>
    public static long MapBytes, ModelBytes, ArmBytes, DrawingFrames;

    static readonly Dictionary<string, long> Reasons = new();
    static uint _cursorAt;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.gpu-world",
        Name = "Retained world renderer",
        Version = "1.0",
        Description = "The map and models drawn by the GPU from meshes kept on it.",
    };

    const uint Renderer = 0x8001FDE4, FrameHead = 0x8001BFB8, Present = 0x8001C050;
    const uint MapWalk = 0x8001E83C, ModelWalk = 0x8001F218, Arm = 0x8001F798;
    const uint PacketRecord = 0x80070EB8;

    // The frame profiler's section for the scene's per-frame setup (free while profiling is off).
    static readonly int ProfBegin = Profiler.Register("retained scene: begin frame", ProfileGroup.Hook);

    public static string Domain => _arm > 0 ? "arm" : _models > 0 ? "models" : _map > 0 ? "map" : "hud";

    /// <summary>The scene is being published this frame (capture or retained).</summary>
    public static bool Capture => Mode != 0 && _frame && _scene > 0 && Literal.Verifying == 0;

    /// <summary>The scene is drawn by the GPU, and the packets it stands for are not written.</summary>
    public static bool Drawing => Capture && Mode == 2 && Blocker == null;

    public static string? Blocker =>
        !RetainedScene.Supported ? "backend-capability"
        : !GteDepth.ZBuffer ? "zbuffer-off"
        : global::Kf1.Renderer.Setting == 2 ? "renderer-verify"
        : null;

    static bool Flag(string name, bool fallback)
    {
        string? v = Environment.GetEnvironmentVariable(name)?.Trim().ToLowerInvariant();
        return string.IsNullOrEmpty(v) ? fallback : v is not ("0" or "off");
    }

    public static void Configure()
    {
        Mode = Environment.GetEnvironmentVariable("KF1_GPU_WORLD")?.Trim().ToLowerInvariant() switch
        {
            "0" or "off" or "reference" => 0,
            "capture" or "shadow" => 1,
            _ => 2,
        };
        MapOn = Flag("KF1_GPU_MAP", true);
        ModelsOn = Flag("KF1_GPU_MODELS", true);
        ArmOn = Flag("KF1_GPU_ARM", true);
        SpritesOn = Flag("KF1_GPU_SPRITES", true);
        Check = Flag("KF1_GPU_CHECK", false);
        CensusOn = Flag("KF1_GPU_CENSUS", false);
        SurfaceProbe = Flag("KF1_GPU_SURFACE_PROBE", false);
        GteDepth.AoProbe = Flag("KF1_AO_PROBE", false);
        _censusFile = Environment.GetEnvironmentVariable("KF1_GPU_CENSUS_FILE") is { Length: > 0 } f ? f : null;
        if (_censusFile != null) CensusOn = true;
        // The main view needs a depth buffer: retained turns the Z-buffer on.
        if (Mode == 2 && !GteDepth.ZBuffer) ZBuffer.Enabled = true;
    }

    public static void Install()
    {
        HookAttach.OnOverlayLoad("gpu world", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        uint[] at = [Renderer, MapWalk, ModelWalk, Arm, FrameHead, Present];
        var methods = at.Select(a => SymbolRegistry.Resolve("game", null, a)).ToArray();
        if (methods.Any(x => x == null)) return false;
        if (!_attached)
        {
            var t = typeof(GpuWorld);
            string[] pre = [nameof(SceneIn), nameof(MapIn), nameof(ModelsIn), nameof(ArmIn)];
            string[] post = [nameof(SceneOut), nameof(MapOut), nameof(ModelsOut), nameof(ArmOut)];
            for (int i = 0; i < 4; i++)
            {
                HookManager.AddPre(_self, methods[i]!, t.GetMethod(pre[i])!, order: int.MinValue + 1);
                HookManager.AddPost(_self, methods[i]!, t.GetMethod(post[i])!, order: int.MaxValue - 1);
            }
            HookManager.AddPost(_self, methods[4]!, t.GetMethod(nameof(Begin))!);
            HookManager.AddPre(_self, methods[5]!, t.GetMethod(nameof(BeforePresent))!);
            _attached = true;
        }
        HookManager.Commit();
        return methods.All(HookAttach.Installed);
    }

    static uint Cursor(IMemory m) => m.ReadU32(m.ReadU32(PacketRecord) + 8u);

    public static void SceneIn(CpuContext c, IMemory m) => _scene++;
    public static void SceneOut(CpuContext c, IMemory m) { _scene--; _frame = false; }

    public static void MapIn(CpuContext c, IMemory m)
    {
        _map++;
        _cursorAt = Cursor(m);
        if (Capture) ReadProjection();
        if (Capture && MapOn && m is PSMemory mem) RetainedMap.Walk(mem);
    }

    public static void MapOut(CpuContext c, IMemory m)
    {
        _map--;
        if (Drawing) MapBytes += Cursor(m) - _cursorAt;
    }

    public static void ModelsIn(CpuContext c, IMemory m)
    {
        _models++;
        _cursorAt = Cursor(m);
        if (Capture) ReadProjection();
    }

    public static void ModelsOut(CpuContext c, IMemory m)
    {
        _models--;
        if (Drawing) ModelBytes += Cursor(m) - _cursorAt;
    }

    // The projection the world was drawn under, put back after the arm: the arm
    // projects at H 2000, and the passes that rebuild positions from depth read the
    // last projection noted.
    static float _worldH, _worldCx, _worldCy;

    public static void ArmIn(CpuContext c, IMemory m)
    {
        if (_arm++ == 0) (_worldH, _worldCx, _worldCy) = (GteDepth.ProjH, GteDepth.ProjCx, GteDepth.ProjCy);
        _cursorAt = Cursor(m);
    }

    public static void ArmOut(CpuContext c, IMemory m)
    {
        if (--_arm == 0) (GteDepth.ProjH, GteDepth.ProjCx, GteDepth.ProjCy) = (_worldH, _worldCx, _worldCy);
        if (Drawing) ArmBytes += Cursor(m) - _cursorAt;
    }

    /// <summary>After the frame head: the frame's camera, and the scene begun.</summary>
    public static void Begin(CpuContext c, IMemory m)
    {
        _frame = false;
        if (Mode == 0 || _scene == 0 || Literal.Verifying != 0 || m is not PSMemory mem) return;
        int p = Profiler.Begin(ProfBegin);
        try
        {
            var view = ReadView(mem);
            RetainedScene.BeginFrame(view);
            RetainedScene.MainView = Mode == 2 && Blocker == null;
            RetainedScene.MainSerial = RetainedScene.MainView ? RetainedScene.Serial : 0;
            RetainedModels.Frame();
            _frame = true;
            Frames++;
            if (Drawing) DrawingFrames++;
        }
        finally { Profiler.End(p); }
    }

    /// <summary>The world's projection, as the walks find the GTE (the frame head's is
    /// another draw's: H 1000 there, 200 in the walks).</summary>
    public static float ProjH = 200f, ProjCx = 160f, ProjCy = 120f;

    static void ReadProjection()
    {
        if ((ushort)Gte.ReadControl(26) is not (> 0 and var h)) return;
        float cx = (int)Gte.ReadControl(24) / 65536f, cy = (int)Gte.ReadControl(25) / 65536f;
        if (h == ProjH && cx == ProjCx && cy == ProjCy) return;
        ProjH = h; ProjCx = cx; ProjCy = cy;
        if (_frame && RetainedScene.Find(RetainedScene.Serial) is { } f) { f.View.H = h; f.View.Cx = cx; f.View.Cy = cy; }
    }

    /// <summary>The camera the frame is drawn from: the view matrix the camera block
    /// built (world to view, 4.12) and the eye it subtracts.</summary>
    public static RetainedScene.View ReadView(IMemory m)
    {
        uint p = CameraBlock.ViewMatrix;
        var cam = CameraBlock.Read(m);
        return new()
        {
            R00 = (short)m.ReadU16(p) / 4096f, R01 = (short)m.ReadU16(p + 2) / 4096f, R02 = (short)m.ReadU16(p + 4) / 4096f,
            R10 = (short)m.ReadU16(p + 6) / 4096f, R11 = (short)m.ReadU16(p + 8) / 4096f, R12 = (short)m.ReadU16(p + 10) / 4096f,
            R20 = (short)m.ReadU16(p + 12) / 4096f, R21 = (short)m.ReadU16(p + 14) / 4096f, R22 = (short)m.ReadU16(p + 16) / 4096f,
            CamX = cam.X, CamY = cam.Y, CamZ = cam.Z,
            Tx = (int)m.ReadU32(p + 20), Ty = (int)m.ReadU32(p + 24), Tz = (int)m.ReadU32(p + 28),
            H = ProjH, Cx = ProjCx, Cy = ProjCy,
        };
    }

    /// <summary>A face, a cell or a model left to the packets.</summary>
    public static void Fallback(uint routine, uint caller, string reason)
    {
        string key = $"{Domain}:{routine:X8}:{caller:X8}:{reason}";
        Reasons.TryGetValue(key, out long n);
        Reasons[key] = n + 1;
    }

    public static void BeforePresent(CpuContext c, IMemory m)
    {
        if (!_frame || !CensusOn || Environment.TickCount64 < _reportAt) return;
        _reportAt = Environment.TickCount64 + 5000;
        Console.WriteLine($"[KF1] gpu world: mode={Mode} frames={Frames} drawing={DrawingFrames} blocker={Blocker ?? "none"} " +
            $"map cells={MapCells} retained={MapRetained} builds={RetainedMap.Builds} restores={RetainedMap.Restores} slots={RetainedMap.Slots} corners={RetainedMap.Corners} " +
            $"models={ModelSubmits} retained={ModelRetained} (cached writer {CachedRetained}) arm={ArmRetained} meshes={RetainedModels.MeshBuilds}/{RetainedModels.MeshHits} " +
            $"poses={RetainedModels.PoseBuilds}/{RetainedModels.PoseHits} pose-uploads={RetainedModels.PoseTexelsUploaded} " +
            $"gpu draws/missed={RetainedScene.MainDraws}/{RetainedScene.MainMissed} instances={RetainedScene.InstancesDrawn} " +
            $"arm draws/calls/missed/cut={RetainedScene.ArmDraws}/{RetainedScene.ArmCalls}/{RetainedScene.ArmMissed}/{RetainedScene.ArmCut} " +
            $"blended noted/drawn={RetainedScene.BlendNoted}/{RetainedScene.BlendDrawn} " +
            $"packet bytes map/models/arm={MapBytes}/{ModelBytes}/{ArmBytes} fallbacks={Reasons.Values.Sum()}");
        Console.WriteLine($"[KF1] map grid: walks={RetainedMap.GridWalks} changed walks={RetainedMap.GridChangedWalks} " +
            $"changed bytes={RetainedMap.GridChangedBytes}");
        if (CensusOn) Console.WriteLine($"[KF1] gpu sprites: {SpriteAssembler.Report()}");
        if (Check) Console.WriteLine($"[KF1] gpu check: {RetainedMap.CheckReport()} | {RetainedModels.CheckReport()} | {SpriteAssembler.CheckReport()} | {SemiAbr.KeyReport()} | {LightCheck.Report()}");
        // Step 4's switches and what reached the buffers: the normal pass's triangles, the
        // occlusion's passes, the mip keys and the live uniforms.
        Console.WriteLine($"[KF1] gpu picture: perpixel={(GteLightMap.Enabled ? 1 : 0)} fog={SceneFeatures.Fog} ao={(GteDepth.AmbientOcclusion ? 1 : 0)} " +
            $"aniso={GteDepth.Anisotropy} live={(GteDepth.AnisotropyLive ? 1 : 0)} mips={(GteDepth.Mipmaps ? 1 : 0)} found/keys={RetainedScene.MipsFound}/{RetainedScene.MipsKeys} " +
            $"plain={GteDepth.PlainDepth:0} live={(GteDepth.PlainDepthLive ? 1 : 0)} ao passes/no-target={GteDepth.AoPasses}/{GteDepth.AoNoTarget} " +
            $"normal triangles map/models={RetainedScene.MainNormalTriangles}/{RetainedScene.MainModelNormalTriangles} map prepasses={RetainedScene.MainMapPrepasses} " +
            $"fade={RenderDistance.FadeCells:0.#} reach={RenderDistance.Cells:0.#} edge={RenderDistance.GameEdge:0}");
        if (SurfaceProbe)
        {
            // Every pixel with a depth should carry a surface: missing counts a family
            // that wrote depth but no normal, ahead an opaque surface in front of the depth.
            Console.WriteLine($"[KF1] surface probe: checks={RetainedScene.SurfaceChecks} pixels={RetainedScene.SurfaceDepthPixels} " +
                $"behind={RetainedScene.SurfaceBehind} ahead={RetainedScene.SurfaceAhead} missing={RetainedScene.SurfaceMissing} " +
                $"ids none/opaque/water/overlay/authored/blended={string.Join('/', RetainedScene.SurfaceIds)}");
            RetainedScene.SurfaceCheck = true;
        }
        foreach (var (k, v) in Reasons.OrderByDescending(kv => kv.Value).Take(8))
            Console.WriteLine($"[KF1] gpu fallback: {k} x{v}");
        if (_censusFile != null)
            File.WriteAllText(_censusFile, JsonSerializer.Serialize(new
            {
                Mode, Frames, DrawingFrames, Blocker = Blocker ?? "none",
                Map = new { Cells = MapCells, Retained = MapRetained, RetainedMap.Builds, RetainedMap.Restores, RetainedMap.Slots, RetainedMap.Corners, Bytes = MapBytes },
                Models = new
                {
                    Submits = ModelSubmits, Retained = ModelRetained, Cached = CachedRetained, Arm = ArmRetained, Bytes = ModelBytes, ArmBytes,
                    RetainedModels.MeshBuilds, RetainedModels.MeshHits, RetainedModels.PoseBuilds, RetainedModels.PoseHits,
                    RetainedModels.PoseTexelsUploaded, RetainedModels.VertexUploads,
                },
                Gpu = new { RetainedScene.MainDraws, RetainedScene.MainMissed, RetainedScene.MainTriangles, RetainedScene.InstancesDrawn,
                    RetainedScene.MainModelTriangles, RetainedScene.ArmDraws, RetainedScene.ArmMissed },
                Check = Check ? new { Map = RetainedMap.CheckReport(), Models = RetainedModels.CheckReport() } : null,
                Reasons,
            }, new JsonSerializerOptions { WriteIndented = true }));
    }
}
