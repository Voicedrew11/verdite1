using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using BiosKernel = RecompOne.Runtime.Bios.Bios;

namespace RecompOne.Runtime.Dispatch;

public static class Dispatcher
{
    private static readonly OverlayLoadedEvent _overlayEvent = new();
    private static readonly Dictionary<string, IOverlay> _registry = new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<int, string> _lbaToName = [];
    private static readonly List<string> _active = [];
    private static readonly Dictionary<uint, Action<CpuContext, IMemory>> _funcMap = [];
    private static IOverlay? _pending;

    public static void Register(string name, IOverlay overlay)
    {
        _registry[name] = overlay;
        if (overlay.LbaStart >= 0) _lbaToName[overlay.LbaStart] = name;
    }

    public static string[] ActiveNames
    {
        get
        {
            lock (_active)
            {
                return _active.ToArray();
            }
        }
    }

    public static IReadOnlyDictionary<string, IOverlay> Overlays => _registry;

    public static void LoadByLba(int lba)
    {
        if (!_lbaToName.TryGetValue(lba, out var name)) return;
        var overlay = _registry[name];
        if (overlay.Base == 0)
        {
            Load(name);
            return;
        }

        _pending = overlay;
    }

    public static bool HasPending => _pending != null;

    public static void NotifyWrite(uint phys)
    {
        var p = _pending;
        if (p == null) return;

        PromoteWrite(p, phys);
    }

    private static void PromoteWrite(IOverlay p, uint phys)
    {
        var start = p.Base & 0x1FFFFFFFu;
        if (phys < start || phys >= start + 0x800u) return;
        _pending = null;
        Load(p.Name);
    }

    public static void ClearPending()
    {
        _pending = null;
    }

    public static void Reset()
    {
        _pending = null;
        lock (_active)
        {
            _active.Clear();
        }

        _funcMap.Clear();
    }

    public static void Load(string name)
    {
        if (!_registry.TryGetValue(name, out var overlay))
            throw new KeyNotFoundException($"overlay not registered: {name}");

        bool already;
        lock (_active)
        {
            already = _active.Remove(name);
        }

        if (!already) HandleRegionOverwrites(overlay);

        lock (_active)
        {
            _active.Add(name);
        }

        foreach (var (addr, fn) in overlay.Functions)
            _funcMap[addr] = fn;

        if (already) return;
        Runtime.OverlayLog.Record(name, OverlayEventKind.Loaded);
        Console.WriteLine($"[Dispatcher] loaded overlay: {name}");

        if (Event.HasAnyListeners<OverlayLoadedEvent>())
        {
            var e = _overlayEvent;
            e.Context = Runtime.Cpu!;
            e.Memory = Runtime.Mem!;
            e.Name = name;
            Event.Dispatch(e);
        }
    }

    private static void HandleRegionOverwrites(IOverlay overlay)
    {
        var newStart = overlay.Base & 0x1FFFFFFFu;
        var newEnd = newStart + overlay.Size;
        var hasRegion = overlay.Base != 0 && overlay.Size != 0;

        List<string>? overwritten = null;
        List<(string Name, int Funcs)>? vramCollisions = null;

        lock (_active)
        {
            foreach (var activeName in _active)
            {
                var other = _registry[activeName];
                var otherHasRegion = other.Base != 0 && other.Size != 0;

                if (hasRegion && otherHasRegion)
                {
                    var s = other.Base & 0x1FFFFFFFu;
                    var e = s + other.Size;

                    if (s < newEnd && e > newStart)
                    {
                        // Any overlap, not only full containment. OPEN/GAME/END
                        // all load at the same base and the later executable is
                        // often smaller, so the contained test would leave the
                        // previous one's functions resident. The same hole
                        // shows up when a smaller FDAT module loads over a
                        // larger one. Upstream still tests `s >= newStart &&
                        // e <= newEnd` here; that is the bug 0008 fixed.
                        overwritten ??= [];
                        overwritten.Add(activeName);
                        continue;
                    }
                }

                var shared = CountSharedFunctions(overlay, other);
                if (shared > 0)
                {
                    vramCollisions ??= [];
                    vramCollisions.Add((activeName, shared));
                }
            }

            if (overwritten != null)
                foreach (var d in overwritten)
                    _active.Remove(d);
        }

        if (overwritten != null)
        {
            Rebuild();
            foreach (var d in overwritten)
            {
                Runtime.OverlayLog.Record(d, OverlayEventKind.Overwritten, overlay.Name);
                Console.WriteLine($"[Dispatcher] overlay {d} overwritten by {overlay.Name}");
            }
        }

        if (vramCollisions != null)
            foreach (var (otherName, n) in vramCollisions)
            {
                Runtime.OverlayLog.Record(overlay.Name, OverlayEventKind.VramCollision, $"{otherName} ({n} funcs)");
                Console.WriteLine(
                    $"[Dispatcher] overlay {overlay.Name} vvram colision with {otherName}: {n} functions");
            }
    }

