using System.Reflection;
using ImGuiNET;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// The mouse spent through stage A's own turn and look, once a world tick. Each
/// axis is Verdite3's three branches, inline in stage A here: a held button adds
/// <c>accel</c> to a velocity and clamps it, no button decays it by <c>decay</c>
/// and applies what is left unclamped, then the angle takes the velocity. So
/// writing <c>step ± decay</c> into the velocity with the axis's buttons masked out
/// of the tick's pad word lands on <c>step</c> exactly, through the game's own
/// pitch limit.
///
///     yaw    vel 0x800A0846, decay rate>>2 (rate s32 0x80057E70), angle 0x800A083A, &amp; 0xFFF
///     pitch  vel 0x800A0848, decay 2, angle 0x800A0838, clamped to ±0xBF
///
/// The pad word is stage A's: func_8005012C's return at 0x80018900, before any
/// test. A tick the mouse drove is followed by a zero velocity, so the view does
/// not coast on the decay when the hand stops. The spending is <see cref="Analog"/>'s,
/// which decides the mouse and the sticks together. See "Mouse look" in docs/INPUT.md.
/// </summary>
public static class MouseLook
{
    /// <summary>This game's values for Verdite.Core.Mouse.</summary>
    public static readonly MouseGame Game = new(
        UnitsPerDegree: 4096f / 360f,   // 12 bits to yaw's circle
        DegreesPerPixel: 0.15f,         // a quarter turn is about 600 px at sensitivity 1
        StepCap: 1024,                  // the most one tick may turn, in yaw units
        PitchLimit: 0xBF,               // stage A's clamp, about 17 degrees
        YawAddress: 0x800A083A,         // u16, 0x1000 a turn
        PitchAddress: 0x800A0838,       // s16
        DefaultLeftButton: 4,           // Triangle: attack
        DefaultRightButton: 3,          // Square: the held item or spell
        DefaultMiddleButton: 2,         // Circle: examine
        TextEditing: () => ImGui.GetCurrentContext() != nint.Zero && ImGui.GetIO().WantTextInput,
        Frames: () => FramePacing.Frames,
        LogicHz: () => FramePacing.LogicHz);

    const uint PadRead = 0x8005012C;
    const uint StageASite = 0x800188F8 + 8;   // stage A's call of it

    static readonly ModInfo _self = new() { Id = "kf1.mouselook", Name = "Mouse look", Version = "1.0" };

    /// <summary>The guest memory, for readers outside a hook (the view's lead).</summary>
    internal static IMemory? Memory { get; private set; }

    public static void Install() => HookAttach.OnOverlayLoad("mouse look", () =>
    {
        SymbolRegistry.Build();
        if (SymbolRegistry.Resolve("game", null, PadRead) is not { } t) return false;
        HookManager.AddPost(_self, t, typeof(MouseLook).GetMethod(nameof(AfterPadRead), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        bool ok = HookAttach.Installed(t);
        Console.WriteLine(ok ? "[KF1] mouse look: attached" : "[KF1] mouse look: not installed");
        return ok;
    });

    /// <summary>Stage A's pad word: the mouse and the sticks spent into it (Analog).</summary>
    public static void AfterPadRead(CpuContext c, IMemory m)
    {
        if (c.RA != StageASite) return;
        Memory = m;
        c.V0 = Analog.Tick(m, (ushort)c.V0);
    }
}
