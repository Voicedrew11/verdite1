using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Can a primitive be recognised in the previous tick's display list?
///
///     KF2_PACKETMATCH=1   the match census, once a second
///     KF2_PACKETMATCH=2   also list the worst-matching contexts
///
/// ## Why this exists
///
/// Everything the port smooths between logic ticks, it smooths by *table*:
/// <see cref="FrameSmoothing"/> carries the camera, <see cref="ObjectSmoothing"/>
/// carries four model tables plus the entity records. That approach has been
/// incomplete three times running, and each miss was found the same way -- a
/// person noticing a stepping door, a stepping crystal, a drawbridge that turned
/// out not to be a model at all -- then a round trip to find the table it came
/// from. See item 6 in `docs/TODO.md`.
///
/// The alternative is to stop enumerating tables and work one layer down: at
/// `DrawOTag` the frame is a list of finished primitives, and if the *same*
/// primitive can be recognised in the previous tick's list then its screen
/// position can be carried by the tick phase. That would be one hook covering
/// doors, tiles, enemies, the drawbridge and the animated-texture scroll alike.
///
/// **Whether it can be recognised is the whole question, and this file only
/// measures it.** Nothing here writes to game memory and nothing here changes a
/// picture; it prints a line, like every other probe in the port.
///
/// ## Where it measures
///
/// A read-only **pre-hook on `DrawOTag`**, walking the ordering table the same way
/// `Widescreen`'s replacement does. That layer sees the whole packet -- command
/// word, every vertex, every UV, every colour -- and each word's address in the
/// primitive arena, which `RenderPrimEvent` does not: that event carries four
/// screen positions and four flags and nothing else, so the intrinsic key below
/// could not be measured from it without first patching the checkout.
///
/// `HookManager` allows one `Replace` owner per function and any number of
/// pre-hooks, and runs all pres before the replacement, so this composes with
/// Widescreen's replacement and with NoDither's, Perspective's and Subpixel's
/// pairs on the same three addresses.
///
/// ## What a primitive is attributed to
///
/// A key needs to name a *thing in the world*, not a position in the stream, or
/// one extra polygon anywhere shifts every key after it. The identity is already
/// in the arguments of the two routines that submit geometry:
///
/// * `func_80032588` -- the model submitter. Its `a2` is the position pointer,
///   `base + slot*stride + posOff`, which is a stable per-slot name across frames
///   and covers all four model tables at once.
/// * `func_80031950` -- the map-tile submitter. Its `a0` is the tile record
///   address **plus the half offset** (`S0` or `S0+5` in `func_80031B1C`), so it
///   already distinguishes a tile's two drawn halves and needs no counter.
///
/// Which packets belong to which is read off the primitive arena, exactly as
/// <see cref="DrawCensus"/> attributes bytes: the game bumps a `{start,end,cur}`
/// descriptor at `0x8017E0A4` once per polygon, so the packets a call produced are
/// the addresses between its entry and exit `cur`. Everything outside both
/// routines -- the HUD, the 2D arm, the screen tints -- is context `other`, and is
/// reported separately because it is not what would be interpolated.
///
/// ## The three keys, and why three
///
/// The hypothesis most likely to be false is that a primitive's **ordinal** within
/// its object is stable. Back-face culling submits only the faces pointing at you,
/// so as the camera turns the number of polygons a slot emits changes and every
/// ordinal after the change shifts by one -- the key would break exactly when
/// motion is most visible. So the run measures the fallback at the same time:
///
/// * **K1, ordinal** -- `(kind, contextId, index within context)`, the obvious key.
/// * **K2, lerpable** -- K1 matched *and* the same primitive shape (vertex count,
///   textured/gouraud/semi/raw). A match that changed shape cannot be carried.
/// * **K3, intrinsic** -- `(kind, contextId, hash of the command byte, the UVs and
///   the CLUT/texpage)`. A rigid model's face has fixed texture coordinates
///   whatever order it is emitted in, so this key does not care about culling. Its
///   risk is the opposite one -- a corridor of identical tiles reusing a UV set --
///   so **collisions are counted**, because a key that is not unique is not a key.
///
///   **Vertex colours are deliberately not in it.** They were at first, and the
///   key read 0-5% on the world against 92-99% for the ordinal, which is not a
///   weak key but a broken one: the colours are the game's own per-frame shading,
///   recomputed as the camera moves, so a face never hashed the same twice. The
///   HUD, whose colours are constant, was the only thing that matched -- which is
///   what named the cause. What is left is the geometry's identity and nothing
///   about its appearance.
///
///   An untextured primitive therefore has **no intrinsic key at all**, and is
///   counted as such rather than as a miss: `no key` in the report is the fraction
///   of the frame this scheme could never address, and it is part of the answer.
///
/// ## Two kinds of miss, and only one of them matters
///
/// A primitive can go unmatched for two completely different reasons, and averaging
/// them together answers nothing:
///
/// * **Its object was not on screen last tick.** Something walked into view, or a
///   tile entered the visibility grid. No scheme can carry a thing that did not
///   exist to be carried, and drawing it where the game put it is correct.
/// * **Its object *was* on screen and the key still missed.** That is the key
///   breaking -- the back-face-culling case -- and it is fatal, because it happens
///   to a thing the player is already looking at.
///
/// So the report gives K1 and K3 twice: over everything, and over the primitives
/// whose context was present in the previous tick as well (`surviving`). **The
/// second number is the one that decides this design.**
///
/// ## Is there anything to carry? The `pose` column
///
/// A matched primitive's screen displacement is not all animation. Split per
/// context, it is two things:
///
/// * **The context's mean** -- every primitive of an object moving together. That is
///   the camera's motion plus the object's own translation, and both are already
///   carried, by <see cref="FrameSmoothing"/> and <see cref="ObjectSmoothing"/>. A
///   packet smoother that carried this too would add the camera a second time and
///   throw objects ahead of the world on every turn.
/// * **The deviation from that mean** -- primitives of one object moving *relative
///   to each other*. That is pose change, and it is the only thing left for a
///   packet-level smoother to recover.
///
/// So `pose` is the mean deviation, in pixels a tick, over matched primitives whose
/// context held still on average. **If it is ~0 the packet approach cannot animate
/// anything** and the whole design is only a different way of doing what the two
/// table smoothers already do. `posed ctx` is the share of contexts showing any of
/// it, which separates "nothing animates" from "one thing animates a lot".
///
/// ## Reading the report
///
/// A person is playing while this runs, so each line labels itself: `yaw` is the
/// mean turn rate and `move` the mean speed over the window, both read from the
/// addresses <see cref="FrameSmoothing"/> uses. A row therefore says on its face
/// whether it was taken standing, walking or turning, and the row that decides
/// this design is the one with a large `yaw`.
///
/// `disp` is the mean distance a matched primitive moved since the last tick. If
/// that is routinely hundreds of pixels the key is matching the wrong things and
/// the match rate is a lie.
/// </summary>
public static class PacketMatch
{
    /// <summary>libgpu `DrawOTag`, per overlay -- the same three addresses
    /// <see cref="Widescreen"/>, NoDither, Perspective and Subpixel use.</summary>
    static readonly (string Overlay, uint Addr)[] DrawOTagSites =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>`func_80032588`, the model submitter: `a2` is the position
    /// pointer, which names the table slot.</summary>
    const uint ModelSubmit = 0x80032588;

