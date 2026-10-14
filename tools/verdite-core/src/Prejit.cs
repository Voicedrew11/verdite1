using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Verdite.Core;

/// <summary>
/// Compile the recompiled code before the game runs it, on background threads,
/// so walking into an area does not stop to JIT it.
///
///     {Tag}_PREJIT=0          leave every method to be compiled on first call
///     {Tag}_PREJIT_PROBE=1    a line per batch as the pass reaches its end
///     {Tag}_PREJIT_THREADS=n  threads for the pass (a quarter of the cores, 1-4)
///
/// ## The defect
///
/// `TieredCompilationQuickJit` is off and must stay off -- tier-up recompiles a
/// hooked method and MonoMod's detour does not follow -- so **every** recompiled
/// function, patch and runtime method is compiled by the full optimizing JIT the
/// first time it is called, on the thread that called it. An area change is the
/// largest batch of first calls the game makes, and it makes them inside one
/// frame. The port's docs carry the measurements.
///
/// ## Preparing rather than tiering
///
/// <c>RuntimeHelpers.PrepareMethod</c> compiles a body without calling it, and
/// the set to compile is already enumerable: every overlay is registered before
/// the first <c>Dispatcher.Load</c>, so <c>Functions</c> holds a delegate per
/// recompiled function and <c>Delegate.Method</c> is the method the JIT would
/// compile later. The port's own assembly -- the patches and Verdite Core, which
/// compiles into it as source -- and the runtime go in the same pass.
///
/// Also taken: **constructors**, which <c>GetMethods</c> does not return (a type's
/// first use compiled its <c>.cctor</c> on the game thread), and **the library's
/// generics closed over the port's value types**, as far as the port's fields
/// name them.
///
/// **The thread starts at the first overlay load, not at the area's.** The load
/// returns straight into the first frame of the new area, so preparing the
/// incoming module from its own <c>OverlayLoadedEvent</c> would race the frames
/// it is meant to protect. It starts during <c>main</c>, which leaves the intro
/// and the title to cover the whole assembly, and the area modules are warmed
/// first because they are what hitches.
///
/// Several threads take from the head of one ordered list, a quarter of the cores
/// (at most four) by default. Lowest priority and a yield every 32 methods, so a
/// machine with few cores spends its cores on the picture; background, so it
/// cannot hold up an exit. A method <see cref="HookManager"/> has already committed
/// is left alone -- MonoMod prepares its target when it installs the detour.
/// </summary>
public static class Prejit
{
    /// <summary>Methods between two yields. Lowest priority is what actually
    /// keeps this off the game thread's back; the yield is for a host whose
    /// scheduler will not preempt on priority alone.</summary>
    const int YieldEvery = 32;

    public static bool Enabled { get; private set; } = true;

    static bool _probe;
    static string _portNamespace = "";
    static Thread[]? _threads;
    static (int Batch, MethodBase Method)[]? _work;
    static string[] _labels = [];
    static int[] _remaining = [], _batchPrepared = [];
    static Stopwatch? _clock;
    static int _next, _finished, _prepared, _left, _refused;

    /// <summary>Threads the pass runs on: a quarter of the cores, at most four.
    /// The port's <c>{Tag}_PREJIT_THREADS</c> overrides it, through
    /// <see cref="Configure"/>.</summary>
    public static int Threads { get; private set; } = Math.Clamp(Environment.ProcessorCount / 4, 1, 4);

    /// <summary>Methods compiled by the pass, and whether it has finished. Read
    /// by the probe line; nothing gates on them.</summary>
    public static int Prepared { get; private set; }

    public static bool Done { get; private set; }

    /// <summary>The port's switches, read by its <c>Program.cs</c> from
    /// <c>Game.Env</c>; a null or blank value leaves the default.</summary>
    public static void Configure(string? enabled, string? probe, string? threads)
    {
        if (!string.IsNullOrWhiteSpace(enabled)) Enabled = enabled != "0";
        if (!string.IsNullOrWhiteSpace(probe)) _probe = probe != "0";
        if (int.TryParse(threads, out int n) && n > 0) Threads = Math.Min(n, Environment.ProcessorCount);
    }

    /// <summary>
    /// Arm the pass. Deferred to the first <see cref="OverlayLoadedEvent"/>
    /// because that is the first moment the dispatcher's registry is populated,
    /// and installed last in Program.cs so the patches' own attach listeners have
    /// run -- and their hooks committed -- before the first method is prepared.
    /// </summary>
    /// <param name="portNamespace">The port's method namespace (<c>Kf2</c>,
    /// <c>Kf3</c>): the patches sit there, in the same assembly as core.</param>
    public static void Install(string portNamespace)
    {
        _portNamespace = portNamespace;

        if (!Enabled)
        {
            Console.WriteLine($"[{Game.Tag}] prejit: off, every method compiles on its first call");
            return;
        }

        Event.AddListener<OverlayLoadedEvent>(_ => Start());
    }

