using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Hold OPEN.EXE's 3D present, <c>func_80016DD0</c> (the attract, its fades, the
/// ending), to a fixed number of vblanks a frame.
///
///     KF1_OPENPACING=3   vblanks a frame (default: 20 fps, GAME.EXE's own gate)
///     KF1_OPENPACING=1   off: the runtime's 60 Hz ceiling, as before
///
/// Those loops step their scene once a frame and wait only a VSync(0), so on the
/// console their rate was the frame's cost; recompiled they ran at 60. Only the
/// present's own VSync is held: the title's 1000-vblank wait (<c>func_8001455C</c>)
/// calls VSync itself and keeps its timing. See "OPEN.EXE's frame rate" in
/// docs/GAME_INTERNALS.md.
/// </summary>
public static class OpenPacing
{
    const uint VSyncThunk = 0x800352C4;   // LibEtc.VSync, OPEN.EXE's copy
    const uint PresentSite = 0x80016DEC;  // its call in func_80016DD0

    const double VBlankMs = 1000.0 / 60.0;
    const double ResyncPeriods = 4.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.openpacing",
        Name = "OPEN.EXE pacing",
        Version = "1.0",
        Description = "Holds the attract and the ending to a fixed number of vblanks a frame.",
    };

    static readonly Stopwatch _clock = Stopwatch.StartNew();

    public static int VBlanks { get; set; } = 3;
    static double _due = -1.0;
    static bool _hooked;

    public static void Configure(string? vblanks)
    {
        if (int.TryParse(vblanks, out var n)) VBlanks = Math.Clamp(n, 1, 6);
    }

    public static void Install()
    {
        HookAttach.OnOverlayLoad("open pacing", () =>
        {
            if (_hooked) return true;
            SymbolRegistry.Build();
            if (SymbolRegistry.Resolve("open", null, VSyncThunk) is not { } t) return false;
            var pre = typeof(OpenPacing).GetMethod(nameof(BeforeVSync), BindingFlags.Public | BindingFlags.Static)!;
            HookManager.AddPre(_self, t, pre);
            HookManager.Commit();
            _hooked = HookAttach.Installed(t);
            Console.WriteLine($"[KF1] open pacing: {(VBlanks > 1 ? $"{VBlanks} vblanks a frame" : "off")}, " +
                              $"present {(_hooked ? "held" : "NOT held")}");
            return _hooked;
        });
    }

    public static void BeforeVSync(CpuContext c, IMemory m)
    {
        if (VBlanks <= 1 || c.RA != PresentSite) return;

        double now = _clock.Elapsed.TotalMilliseconds;
        if (_due < 0.0 || _due < now - ResyncPeriods * VBlankMs) _due = now;
        _due += VBlanks * VBlankMs;

        // Poll while waiting, so the vblank handlers (the sound driver's among them)
        // run on time rather than in a burst at the present.
        while (_clock.Elapsed.TotalMilliseconds < _due)
        {
            RecompOne.Runtime.Interrupts.Poll(c, m);
            if (_due - _clock.Elapsed.TotalMilliseconds > 2.0) Thread.Sleep(1);
            else Thread.SpinWait(48);
        }
    }
}