    /// <summary>`func_80031950`, the map-tile submitter: `a0` is the tile record
    /// address plus the half offset.</summary>
    const uint TileSubmit = 0x80031950;

    /// <summary>Points at this frame's `{start, end, current}` arena descriptor.
    /// The same address <see cref="DrawCensus"/> and <see cref="PrimBuffer"/>
    /// read.</summary>
    const uint ActiveDescriptor = 0x8017E0A4;

    // The composed view angle and the player position, for the self-labelling
    // columns. Same addresses as patches/FrameSmoothing.cs.
    const uint ComposedYaw = 0x80199506;   // u16, wrapped to 12 bits
    const uint PosX = 0x801994EC, PosZ = 0x801994F4;

    /// <summary>One turn, in the game's angle units.</summary>
    const int AngleMod = 0x1000;

    const int KindModel = 0, KindTile = 1, KindOther = 2;
    static readonly string[] KindNames = ["models", "map tiles", "other (HUD, 2D)"];

    static bool _measure, _verbose;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.packetmatch",
        Name = "Packet match probe",
        Version = "1.0",
        Description = "How much of a frame can be matched to the previous tick's frame.",
    };

    public static void Configure(string? probe)
    {
        if (string.IsNullOrWhiteSpace(probe) || probe.Equals("0", StringComparison.Ordinal)) return;
        _measure = true;
        _verbose = probe.Equals("2", StringComparison.Ordinal);
    }

    public static void Install()
    {
        if (!_measure) return;

        _windowStart = Now;
        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            // Every module load is an area change as far as this is concerned: the
            // context ids are all new and the player has been put somewhere else,
            // so the previous frame is not a thing to match against.
            Forget();
            if (attached) return;
            attached = true;
            Attach();
        });
    }

    static void Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(PacketMatch);

        // Post before pre throughout: an orphan post is harmless (Leave returns on
        // an empty stack) while an orphan pre would push a frame nothing pops.
        int contexts = 0;
        contexts += Pair(self, ModelSubmit, nameof(PreModel), nameof(PostModel)) ? 1 : 0;
        contexts += Pair(self, TileSubmit, nameof(PreTile), nameof(PostTile)) ? 1 : 0;

        int walks = 0;
        var walk = self.GetMethod(nameof(BeforeDrawOTag), BindingFlags.Public | BindingFlags.Static)!;
        foreach (var (overlay, addr) in DrawOTagSites)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null) continue;
            if (HookManager.AddPre(_self, target, walk)) walks++;
        }

        HookManager.Commit();

        if (walks == 0)
            Console.Error.WriteLine("[KF2] packetmatch: no DrawOTag resolved; nothing will be measured");
        Console.WriteLine($"[KF2] packetmatch: walking {walks} DrawOTag site(s), " +
                          $"{contexts} of 2 context(s) attributed" + (_verbose ? ", verbose" : ""));
    }

    static bool Pair(Type self, uint addr, string pre, string post)
    {
        var target = SymbolRegistry.Resolve("game", null, addr);
        if (target == null)
        {
            Console.Error.WriteLine($"[KF2] packetmatch: no game function at 0x{addr:X8}");
            return false;
        }
        var after = self.GetMethod(post, BindingFlags.Public | BindingFlags.Static)!;
        var before = self.GetMethod(pre, BindingFlags.Public | BindingFlags.Static)!;
        return HookManager.AddPost(_self, target, after) && HookManager.AddPre(_self, target, before);
    }

    // ---------------------------------------------------------------- contexts

    /// <summary>A span of the primitive arena and what was drawing into it.</summary>
    struct Span { public uint Lo, Hi; public int Kind; public uint Id; }

    struct Open { public int Kind; public uint Id; public uint Desc; public uint Entry; }

    static readonly List<Span> _spans = new(1024);
    static readonly Open[] _open = new Open[16];
    static int _depth;

    public static void PreModel(CpuContext c, IMemory m) => Enter(KindModel, c.A2, m);
    public static void PostModel(CpuContext c, IMemory m) => Leave(m);
    public static void PreTile(CpuContext c, IMemory m) => Enter(KindTile, c.A0, m);
    public static void PostTile(CpuContext c, IMemory m) => Leave(m);

    /// <summary>This frame's arena descriptor and its bump pointer, or zero if
    /// there is no arena yet. The two reads <see cref="DrawCensus"/> makes.</summary>
    static (uint Desc, uint Cur) Arena(IMemory m)
    {
        uint desc = m.ReadU32(ActiveDescriptor);
        if (desc == 0) return (0u, 0u);
        uint start = m.ReadU32(desc), end = m.ReadU32(desc + 4u);
        return start == 0 || end <= start ? (0u, 0u) : (desc, m.ReadU32(desc + 8u));
    }

    static void Enter(int kind, uint id, IMemory m)
    {
        if (!_measure) return;
        if (_depth >= _open.Length) { _depth++; return; }
        var a = Arena(m);
        _open[_depth++] = new Open { Kind = kind, Id = id, Desc = a.Desc, Entry = a.Cur };
    }

    static void Leave(IMemory m)
    {
        if (_depth <= 0) return;
        if (_depth > _open.Length) { _depth--; return; }

        var f = _open[--_depth];
        var a = Arena(m);
        // A swapped descriptor means the arena changed under the call, which the
        // two submitters never do -- but the subtraction would be meaningless if
        // they did, so it is dropped rather than guessed at.
        if (a.Cur == 0 || a.Desc != f.Desc || a.Cur <= f.Entry) return;
        _spans.Add(new Span { Lo = f.Entry & RecompOne.Runtime.Runtime.RamWordMask, Hi = a.Cur & RecompOne.Runtime.Runtime.RamWordMask, Kind = f.Kind, Id = f.Id });
    }

    // ------------------------------------------------------------------- walk

    struct Prim
    {
        public uint Addr;
        public int Kind;
        public uint Id;
        public int Ordinal;
        public int Shape;
        public ulong Hash;
        public bool Keyed;
        public float Cx, Cy;
    }

    struct Seen { public int Shape; public ulong Hash; public float Cx, Cy; }

    /// <summary>One matched primitive's tick displacement, kept until its context's
    /// mean is known.</summary>
    struct Move { public int Kind; public uint Id; public float Dx, Dy; }

    static readonly List<Move> _moves = new(2048);

    static readonly List<Prim> _prims = new(2048);
    static Dictionary<(int, uint, int), Seen> _curOrd = new(2048), _prevOrd = new(2048);
    static Dictionary<(int, uint, ulong), Seen> _curInt = new(2048), _prevInt = new(2048);
    static bool _primed;

    static readonly Comparison<Span> ByAddress = (a, b) => a.Lo.CompareTo(b.Lo);

    static readonly Comparison<Prim> ByContextThenAddress = (a, b) =>
        a.Kind != b.Kind ? a.Kind - b.Kind :
        a.Id != b.Id ? a.Id.CompareTo(b.Id) :
        a.Addr.CompareTo(b.Addr);

    /// <summary>
    /// The read-only half of the walk libgpu's `DrawOTag` does, taken before it
    /// runs. Only tick frames are sampled -- <see cref="FramePacing.TickedThisFrame"/>
    /// -- so the comparison is always between two consecutive states of the world
    /// and the numbers read the same at 20, 60 and 144 fps.
    /// </summary>
    public static void BeforeDrawOTag(CpuContext c, IMemory m)
    {
        if (!_measure) return;
        try
        {
            if (!FramePacing.TickedThisFrame) return;
            Walk(c.A0, m);
            Label(m);     // before the compare: the pose split needs to know whether
            Compare();    // the camera held still on this tick
            Report();
        }
        finally
        {
            // The arena is rewound once a frame at the head of stage 13, so the
            // spans belong to the frame that just finished and nothing carries.
            _spans.Clear();
            _depth = 0;
        }
    }

    static void Walk(uint head, IMemory m)
    {
        _prims.Clear();
        _spans.Sort(ByAddress);

        uint mask = RecompOne.Runtime.Runtime.RamWordMask;
        uint addr = head & mask;
        for (int guard = 0; guard < 0x100000; guard++)
        {
            uint header = m.ReadU32(addr);
            uint count = header >> 24;
            if (count > 0) Decode(addr, (int)count, m);

            uint next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = next & mask;
        }

        // The ordering table is sorted by depth, so the walk does not visit
        // packets in the order they were built. The ordinal has to be the
        // submission order to be stable, and within one context that is address
        // order -- the arena is a bump allocator.
        _prims.Sort(ByContextThenAddress);
        int ord = 0;
        for (int i = 0; i < _prims.Count; i++)
        {
            if (i > 0 && (_prims[i].Kind != _prims[i - 1].Kind || _prims[i].Id != _prims[i - 1].Id)) ord = 0;
            var p = _prims[i];
            p.Ordinal = ord++;
            _prims[i] = p;
        }
    }

    /// <summary>Which call built the packet at this address, by the arena spans
    /// recorded across the two submitters.</summary>
    static (int Kind, uint Id) Owner(uint addr)
    {
        int lo = 0, hi = _spans.Count - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            var s = _spans[mid];
            if (addr < s.Lo) hi = mid - 1;
            else if (addr >= s.Hi) lo = mid + 1;
            else return (s.Kind, s.Id);
        }
        return (KindOther, 0u);
    }

    static int CoordX(uint w) { int x = (int)(w & 0x7FF); return (x & 0x400) != 0 ? x - 0x800 : x; }
    static int CoordY(uint w) { int y = (int)((w >> 16) & 0x7FF); return (y & 0x400) != 0 ? y - 0x800 : y; }

    static ulong Mix(ulong h, uint v) { h ^= v; return h * 0x100000001B3ul; }

    static long _skipped;

    /// <summary>
    /// Unpack one ordering-table node the way `Gpu.DrawPolygon` unpacks the FIFO:
    /// bit 28 gouraud, 27 quad, 26 textured, 25 semi-transparent, 24 raw, then per
    /// vertex an optional colour word, a position word and an optional UV word.
    /// Anything that is not a polygon or a rectangle -- lines, texpage and
    /// environment words -- is counted and skipped, because it is not what would
    /// be interpolated.
    /// </summary>
    static void Decode(uint node, int count, IMemory m)
    {
        uint W(int i) => m.ReadU32(node + 4u + (uint)i * 4u);

        uint cmd = W(0);
        int op = (int)(cmd >> 24);
        var (kind, id) = Owner(node);
        // The opcode and its flag bits, and nothing of the colour: see the K3 note
        // above -- the colour is this frame's shading, not the face's identity.
        ulong h = Mix(0xCBF29CE484222325ul, cmd >> 24);

        if (op >= 0x20 && op <= 0x3F)
        {
            bool gouraud = (cmd & (1u << 28)) != 0, quad = (cmd & (1u << 27)) != 0;
            bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
            int n = quad ? 4 : 3;

            int idx = 1;
            long sx = 0, sy = 0;
            for (int i = 0; i < n; i++)
            {
                if (gouraud && i > 0)
                {
                    if (idx >= count) { _skipped++; return; }
                    idx++;   // a colour word, skipped rather than hashed
                }
                if (idx >= count) { _skipped++; return; }
                uint vw = W(idx++);
                sx += CoordX(vw); sy += CoordY(vw);
                if (tex)
                {
                    if (idx >= count) { _skipped++; return; }
                    uint uvw = W(idx++);
                    // Only the low half is a texture coordinate. The high half is
                    // the CLUT on the first vertex and the texpage on the second,
                    // and on the third and fourth it is **pad** -- PSY-Q does not
                    // clear it, and the arena hands out memory two frames stale, so
                    // hashing it made the key differ from itself with the camera
                    // standing still. `Gpu.DrawPolygon` reads exactly these three.
                    h = Mix(h, uvw & 0xFFFFu);
                    if (i <= 1) h = Mix(h, uvw >> 16);
                }

            }

            _prims.Add(new Prim
            {
                Addr = node, Kind = kind, Id = id,
                Shape = n | (tex ? 16 : 0) | (gouraud ? 32 : 0) | (semi ? 64 : 0) | (raw ? 128 : 0),
                Hash = h, Keyed = tex, Cx = (float)sx / n, Cy = (float)sy / n,
            });
            return;
        }

        if (op >= 0x60 && op <= 0x7F)
        {
            bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
            int size = (int)((cmd >> 27) & 3u);

            int idx = 1;
            if (idx >= count) { _skipped++; return; }
            uint xy = W(idx++);
            if (tex)
            {
                if (idx >= count) { _skipped++; return; }
                h = Mix(h, W(idx++));
            }
            if (size == 0)
            {
                if (idx >= count) { _skipped++; return; }
                h = Mix(h, W(idx++));   // width and height: the sprite's own size
            }
            else h = Mix(h, (uint)size);

            _prims.Add(new Prim
            {
                Addr = node, Kind = kind, Id = id,
                Shape = 2 | (tex ? 16 : 0) | (semi ? 64 : 0) | (raw ? 128 : 0),
                Hash = h, Keyed = tex, Cx = CoordX(xy), Cy = CoordY(xy),
            });
            return;
        }

        _skipped++;
    }

    // ---------------------------------------------------------------- compare

    // Per kind: primitives, K1 matched, K2 (matched and same shape), K3 matched,
    // K3 keys that were not unique, and the summed displacement of the matched.
    static readonly long[] _tot = new long[3], _k1 = new long[3], _k2 = new long[3];
    static readonly long[] _k3 = new long[3], _coll = new long[3], _keyed = new long[3];

    // The same census again, restricted to primitives whose context was drawn last
    // tick too. See "Two kinds of miss" above: this is the number that decides it.
    static readonly long[] _sTot = new long[3], _sK1 = new long[3], _sK3 = new long[3], _sKeyed = new long[3];

    // The pose split: how much of a matched primitive's motion is left once its
    // context's mean is taken out. See "Is there anything to carry?" above.
    static readonly double[] _poseSum = new double[3], _meanSum = new double[3];
    static readonly long[] _poseN = new long[3], _ctxN = new long[3], _ctxPosed = new long[3];
    static long _poseTicks;

    // Of the primitives K1 matched, how many matched a face with the same texture
    // coordinates -- that is, the same face. See the note in Compare.
    static readonly long[] _k1Right = new long[3], _k1Keyed = new long[3];
    static readonly double[] _disp1 = new double[3], _disp3 = new double[3];
    static long _ticks, _contexts, _contextsMatched;

    static readonly Dictionary<(int, uint), int> _ctxSeen = new(512);

    /// <summary>Which contexts drew anything last tick, so "this object is new" can
    /// be told from "the key broke on an object that was already there".</summary>
    static HashSet<(int, uint)> _curCtx = new(512), _prevCtx = new(512);

    /// <summary>KF2_PACKETMATCH=2 only: per context, primitives seen and how many
    /// each key matched, so the report can name the contexts that are dragging the
    /// average down rather than only stating it.</summary>
    struct CtxTally { public long Tot, Keyed, K1, K3, Moved; public double Disp; }
    static readonly Dictionary<(int, uint), CtxTally> _ctxTally = new(512);

    static void Compare()
    {
        _curOrd.Clear();
        _curInt.Clear();
        _ctxSeen.Clear();
        _curCtx.Clear();
        _moves.Clear();

        foreach (var p in _prims)
        {
            var seen = new Seen { Shape = p.Shape, Hash = p.Hash, Cx = p.Cx, Cy = p.Cy };
            var kOrd = (p.Kind, p.Id, p.Ordinal);
            var kInt = (p.Kind, p.Id, p.Hash);

            _curOrd[kOrd] = seen;
            if (p.Keyed && !_curInt.TryAdd(kInt, seen)) _coll[p.Kind]++;

            if (!_primed) continue;

            bool survived = _prevCtx.Contains((p.Kind, p.Id));

            _tot[p.Kind]++;
            if (p.Keyed) _keyed[p.Kind]++;
            if (survived) { _sTot[p.Kind]++; if (p.Keyed) _sKeyed[p.Kind]++; }

            double moved = 0.0;
            if (_prevOrd.TryGetValue(kOrd, out var was))
            {
                _k1[p.Kind]++;
                if (survived) _sK1[p.Kind]++;
                // Matching is not the same as matching the *right* face. An ordinal
                // survives a back-face cull dropping a polygon -- the key still
                // exists on both sides -- while naming a different triangle. The
                // texture coordinates are the face's own identity, so agreeing with
                // them is what makes a match true.
                if (p.Keyed) { _k1Keyed[p.Kind]++; if (was.Hash == p.Hash) _k1Right[p.Kind]++; }
                if (was.Shape == p.Shape) _k2[p.Kind]++;
                moved = Math.Sqrt((p.Cx - was.Cx) * (p.Cx - was.Cx) + (p.Cy - was.Cy) * (p.Cy - was.Cy));
                _disp1[p.Kind] += moved;
                _moves.Add(new Move { Kind = p.Kind, Id = p.Id, Dx = p.Cx - was.Cx, Dy = p.Cy - was.Cy });
            }
            if (p.Keyed && _prevInt.TryGetValue(kInt, out var wasI))
            {
                _k3[p.Kind]++;
                if (survived) _sK3[p.Kind]++;
                _disp3[p.Kind] += Math.Sqrt((p.Cx - wasI.Cx) * (p.Cx - wasI.Cx) + (p.Cy - wasI.Cy) * (p.Cy - wasI.Cy));
            }

            var ctx = (p.Kind, p.Id);
            if (_ctxSeen.TryAdd(ctx, 0))
            {
                _contexts++;
                if (_prevOrd.ContainsKey((p.Kind, p.Id, 0))) _contextsMatched++;
            }

            if (_verbose)
            {
                _ctxTally.TryGetValue(ctx, out var t);
                t.Tot++;
                if (_prevOrd.ContainsKey(kOrd)) { t.K1++; t.Moved++; t.Disp += moved; }
                if (p.Keyed) { t.Keyed++; if (_prevInt.ContainsKey(kInt)) t.K3++; }
                _ctxTally[ctx] = t;
            }
        }

        PoseSplit();
        foreach (var p in _prims) _curCtx.Add((p.Kind, p.Id));

        if (_primed) _ticks++;
        (_prevOrd, _curOrd) = (_curOrd, _prevOrd);
        (_prevInt, _curInt) = (_curInt, _prevInt);
        (_prevCtx, _curCtx) = (_curCtx, _prevCtx);
        _primed = true;
    }

    /// <summary>
    /// Take each context's mean displacement out of its primitives and measure what
    /// is left. The mean is the object moving as one -- camera and translation, both
    /// already carried elsewhere -- and the remainder is the only thing a packet
    /// smoother could add. Reported only for contexts whose mean is small, so a
    /// model sweeping across the screen does not report its own translation as pose.
    /// </summary>
    static void PoseSplit()
    {
        if (_moves.Count == 0 || !_camStill) return;
        _poseTicks++;
        _moves.Sort(static (a, b) => a.Kind != b.Kind ? a.Kind - b.Kind : a.Id.CompareTo(b.Id));

        int i = 0;
        while (i < _moves.Count)
        {
            int j = i;
            double sx = 0, sy = 0;
            while (j < _moves.Count && _moves[j].Kind == _moves[i].Kind && _moves[j].Id == _moves[i].Id)
            {
                sx += _moves[j].Dx; sy += _moves[j].Dy; j++;
            }
            int n = j - i;
            int kind = _moves[i].Kind;
            double mx = sx / n, my = sy / n;
            double mean = Math.Sqrt(mx * mx + my * my);

            _ctxN[kind]++;
            _meanSum[kind] += mean;

            // The camera is already known to have held still, so the only thing that
            // can still put a rigid object's whole silhouette in motion is the object
            // translating -- which ObjectSmoothing carries. Taking its mean out leaves
            // pose; the bound keeps a slot that was re-placed rather than moved out of
            // the average.
            if (mean <= 32.0)
            {
                double dev = 0;
                for (int k = i; k < j; k++)
                {
                    double ex = _moves[k].Dx - mx, ey = _moves[k].Dy - my;
                    dev += Math.Sqrt(ex * ex + ey * ey);
                }
                _poseSum[kind] += dev;
                _poseN[kind] += n;
                if (dev / n > 0.5) _ctxPosed[kind]++;
            }
            i = j;
        }
    }

    // ------------------------------------------------------------ self-labels

    static int _lastYaw = -1;
    static long _lastX, _lastZ;
    static bool _haveLast;
    static double _yawSum, _moveSum;

    /// <summary>The camera did not move at all on this tick. Only then is a
    /// primitive's deviation from its context's mean unambiguously animation:
    /// with the eye translating, a model's near and far faces move by different
    /// amounts on their own, and that parallax survives the mean and would be
    /// counted as pose. See "Is there anything to carry?".</summary>
    static bool _camStill;

    /// <summary>The turn rate and the walking speed over the window, so a line
    /// taken while the player was spinning says so without anyone having kept
    /// notes beside the log. Turning is the case this probe exists for.</summary>
    static void Label(IMemory m)
    {
        int yaw = m.ReadU16(ComposedYaw) & (AngleMod - 1);
        long x = (int)m.ReadU32(PosX), z = (int)m.ReadU32(PosZ);

        _camStill = false;
        if (_haveLast)
        {
            int d = (yaw - _lastYaw) & (AngleMod - 1);
            if (d > AngleMod / 2) d -= AngleMod;
            _yawSum += Math.Abs(d);
            double dx = x - _lastX, dz = z - _lastZ;
            _moveSum += Math.Sqrt(dx * dx + dz * dz);
            _camStill = d == 0 && dx == 0 && dz == 0;
        }
        _lastYaw = yaw; _lastX = x; _lastZ = z; _haveLast = true;
    }

    /// <summary>Cleared on an area change: every context id is new, the player has
    /// been moved, and a window straddling the two would report a collapse that
    /// says nothing about the design.</summary>
    static void Forget()
    {
        _primed = false;
        _haveLast = false;
        _prevOrd.Clear();
        _prevInt.Clear();
        _prevCtx.Clear();
    }

    // ----------------------------------------------------------------- report

    static double _windowStart;
    static double Now => Environment.TickCount64 / 1000.0;

    static void Report()
    {
        double now = Now;
        if (now - _windowStart < 1.0) return;
        double secs = now - _windowStart;

        long tot = _tot[0] + _tot[1] + _tot[2];
        if (_ticks > 0 && tot > 0)
        {
            double deg = _yawSum / secs * 360.0 / AngleMod;
            Console.WriteLine($"[packetmatch] {_ticks} tick frame(s) in {secs:0.0}s, " +
                              $"{(double)tot / _ticks:0} prim/frame, " +
                              $"yaw {deg:0} deg/s, move {_moveSum / secs:0} u/s, " +
                              $"{_contexts / (double)_ticks:0.0} context/frame " +
                              $"({Pct(_contextsMatched, _contexts)} carried over)");

            for (int k = 0; k < 3; k++)
            {
                if (_tot[k] == 0) continue;
                Console.WriteLine($"[packetmatch]   {KindNames[k],-16} " +
                                  $"{(double)_tot[k] / _ticks,6:0} prim  " +
                                  $"K1 ordinal {Pct(_k1[k], _tot[k])}  " +
                                  $"K2 lerpable {Pct(_k2[k], _tot[k])}  " +
                                  $"K3 intrinsic {Pct(_k3[k], _keyed[k])}  " +
                                  $"collisions {Pct(_coll[k], _keyed[k])}  " +
                                  $"disp {Div(_disp1[k], _k1[k]):0.0}/{Div(_disp3[k], _k3[k]):0.0} px");
                Console.WriteLine($"[packetmatch]   {"K1 correctness",-16} " +
                                  $"{Pct(_k1Right[k], _k1Keyed[k])} of ordinal matches were the same face " +
                                  $"(by texture coordinates)");
                Console.WriteLine($"[packetmatch]   {"pose split",-16} " +
                                  $"{Div(_poseSum[k], _poseN[k]),6:0.00} px/tick left after the context mean " +
                                  $"({Div(_meanSum[k], _ctxN[k]):0.0} px), " +
                                  $"{Pct(_ctxPosed[k], _ctxN[k])} of contexts show any" +
                                  (_poseTicks == 0 ? "  [no still tick this window]" : $"  [{_poseTicks} still tick(s)]"));
                Console.WriteLine($"[packetmatch]   {"in surviving ctx",-16} " +
                                  $"{(double)_sTot[k] / _ticks,6:0} prim  " +
                                  $"K1 ordinal {Pct(_sK1[k], _sTot[k])}  " +
                                  $"{Pct(_tot[k] - _sTot[k], _tot[k])} of the row was newly on screen  " +
                                  $"K3 intrinsic {Pct(_sK3[k], _sKeyed[k])}");
            }

            if (_skipped > 0)
                Console.WriteLine($"[packetmatch]   {(double)_skipped / _ticks:0.0} non-drawing node(s) a frame, not counted");

            if (_verbose && _ctxTally.Count > 0)
            {
                // The contexts dragging the average down, worst first. A context
                // that only appeared for one frame of the window will sit here
                // legitimately -- something walked into view -- so the count is
                // printed beside the rate.
                var worst = _ctxTally
                    .Where(e => e.Value.Tot >= 8)
                    .OrderBy(e => (double)e.Value.K1 / e.Value.Tot)
                    .Take(5);
                foreach (var (ctx, t) in worst)
                    Console.WriteLine($"[packetmatch]     worst {KindNames[ctx.Item1],-16} 0x{ctx.Item2:X8}  " +
                                      $"{t.Tot,5} prim  K1 {Pct(t.K1, t.Tot)}  K3 {Pct(t.K3, t.Keyed)}");

                // What actually moved. Read with the camera standing still -- yaw 0,
                // move 0 -- this names the thing that is animating, by the address
                // that identifies it: a model table slot, or a map tile record and
                // half. It is how "which table is the drawbridge in" gets answered
                // without a screenshot or a guess.
                var movers = _ctxTally
                    .Where(e => e.Value.Moved >= 4 && e.Value.Disp / e.Value.Moved > 0.5)
                    .OrderByDescending(e => e.Value.Disp / e.Value.Moved)
                    .Take(5);
                foreach (var (ctx, t) in movers)
                    Console.WriteLine($"[packetmatch]     moved {KindNames[ctx.Item1],-16} 0x{ctx.Item2:X8}  " +
                                      $"{t.Tot,5} prim  {t.Disp / t.Moved,6:0.0} px/tick  K1 {Pct(t.K1, t.Tot)}");
            }
        }

        Array.Clear(_tot); Array.Clear(_k1); Array.Clear(_k2);
        Array.Clear(_k3); Array.Clear(_coll); Array.Clear(_keyed);
        Array.Clear(_sTot); Array.Clear(_sK1); Array.Clear(_sK3); Array.Clear(_sKeyed);
        Array.Clear(_poseSum); Array.Clear(_meanSum);
        Array.Clear(_poseN); Array.Clear(_ctxN); Array.Clear(_ctxPosed);
        _poseTicks = 0;
        Array.Clear(_k1Right); Array.Clear(_k1Keyed);
        Array.Clear(_disp1); Array.Clear(_disp3);
        _ctxTally.Clear();
        _ticks = 0; _contexts = 0; _contextsMatched = 0; _skipped = 0;
        _yawSum = 0; _moveSum = 0;
        _windowStart = now;
    }

    static string Pct(long hit, long of) => of == 0 ? "  n/a" : $"{100.0 * hit / of,5:0.0}%";
    static double Div(double a, long b) => b == 0 ? 0.0 : a / b;
}