    static void Start()
    {
        if (_threads != null) return;
        Order();
        _clock = Stopwatch.StartNew();
        _threads = new Thread[Threads];
        for (int i = 0; i < _threads.Length; i++)
        {
            _threads[i] = new Thread(Warm)
            {
                IsBackground = true,
                Priority = ThreadPriority.Lowest,
                Name = Game.Id + "-prejit",
            };
            _threads[i].Start();
        }
        Console.WriteLine($"[{Game.Tag}] prejit: warming the recompiled code on {_threads.Length} background thread(s)");
    }

    /// <summary>The whole pass, in order, flattened up front so that several
    /// threads can take from the head of it: reflection over the assemblies is
    /// milliseconds, the compile is seconds. Each item keeps its batch, so the
    /// probe can say when a batch is done.</summary>
    static void Order()
    {
        var seen = new HashSet<RuntimeMethodHandle>();
        var labels = new List<string>();
        var work = new List<(int, MethodBase)>();
        foreach (var (label, methods) in Batches())
        {
            int batch = labels.Count;
            labels.Add(label);
            foreach (var mi in methods)
                if (mi != null && seen.Add(mi.MethodHandle)) work.Add((batch, mi));
        }

        _labels = labels.ToArray();
        _work = work.ToArray();
        _remaining = new int[_labels.Length];
        _batchPrepared = new int[_labels.Length];
        foreach (var (batch, _) in _work) _remaining[batch]++;
    }

    static void Warm()
    {
        var work = _work!;
        int step = 0;

        for (int i; (i = Interlocked.Increment(ref _next) - 1) < work.Length;)
        {
            var (batch, mi) = work[i];
            switch (Prepare(mi))
            {
                case Verdict.Prepared:
                    Interlocked.Increment(ref _prepared);
                    Interlocked.Increment(ref _batchPrepared[batch]);
                    break;
                case Verdict.LeftAlone: Interlocked.Increment(ref _left); break;
                default: Interlocked.Increment(ref _refused); break;
            }

            // A batch is reported by the thread that finishes its last method,
            // so the time is when the batch was done, not what it cost.
            if (Interlocked.Decrement(ref _remaining[batch]) == 0 && _probe)
                Console.WriteLine($"[{Game.Tag}] prejit: {_labels[batch]} {_batchPrepared[batch]} method(s) done by " +
                                  $"{_clock!.Elapsed.TotalMilliseconds:0} ms");

            if (++step % YieldEvery == 0) Thread.Yield();
        }

        if (Interlocked.Increment(ref _finished) != _threads!.Length) return;
        Prepared = _prepared;
        Done = true;
        Console.WriteLine($"[{Game.Tag}] prejit: {_prepared} method(s) compiled in " +
                          $"{_clock!.Elapsed.TotalMilliseconds:0} ms on {_threads.Length} thread(s), " +
                          $"{_left} left to their hook" + (_refused > 0 ? $", {_refused} refused" : ""));
    }

    enum Verdict { Prepared, LeftAlone, Refused }

    static Verdict Prepare(MethodBase mi)
    {
        // A generic definition has no code until it is instantiated, and an
        // abstract or extern method has no body at all; PrepareMethod throws on
        // both rather than reporting it.
        if (mi.IsAbstract || mi.IsGenericMethodDefinition || mi.ContainsGenericParameters)
            return Verdict.LeftAlone;

        if (mi is MethodInfo m && HookManager.IsCommitted(m)) return Verdict.LeftAlone;

        try
        {
            // A method of a closed generic type needs its owner's arguments to
            // name the instantiation it compiles.
            var owner = mi.DeclaringType;
            if (owner is { IsGenericType: true })
                RuntimeHelpers.PrepareMethod(mi.MethodHandle,
                    [.. owner.GetGenericArguments().Select(static a => a.TypeHandle)]);
            else
                RuntimeHelpers.PrepareMethod(mi.MethodHandle);
            return Verdict.Prepared;
        }
        catch
        {
            // Nothing here is load-bearing: a method that will not prepare is a
            // method that compiles on its first call, which is where it was.
            return Verdict.Refused;
        }
    }

