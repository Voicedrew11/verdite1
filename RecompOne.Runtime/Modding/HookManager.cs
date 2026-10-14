using System.Diagnostics;
using System.Reflection;
using MonoMod.RuntimeDetour;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Modding;

public static class HookManager
{
    private sealed class Entry<T>
    {
        public ModInfo Mod = null!;
        public T Fn = default!;
        public string Name = "";
        public int Profile;
        public int Order;
    }

    private sealed class FunctionHooks
    {
        public Entry<Func<CpuContext, IMemory, bool>>[] Pres = [];
        public Entry<Action<CpuContext, IMemory>>[] Posts = [];
        public Action<Action<CpuContext, IMemory>, CpuContext, IMemory>? Replace;
        public ModInfo? ReplaceOwner;
        public string ReplaceName = "";
        public string Name = "";
        public Action<CpuContext, IMemory>? OrigSeen, OrigCounted;
        public int OrigRuns, OrigMisses;
        public Hook? Hook;
        public int Profile;
        public int ReplaceProfile;

        public bool Empty => Pres.Length == 0 && Posts.Length == 0 && Replace == null;
    }

    private static readonly Dictionary<MethodInfo, FunctionHooks> _hooks = [];
    private static readonly object _gate = new();

    private static readonly Type[] SigBasic = [typeof(CpuContext), typeof(IMemory)];
    private static readonly Type[] SigOrig = [typeof(Action<CpuContext, IMemory>), typeof(CpuContext), typeof(IMemory)];

    public static int HookedFunctionCount
    {
        get
        {
            lock (_gate)
            {
                return _hooks.Count;
            }
        }
    }

    public static int CountForMod(ModInfo mod)
    {
        lock (_gate)
        {
            var n = 0;
            foreach (var (_, h) in _hooks)
            {
                foreach (var p in h.Pres)
                    if (p.Mod == mod)
                        n++;
                foreach (var p in h.Posts)
                    if (p.Mod == mod)
                        n++;
                if (h.ReplaceOwner == mod) n++;
            }

            return n;
        }
    }

    public static bool AddReplace(ModInfo mod, MethodInfo target, MethodInfo impl)
    {
        Action<Action<CpuContext, IMemory>, CpuContext, IMemory> replace;
        if (Matches(impl, typeof(void), SigOrig))
        {
            replace = impl.CreateDelegate<Action<Action<CpuContext, IMemory>, CpuContext, IMemory>>();
        }
        else if (Matches(impl, typeof(void), SigBasic))
        {
            var direct = impl.CreateDelegate<Action<CpuContext, IMemory>>();
            replace = (orig, c, m) => direct(c, m);
        }
        else
        {
            Console.Error.WriteLine($"[Mods] {mod.Id}: invalid replace signature on {Describe(impl)}");
            return false;
        }

        lock (_gate)
        {
            var hooks = Get(target);
            if (hooks.ReplaceOwner != null)
            {
                Console.Error.WriteLine(
                    $"[Mods] replace conflict on {target.Name}: {mod.Id} ignored, {hooks.ReplaceOwner.Id} already owns it");
                return false;
            }

            if (Refused(mod, target)) return false;
            hooks.Replace = replace;
            hooks.ReplaceOwner = mod;
            hooks.ReplaceName = Describe(impl);
            hooks.ReplaceProfile = HookSection(impl, "replace");
        }

        return true;
    }

    //0070. Pres and posts on one function run in ascending `order`, and in the
    //order they were added among equals. Without it the order is whatever order
    //the patches happened to be installed in, and a post that must run after
    //another's (a redraw after the restores) depends on a line's place in a file.
    public static bool AddPre(ModInfo mod, MethodInfo target, MethodInfo impl, int order = 0)
    {
        Func<CpuContext, IMemory, bool> pre;
        if (Matches(impl, typeof(bool), SigBasic))
        {
            pre = impl.CreateDelegate<Func<CpuContext, IMemory, bool>>();
        }
        else if (Matches(impl, typeof(void), SigBasic))
        {
            var direct = impl.CreateDelegate<Action<CpuContext, IMemory>>();
            pre = (c, m) =>
            {
                direct(c, m);
                return true;
            };
        }
        else
        {
            Console.Error.WriteLine($"[Mods] {mod.Id}: invalid pre hook signature on {Describe(impl)}");
            return false;
        }

        lock (_gate)
        {
            if (Refused(mod, target)) return false;
            var hooks = Get(target);
            hooks.Pres = Insert(hooks.Pres, new Entry<Func<CpuContext, IMemory, bool>> { Mod = mod, Fn = pre, Name = Describe(impl), Profile = HookSection(impl, "pre"), Order = order });
        }

        return true;
    }