    private static int CountSharedFunctions(IOverlay a, IOverlay b)
    {
        var smaller = a.Functions.Count <= b.Functions.Count ? a : b;
        var larger = ReferenceEquals(smaller, a) ? b : a;

        var n = 0;
        foreach (var addr in smaller.Functions.Keys)
            if (larger.Functions.ContainsKey(addr))
                n++;
        return n;
    }

    public static void TryLoad(string name)
    {
        if (_registry.ContainsKey(name))
            Load(name);
    }

    public static void Unload(string name)
    {
        bool removed;
        lock (_active)
        {
            removed = _active.Remove(name);
        }

        if (!removed) return;
        Rebuild();
        Runtime.OverlayLog.Record(name, OverlayEventKind.Unloaded);
    }

    /// <summary>Is addr the entry point of a function in a resident overlay?</summary>
    public static bool HasFunction(uint addr) => _funcMap.ContainsKey(addr);

    public static bool CanCall(uint addr)
    {
        return addr switch
        {
            0xA0u or 0xB0u or 0xC0u => true,
            _ => (addr & 0xFF000000u) == 0xBFC00000u || _funcMap.ContainsKey(addr) ||
                 _funcMap.ContainsKey(Cached(addr))
        };
    }

    private static uint Cached(uint addr)
    {
        var phys = addr & 0x1FFFFFFFu;
        return phys < 0x00800000u ? 0x80000000u | phys : addr;
    }

    public static bool Tolerant;

    private static readonly HashSet<uint> _reported = [];

    private static bool IsReturnSite(IMemory m, uint addr)
    {
        if (addr < 0x80000008u || (addr & 3u) != 0) return false;

        var w = m.ReadU32(addr - 8);
        var op = w >> 26;
        if (op == 3) return true;
        if (op == 0 && (w & 0x3Fu) == 9) return true;
        return op == 1 && ((w >> 16) & 0x1Fu) is 0x10 or 0x11;
    }

    public static void Call(CpuContext c, IMemory m, uint addr)
    {
        if (BiosKernel.TryDispatch(c, m, addr)) return;

        if (_funcMap.TryGetValue(addr, out var fn))
        {
            fn(c, m);
            return;
        }

        var cached = Cached(addr);
        if (cached != addr && _funcMap.TryGetValue(cached, out fn))
        {
            fn(c, m);
            return;
        }
        
        if (IsReturnSite(m, addr)) return;

        if (!Tolerant) throw new InvalidOperationException($"unmapped call: 0x{addr:X8}");

        lock (_reported)
            if (_reported.Add(addr))
                Console.WriteLine($"[Dispatcher] skipped an unmapped call to 0x{addr:X8}");

        c.V0 = 0u;
    }

    private static void Rebuild()
    {
        _funcMap.Clear();
        lock (_active)
        {
            foreach (var name in _active)
            foreach (var (addr, fn) in _registry[name].Functions)
                _funcMap[addr] = fn;
        }
    }
}