    /// <summary>
    /// Ordered by what a walk into an area needs, because the pass is racing the
    /// player: the area modules (small, so the lot is a fraction of a second),
    /// then the port's own replace hooks and the runtime they spend their time in
    /// -- which is what the first frame of an area is -- then the generics those
    /// fields close, then <c>game</c>, which holds the stages and the renderer and
    /// is the bulk. <c>open</c> and <c>end</c> are last: the title has already run
    /// by the time the pass reaches it, and the ending is far away.
    /// </summary>
    static IEnumerable<(string Label, IEnumerable<MethodBase?> Methods)> Batches()
    {
        var overlays = Dispatcher.Overlays;
        var names = overlays.Keys.ToList();
        names.Sort(static (a, b) => Rank(a) != Rank(b)
            ? Rank(a) - Rank(b)
            : string.CompareOrdinal(a, b));

        foreach (var name in names)
        {
            if (Rank(name) > 0) break;
            yield return (name, Functions(overlays, name));
        }

        yield return ("patches", PortMethods());
        yield return ("runtime", AssemblyMethods(typeof(HookManager).Assembly, null));
        yield return ("generics", Generics());

        foreach (var name in names)
        {
            if (Rank(name) == 0) continue;
            yield return (name, Functions(overlays, name));
        }
    }

    static IEnumerable<MethodBase?> Functions(IReadOnlyDictionary<string, IOverlay> overlays, string name)
    {
        IReadOnlyDictionary<uint, Action<CpuContext, IMemory>>? fns = null;
        try { fns = overlays[name].Functions; }
        catch { }
        return fns == null ? [] : fns.Values.Select(static f => f.Method);
    }

    static int Rank(string name) =>
        name.StartsWith("fdat", StringComparison.OrdinalIgnoreCase) ? 0
        : name.Equals("game", StringComparison.OrdinalIgnoreCase) ? 1
        : name.Equals("main", StringComparison.OrdinalIgnoreCase) ? 2
        : 3;

    /// <summary>`generated/` and `patches/` compile into one assembly, so the
    /// recompiled functions above and these come out of the same place; the
    /// namespace is what separates them. Verdite Core's source compiles into the
    /// same assembly, so its namespace is warmed here as well, after the port's.</summary>
    static IEnumerable<MethodBase?> PortMethods()
        => AssemblyMethods(typeof(Prejit).Assembly, _portNamespace)
            .Concat(AssemblyMethods(typeof(Prejit).Assembly, "Verdite.Core"));

    /// <summary>
    /// The library's generic types as the port's own fields close them over a
    /// value type -- `Dictionary&lt;(uint, uint, Family), Mesh&gt;`, `List&lt;Vertex&gt;` --
    /// which share no code with any other instantiation and are not precompiled,
    /// so they compiled on the game thread at the first area. A field is the part
    /// of this that can be enumerated; a local's or a LINQ chain's instantiation
    /// still compiles on its first call.
    /// </summary>
    static IEnumerable<MethodBase?> Generics()
    {
        var closed = new HashSet<Type>();
        foreach (var asm in new[] { typeof(Prejit).Assembly, typeof(HookManager).Assembly })
        {
            Type?[] types;
            try { types = asm.GetTypes(); }
            catch (ReflectionTypeLoadException e) { types = e.Types; }
            foreach (var t in types)
            {
                if (t == null || t.ContainsGenericParameters) continue;
                FieldInfo[] fields;
                try { fields = t.GetFields(All); }
                catch { continue; }
                foreach (var f in fields) Collect(f.FieldType, closed);
            }
        }

        foreach (var g in closed)
        {
            MethodBase[] methods;
            try { methods = [.. g.GetMethods(All), .. g.GetConstructors(All)]; }
            catch { continue; }
            foreach (var mi in methods) yield return mi;
        }
    }

    static void Collect(Type t, HashSet<Type> closed)
    {
        while (t.HasElementType) t = t.GetElementType()!;
        if (!t.IsGenericType || t.ContainsGenericParameters) return;
        var args = t.GetGenericArguments();
        if (t.Assembly != typeof(Prejit).Assembly && t.Assembly != typeof(HookManager).Assembly
            && args.Any(static a => a.IsValueType) && !closed.Add(t))
            return;
        foreach (var a in args) Collect(a, closed);
    }

    const BindingFlags All = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                             BindingFlags.Instance | BindingFlags.DeclaredOnly;

    /// <summary>Every method an assembly declares, optionally under one namespace
    /// root. The runtime goes in whole: a replace hook's own time is spent in the
    /// runtime's fast paths, and those are compiled on first call like everything
    /// else.</summary>
    static IEnumerable<MethodBase?> AssemblyMethods(Assembly asm, string? ns)
    {
        Type?[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException e) { types = e.Types; }

        foreach (var t in types)
        {
            if (t == null || t.ContainsGenericParameters) continue;
            if (ns != null && (t.Namespace == null || !t.Namespace.StartsWith(ns, StringComparison.Ordinal)))
                continue;

            // Constructors too, static ones above all: GetMethods leaves them
            // out, and a type's first use then compiled its .cctor on the game
            // thread at the first area.
            MethodBase[] methods;
            try { methods = [.. t.GetMethods(All), .. t.GetConstructors(All)]; }
            catch { continue; }

            foreach (var mi in methods) yield return mi;
        }
    }
}