    public static bool AddPost(ModInfo mod, MethodInfo target, MethodInfo impl, int order = 0)
    {
        if (!Matches(impl, typeof(void), SigBasic))
        {
            Console.Error.WriteLine($"[Mods] {mod.Id}: invalid post hook signature on {Describe(impl)}");
            return false;
        }

        var post = impl.CreateDelegate<Action<CpuContext, IMemory>>();
        lock (_gate)
        {
            if (Refused(mod, target)) return false;
            var hooks = Get(target);
            hooks.Posts = Insert(hooks.Posts, new Entry<Action<CpuContext, IMemory>> { Mod = mod, Fn = post, Name = Describe(impl), Profile = HookSection(impl, "post"), Order = order });
        }

        return true;
    }

    public static void RemoveMod(ModInfo mod)
    {
        lock (_gate)
        {
            foreach (var target in _hooks.Keys.ToArray())
            {
                var hooks = _hooks[target];
                hooks.Pres = hooks.Pres.Where(p => p.Mod != mod).ToArray();
                hooks.Posts = hooks.Posts.Where(p => p.Mod != mod).ToArray();
                if (hooks.ReplaceOwner == mod)
                {
                    hooks.Replace = null;
                    hooks.ReplaceOwner = null;
                }

                if (hooks.Empty)
                {
                    hooks.Hook?.Dispose();
                    hooks.Hook = null;
                    _hooks.Remove(target);
                }
            }
        }
    }

    public static bool IsRegistered(MethodInfo target)
    {
        lock (_gate)
            return _hooks.ContainsKey(target);
    }

    public static bool IsCommitted(MethodInfo target)
    {
        lock (_gate)
            return _hooks.TryGetValue(target, out var hooks) && hooks.Hook != null;
    }

    //Installing a detour is the one step here that is not bookkeeping, and it is
    //the one that can throw. It used to throw straight out of this loop, which
    //abandoned every function after it in the dictionary -- silently, because a
    //caller hooking from an event listener has its exception swallowed by
    //Event.Dispatch. A mod losing one hook is a mod losing one hook; a mod losing
    //the rest of its hooks because of it is a different bug every time. Each
    //function is therefore committed on its own, and a failure is named.
    public static void Commit()
    {
        lock (_gate)
        {
            foreach (var (target, hooks) in _hooks)
            {
                if (hooks.Hook != null) continue;
                var state = hooks;
                // 0027. Upstream installs each detour with no guard, so the
                // first `new Hook` that throws abandons every function after it
                // in the dictionary -- silently, because the exception is
                // swallowed by Event.Dispatch into one stderr line.
                try
                {
                    hooks.Hook = new Hook(target,
                        (Action<Action<CpuContext, IMemory>, CpuContext, IMemory>)
                        ((orig, c, m) => Invoke(state, orig, c, m)));
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine(
                        $"[Mods] could not hook {target.DeclaringType?.Name}.{target.Name}: {e.Message}");
                }
            }
        }
    }

    private static void Invoke(FunctionHooks hooks, Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (Profiler.Recording)
        {
            InvokeProfiled(hooks, orig, c, m);
            return;
        }

        var pres = hooks.Pres;
        var posts = hooks.Posts;
        var replace = hooks.Replace;
        var owner = hooks.ReplaceOwner;

        var skip = false;
        for (var i = 0; i < pres.Length; i++)
            if (!RunPre(hooks, pres[i], c, m))
                skip = true;
        if (!skip)
        {
            if (replace != null) RunReplace(hooks, replace, owner, orig, c, m);
            else orig(c, m);
        }

        for (var i = 0; i < posts.Length; i++)
            RunPost(hooks, posts[i], c, m);
    }

    //0108. A hook that throws is turned off, not the game. Every port patch and
    //every mod is a set of hooks here, and an exception out of one went straight
    //into the recompiled caller and ended the session -- for C# that only stands
    //in for a routine the game still has. Now the hook's whole mod (all of its
    //hooks, so none is left half-working) is taken off every function for the
    //rest of the session, a fault report is written, the player is told once,
    //and the call goes on: a pre as if it had let the routine run, a post as if
    //it had returned, a replacement by running the game's own routine -- unless
    //the replacement had already called it, which is counted, so it never runs
    //twice. Only an exception thrown by the hook's own code counts: one that came
    //up out of the game's code through it (Blame) is the game's crash, and is
    //marked so that no hook further out claims it either. What a hook wrote
    //before it threw stays written.
    private static bool RunPre(FunctionHooks hooks, Entry<Func<CpuContext, IMemory, bool>> pre, CpuContext c, IMemory m)
    {
        try
        {
            return pre.Fn(c, m);
        }
        catch (Exception e) when (Containable(e))
        {
            if (!Contain(hooks, pre.Mod, "pre", pre.Name, e)) throw;
            return true;
        }
    }

