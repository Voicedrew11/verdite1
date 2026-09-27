using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Creature animation, carried between ticks with the view and the positions.
///
/// An animated creature (entry <c>+4</c>, the clip, not 0xFF) is posed by
/// <c>func_800205D4(entry+0x34, model, clip, time)</c>, <c>time</c> being the
/// u16 at entry <c>+8</c>, in ticks. The clip is a list of segments, each a flag
/// and a duration; the routine finds the segment <c>time</c> lies in, copies that
/// segment's base pose when the segment changes, and morphs it towards the next
/// keyframe with <c>gteMIMefunc</c> at a weight of
/// <c>((time - start) &lt;&lt; 12) / duration</c> (or 4096 minus that when the
/// segment's flag is set). Time is a whole number of ticks, so a pose changes only
/// at a tick.
///
/// For a carried picture this works out the time the picture stands for --
/// <c>cur - (cur - prev) * (1 - phase)</c>, as <see cref="ObjectCarry"/> places the
/// creature -- walks the same segment table in C#, and when that time lies in the
/// segment the game chose for <c>cur</c>, hands <c>gteMIMefunc</c> (at its one call
/// that applies the weight, return address <c>0x8002092C</c>) the weight for it. A
/// different segment, a different clip, or a step backwards leaves the game's
/// pose alone. See "Creature animation" in docs/KF1.md.
///
///     KF2_SMOOTH_ANIM=0          leave poses at the tick
///     KF2_SMOOTH_ANIM_PROBE=1    a line a second: poses, and how many were carried
/// </summary>
public static class AnimCarry
{
    const uint PoseRoutine = 0x800205D4;
    const uint Morph = 0x8004C860;            // gteMIMefunc
    const uint MorphWeightReturn = 0x8002092C;
    const uint ModelTable = 0x80090FCC;       // u32 per model: its animation header

    public static bool Enabled { get; private set; } = true;
    static bool? _forced;
    static bool _probe;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.animcarry",
        Name = "Animation smoothing",
        Version = "1.0",
        Description = "Poses animated creatures between ticks.",
    };

    // Per creature entry, the time the current picture stands for.
    static readonly Dictionary<uint, double> _display = new();

    // Set for the length of one pose call.
    static int _override = -1;

    static long _poses, _carried;
    static double _windowStart;

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() != "0";
        _probe = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
    }

    public static void Install()
    {
        if (_forced is { } f) Enabled = f;
        Event.AddListener<RuntimeReadyEvent>(_ =>
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(ObjectCarry.OnKey, true));
        HookAttach.OnOverlayLoad("anim carry", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var pose = SymbolRegistry.Resolve("game", null, PoseRoutine);
        var morph = SymbolRegistry.Resolve("game", null, Morph);
        if (pose == null || morph == null) return false;
        const BindingFlags f = BindingFlags.Public | BindingFlags.Static;
        HookManager.AddPre(_self, pose, typeof(AnimCarry).GetMethod(nameof(BeforePose), f)!);
        HookManager.AddPost(_self, pose, typeof(AnimCarry).GetMethod(nameof(AfterPose), f)!);
        HookManager.AddPre(_self, morph, typeof(AnimCarry).GetMethod(nameof(BeforeMorph), f)!);
        HookManager.Commit();
        return HookAttach.Installed(pose) && HookAttach.Installed(morph);
    }

    /// <summary>Called by <see cref="ObjectCarry"/> for each carried creature.</summary>
    internal static void Note(uint entry, double time) => _display[entry] = time;

    internal static void Clear() => _display.Clear();

    public static void BeforePose(CpuContext c, IMemory m)
    {
        _override = -1;
        _poses++;
        if (!Enabled || !ViewCarry.Carrying) return;
        uint entry = c.A0 - 0x34;
        if (!_display.TryGetValue(entry, out double t)) return;

        uint model = c.A1 & 0xFFFF, clip = c.A2 & 0xFFFF, time = c.A3 & 0xFFFF;
        if (t >= time || t < 0) return;
        var seg = Segment(m, model, clip, time);
        var at = Segment(m, model, clip, (uint)Math.Floor(t));
        if (seg is not { } s || at is not { } a || s.Index != a.Index) return;

        double w = (t - s.Start) * 4096.0 / s.Duration;
        if (s.Flag != 0) w = 4096.0 - w;
        _override = (int)Math.Clamp(Math.Round(w), 0, 4096);
        _carried++;
    }

    public static void AfterPose(CpuContext c, IMemory m)
    {
        _override = -1;
        if (_probe) Report();
    }

    public static void BeforeMorph(CpuContext c, IMemory m)
    {
        if (_override >= 0 && c.RA == MorphWeightReturn) c.A3 = (uint)_override;
    }

    /// <summary>The segment <paramref name="time"/> lies in, as the pose routine
    /// finds it (0x800206E0-0x800207A8).</summary>
    static (int Index, uint Start, uint Duration, ushort Flag)? Segment(IMemory m, uint model, uint clip, uint time)
    {
        uint hdr = m.ReadU32(ModelTable + model * 4);
        if (hdr == 0) return null;
        uint clips = hdr + m.ReadU32(hdr + 0x10);
        uint list = hdr + m.ReadU32(clips + clip * 4);
        int count = m.ReadU16(list);
        if (count <= 0 || count > 256) return null;
        uint start = 0;
        for (int i = 0; i < count; i++)
        {
            uint seg = hdr + m.ReadU32(list + 4 + (uint)i * 4);
            uint dur = m.ReadU16(seg + 2);
            uint end = (start + dur) & 0xFFFF;
            if (time < end && dur != 0) return (i, start, dur, m.ReadU16(seg));
            start = end;
        }
        return null;
    }

    static void Report()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_windowStart == 0) { _windowStart = now; return; }
        double secs = now - _windowStart;
        if (secs < 1.0) return;
        Console.WriteLine($"[KF1] anim: {_poses / secs:0.0} poses/s, {_carried / secs:0.0} carried, {_display.Count} creatures timed");
        _poses = _carried = 0;
        _windowStart = now;
    }
}
