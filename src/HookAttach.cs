using System.Reflection;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Modding;

namespace Verdite.Core;

/// <summary>
/// Attach on overlay loads until the pass reports success, and claim only what
/// HookManager actually committed. The reasoning is "A registration is not a
/// hook" in Verdite2's docs/PATCHES_AND_MODS.md.
/// </summary>
static class HookAttach
{
    public const int MaxTries = 3;

    public static void OnOverlayLoad(string label, Func<bool> attach, string? hint = null, int maxTries = MaxTries)
    {
        bool done = false;
        int tries = 0;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (done || tries >= maxTries) return;
            tries++;
            try
            {
                done = attach();
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[{Game.Tag}] {label}: attach failed -- {e}");
            }
            if (!done && tries >= maxTries)
                Console.Error.WriteLine($"[{Game.Tag}] {label}: giving up on the rest; what is hooked is hooked." +
                                        (hint == null ? "" : $" {hint}"));
        });
    }

    /// <summary>Is a detour installed on this function? Only true after Commit.</summary>
    public static bool Installed(MethodInfo? target)
        => target != null && HookManager.IsCommitted(target);
}
