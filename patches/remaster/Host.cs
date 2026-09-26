using System.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;
using Kf2.Settings;

namespace Kf2.Remaster;

/// <summary>One layer of the remaster. Phase 1 has one, and carries only what it needs
/// of the interface in docs/REMASTER.md.</summary>
public interface IRemasterFeature
{
    string Id { get; }

    /// <summary>Once a frame on the game thread: resolve keys, apply edits.</summary>
    void OnFrame();

    /// <summary>Put back everything the feature changed.</summary>
    void Detach();

    /// <summary>The feature's part of the probe line.</summary>
    string Probe();
}

/// <summary>
/// The remaster: authored data, applied over the game from a pack.
///
///     KF2_REMASTER=1         on (off by default; nothing is applied until it is)
///     KF2_REMASTER_PACK=dir  the working pack (packs/working)
///     KF2_REMASTER_PROBE=1   a line every two seconds: the area, its fingerprint, what applied
///     KF2_REMASTER_LIGHTS=0  leave the pack's lights out
///     KF2_REMASTER_ATMOS=0   leave the pack's light-record overrides out
///     KF2_REMASTER_SHADOWS=0 no shadows from the authored lights
///     KF2_REMASTER_SHADOW_MODELS=0 only the map casts them
///
/// Shift+E opens the editor, which pauses the world. See docs/REMASTER.md.
/// </summary>
public static class Host
{
    public const string OnKey = "kf2.remaster.on";

    static bool? _forced;
    static bool _probe;
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _probeAt;
    static long _packetsAt;

    public static bool Enabled { get; private set; }

    public static readonly IRemasterFeature[] Features = [new Surfaces(), new Lights(), new Atmosphere()];

    public static void Configure(string? on, string? pack, string? probe, string? lights, string? atmos = null)
    {
        Lights.Configure(lights);
        Atmosphere.Configure(atmos);
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() != "0";
        Pack.Configure(pack);
        _probe = probe?.Trim() is not (null or "" or "0");
        Identity.Probe = _probe;
    }

    public static void Install()
    {
        Enabled = _forced ?? false;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? PatchSettings.Get(OnKey, false);
            Pack.Load();
            Pack.Watch();
            Editor.Register();
            Console.WriteLine($"[KF2] remaster: {(Enabled ? "on" : "off")}, pack {Pack.Root}" +
                              (Pack.LastError != null ? $" ({Pack.LastError})" : ""));
        });

        Event.AddListener<OverlayLoadedEvent>(e => Identity.Invalidate(e.Name));
        Identity.Install();
        Event.AddListener<VSyncEvent>(_ => Frame());
        Lights.Install();
        Atmosphere.Install();
        Editor.Install();
    }

    public static void SetEnabled(bool on)
    {
        if (on == Enabled) return;
        Enabled = on;
        PatchSettings.Set(OnKey, on);
        if (!on)
            foreach (var f in Features) f.Detach();
    }

    /// <summary>Off and closed, one test.</summary>
    static void Frame()
    {
        Faces.Recording = Editor.Open || FaceProbe.On;
        Faces.Wanted = Faces.Recording || Surfaces.PerFace;
        if (!Enabled && !Editor.Open) return;
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return;
        Identity.Poll(m);
        Pack.Poll();
        foreach (var f in Features) f.OnFrame();
        if (_probe) Report();
    }

    static void Report()
    {
        double now = _clock.Elapsed.TotalSeconds;
        if (now < _probeAt) return;
        double dt = _probeAt == 0 ? 2.0 : now - _probeAt + 2.0;
        _probeAt = now + 2.0;
        long packets = Surfaces.Packets - _packetsAt;
        _packetsAt = Surfaces.Packets;
        Console.WriteLine($"[KF2] remaster: {(Enabled ? "on" : "off")}, area {Identity.Area} " +
                          (Identity.Settled ? $"settled (fingerprint {Identity.FingerprintText} {(Identity.FromLoad ? "as loaded" : "LIVE, the load was not seen")}, gap {Identity.LastGap})" : "not settled") +
                          $"; {string.Join("; ", Features.Select(f => f.Probe()))}; {packets / dt:F0} authored packet(s)/s");
    }
}