    private static void RunPost(FunctionHooks hooks, Entry<Action<CpuContext, IMemory>> post, CpuContext c, IMemory m)
    {
        try
        {
            post.Fn(c, m);
        }
        catch (Exception e) when (Containable(e))
        {
            if (!Contain(hooks, post.Mod, "post", post.Name, e)) throw;
        }
    }

    private static void RunReplace(FunctionHooks hooks, Action<Action<CpuContext, IMemory>, CpuContext, IMemory> replace,
                                   ModInfo? owner, Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        var counted = ReferenceEquals(hooks.OrigSeen, orig) ? hooks.OrigCounted! : Count(hooks, orig);
        var before = hooks.OrigRuns;
        try
        {
            replace(counted, c, m);
        }
        catch (Exception e) when (Containable(e))
        {
            if (owner == null || !Contain(hooks, owner, "replace", hooks.ReplaceName, e)) throw;
            if (hooks.OrigRuns == before) orig(c, m);
        }
    }

    //The detour hands over the same trampoline every call, so the counting
    //wrapper is made once per function; a detour that did not would cost an
    //allocation a call, which is said once.
    private static Action<CpuContext, IMemory> Count(FunctionHooks hooks, Action<CpuContext, IMemory> orig)
    {
        if (hooks.OrigSeen != null && ++hooks.OrigMisses == 64)
            Console.Error.WriteLine($"[Mods] {hooks.Name}: the detour's original changes from call to call");
        hooks.OrigSeen = orig;
        return hooks.OrigCounted = (c, m) =>
        {
            hooks.OrigRuns++;
            orig(c, m);
        };
    }

    private const string DecidedKey = "RecompOne.HookManager.Decided";
    private static readonly HashSet<ModInfo> _turnedOff = [];
    private static readonly List<string> _faults = [];

    /// <summary>A mod was turned off for throwing: the mod, which hook, the exception.</summary>
    public static event Action<ModInfo, string, Exception>? Faulted;

    /// <summary>Mark an exception as the game's, so no hook it passes through is
    /// turned off for it: for a port's deliberate stop, and for tests.</summary>
    public static Exception NotAHookFault(Exception e)
    {
        e.Data[DecidedKey] = "game";
        return e;
    }

    public static bool IsTurnedOff(ModInfo mod)
    {
        lock (_gate)
            return _turnedOff.Contains(mod);
    }

    public static string DescribeFaults()
    {
        lock (_faults)
            return _faults.Count == 0 ? "(none)" : string.Join('\n', _faults);
    }

    //Control flow, not faults: a hard reset and a guest thread's exit unwind
    //through hooks on purpose.
    private static bool Containable(Exception e)
    {
        return e is not (HardResetSignal or Bios.BiosB.ThreadGone or OutOfMemoryException
            or ThreadInterruptedException or InsufficientExecutionStackException);
    }

    private static bool Contain(FunctionHooks hooks, ModInfo mod, string kind, string impl, Exception e)
    {
        try
        {
            if (e.Data[DecidedKey] != null) return false;
            if (!Blame(e))
            {
                e.Data[DecidedKey] = "game";
                return false;
            }

            e.Data[DecidedKey] = mod.Id;
        }
        catch
        {
            return false;
        }

        TurnOff(mod);
        var where = $"{mod.Id}: {kind} {impl} on {hooks.Name}";
        lock (_faults)
            _faults.Add($"{DateTime.Now:HH:mm:ss} {where}: {e.GetType().Name}: {e.Message}");
        Console.Error.WriteLine($"[Mods] {where} threw, and {mod.Id} is turned off for this session: {e}");
        var report = CrashReport.Write("fault", e,
            $"{where} threw; {mod.Id} is turned off for the rest of the session and the game's own routine runs in its place");
        Runtime.ShowNotice($"\"{mod.Name}\" failed and has been turned off until the game restarts; the game's own " +
                           "routine is used in its place." +
                           (report == null ? "" : $" A report was saved to {Path.GetFullPath(report)}."));
        try
        {
            Faulted?.Invoke(mod, where, e);
        }
        catch (Exception listener)
        {
            Console.Error.WriteLine($"[Mods] a Faulted listener threw: {listener.Message}");
        }

        return true;
    }

