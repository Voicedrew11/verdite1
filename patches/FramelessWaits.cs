using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Two GAME.EXE loops present once and then spin on the pad until a button: the
/// event picture <c>func_8002C794</c> (a sign) and the card message
/// <c>func_8001B7B0</c>. The runtime's watchdog counts presented frames, so a
/// player reading a sign for 15 s got a hang report. It is paused while either
/// runs. See "Drawing contexts" in docs/GPU_RENDERER.md.
/// </summary>
public static class FramelessWaits
{
    const uint EventPicture = 0x8002C794, CardMessage = 0x8001B7B0;

    static int _depth;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.frameless-waits",
        Name = "Frameless waits",
        Version = "1.0",
        Description = "Pauses the hang watchdog while a sign or the card message waits for a button.",
    };

    public static void Enter(CpuContext c, IMemory m)
    {
        _depth++;
        Watchdog.Paused = true;
    }

    public static void Leave(CpuContext c, IMemory m)
    {
        if (--_depth <= 0) { _depth = 0; Watchdog.Paused = false; }
    }

    public static void Install()
    {
        HookAttach.OnOverlayLoad("frameless-waits", () =>
        {
            SymbolRegistry.Build();
            MethodInfo M(string n) => typeof(FramelessWaits).GetMethod(n, BindingFlags.Public | BindingFlags.Static)!;
            var hooked = new List<MethodInfo>();
            foreach (uint addr in new[] { EventPicture, CardMessage })
            {
                if (SymbolRegistry.Resolve("game", null, addr) is not { } t) return false;
                HookManager.AddPre(_self, t, M(nameof(Enter)), int.MinValue);
                HookManager.AddPost(_self, t, M(nameof(Leave)), int.MaxValue);
                hooked.Add(t);
            }
            HookManager.Commit();
            return hooked.All(HookAttach.Installed);
        });
    }
}
