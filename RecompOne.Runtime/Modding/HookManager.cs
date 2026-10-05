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
        public int Profile;
        public int Order;
    }

    private sealed class FunctionHooks
    {
        public Entry<Func<CpuContext, IMemory, bool>>[] Pres = [];
        public Entry<Action<CpuContext, IMemory>>[] Posts = [];
        public Action<Action<CpuContext, IMemory>, CpuContext, IMemory>? Replace;
        public ModInfo? ReplaceOwner;
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

            hooks.Replace = replace;
            hooks.ReplaceOwner = mod;
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
            var hooks = Get(target);
            hooks.Pres = Insert(hooks.Pres, new Entry<Func<CpuContext, IMemory, bool>> { Mod = mod, Fn = pre, Profile = HookSection(impl, "pre"), Order = order });
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
            var hooks = Get(target);
            hooks.Posts = Insert(hooks.Posts, new Entry<Action<CpuContext, IMemory>> { Mod = mod, Fn = post, Profile = HookSection(impl, "post"), Order = order });
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

        var skip = false;
        for (var i = 0; i < pres.Length; i++)
            if (!pres[i].Fn(c, m))
                skip = true;
        if (!skip)
        {
            if (replace != null) replace(orig, c, m);
            else orig(c, m);
        }

        for (var i = 0; i < posts.Length; i++)
            posts[i].Fn(c, m);
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
            var skip = false;
            for (var i = 0; i < pres.Length; i++)
            {
                var t = Profiler.Begin(pres[i].Profile);
                if (!pres[i].Fn(c, m))
                    skip = true;
                Profiler.End(t);
            }

            if (!skip)
            {
                if (replace != null)
                {
                    var t = Profiler.Begin(hooks.ReplaceProfile);
                    replace(orig, c, m);
                    Profiler.End(t);
                }
                else orig(c, m);
            }

            for (var i = 0; i < posts.Length; i++)
            {
                var t = Profiler.Begin(posts[i].Profile);
                posts[i].Fn(c, m);
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
            _hooks[target] = hooks = new FunctionHooks();
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