    //Whose code threw: the first frame, from the throw outwards, that is either
    //the game's recompiled code (namespace Recompiled, or a detour's dynamic copy
    //of it, which has no type) or anything that is not the runtime or a library.
    //The runtime's own frames say nothing either way: a hook calling the runtime
    //wrongly and the game doing so look the same there. Reaching the catch with
    //neither means the hook called the runtime directly: the hook's.
    private static bool Blame(Exception e)
    {
        foreach (var frame in new StackTrace(e, false).GetFrames())
        {
            var method = frame.GetMethod();
            if (method == null) return false;
            var type = method.DeclaringType;
            if (type == null) return false;
            var ns = type.Namespace ?? "";
            if (ns == "Recompiled" || ns.StartsWith("Recompiled.", StringComparison.Ordinal)) return false;
            if (ns.StartsWith("RecompOne.", StringComparison.Ordinal) || ns == "System" ||
                ns.StartsWith("System.", StringComparison.Ordinal) || ns.StartsWith("Microsoft.", StringComparison.Ordinal) ||
                ns.StartsWith("MonoMod", StringComparison.Ordinal) || ns.StartsWith("Mono.", StringComparison.Ordinal))
                continue;
            return true;
        }

        return true;
    }

    private static void TurnOff(ModInfo mod)
    {
        lock (_gate)
        {
            _turnedOff.Add(mod);
            foreach (var hooks in _hooks.Values)
            {
                hooks.Pres = hooks.Pres.Where(p => p.Mod != mod).ToArray();
                hooks.Posts = hooks.Posts.Where(p => p.Mod != mod).ToArray();
                if (hooks.ReplaceOwner != mod) continue;
                hooks.Replace = null;
                hooks.ReplaceOwner = null;
            }
        }
    }

    //A mod turned off stays off: one that hooks again on the next overlay load
    //would otherwise come back and throw again.
    private static bool Refused(ModInfo mod, MethodInfo target)
    {
        if (!_turnedOff.Contains(mod)) return false;
        Console.Error.WriteLine($"[Mods] {mod.Id}: not hooking {target.Name}, it was turned off this session");
        return true;
    }

    //0045. The same calls as Invoke, each inside a profiler section: the hooked
    //function as a whole (whose self time is the recompiled body), and every
    //delegate on its own, named for the method that implements it. The finally is
    //what keeps an exception thrown through a hook -- a hard reset unwinding the
    //game -- from leaving sections open under the frame for good.
    private static void InvokeProfiled(FunctionHooks hooks, Action<CpuContext, IMemory> orig, CpuContext c,
                                       IMemory m)
    {
        var pres = hooks.Pres;
        var posts = hooks.Posts;
        var replace = hooks.Replace;

        var fn = Profiler.Begin(hooks.Profile);
        try
        {
            var owner = hooks.ReplaceOwner;
            var skip = false;
            for (var i = 0; i < pres.Length; i++)
            {
                var t = Profiler.Begin(pres[i].Profile);
                if (!RunPre(hooks, pres[i], c, m))
                    skip = true;
                Profiler.End(t);
            }

            if (!skip)
            {
                if (replace != null)
                {
                    var t = Profiler.Begin(hooks.ReplaceProfile);
                    RunReplace(hooks, replace, owner, orig, c, m);
                    Profiler.End(t);
                }
                else orig(c, m);
            }

            for (var i = 0; i < posts.Length; i++)
            {
                var t = Profiler.Begin(posts[i].Profile);
                RunPost(hooks, posts[i], c, m);
                Profiler.End(t);
            }
        }
        finally
        {
            Profiler.End(fn);
        }
    }

    private static int HookSection(MethodInfo impl, string kind)
    {
        return Profiler.Register($"{kind} {impl.DeclaringType?.Name}.{impl.Name}", ProfileGroup.Hook);
    }

    //0070. After every entry of the same or a lower order.
    private static Entry<T>[] Insert<T>(Entry<T>[] list, Entry<T> entry)
    {
        var at = list.Length;
        while (at > 0 && list[at - 1].Order > entry.Order) at--;
        return [.. list[..at], entry, .. list[at..]];
    }

    private static FunctionHooks Get(MethodInfo target)
    {
        if (!_hooks.TryGetValue(target, out var hooks))
        {
            _hooks[target] = hooks = new FunctionHooks { Name = $"{target.DeclaringType?.Name}.{target.Name}" };
            hooks.Profile = Profiler.Register(Profiler.FunctionName(target), ProfileGroup.Game);
        }

        return hooks;
    }

    private static bool Matches(MethodInfo impl, Type ret, Type[] args)
    {
        if (impl.ReturnType != ret) return false;
        var pars = impl.GetParameters();
        if (pars.Length != args.Length) return false;
        for (var i = 0; i < pars.Length; i++)
            if (pars[i].ParameterType != args[i])
                return false;
        return true;
    }

    private static string Describe(MethodInfo impl)
    {
        return $"{impl.DeclaringType?.Name}.{impl.Name}";
    }
}