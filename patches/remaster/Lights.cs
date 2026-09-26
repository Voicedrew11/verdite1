using System.Numerics;
using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2.Remaster;

/// <summary>
/// Authored point and spot lights (runtime <c>0071</c>).
///
///     KF2_REMASTER_LIGHTS=0   leave the pack's lights out (they apply with the remaster by default)
///     KF2_REMASTER_SHADOWS=0  no shadows from them (on by default)
///     KF2_REMASTER_SHADOW_MODELS=0  only the map casts (creatures and objects cast too by default)
///     KF2_REMASTER_SHADOW_SIZE=1024 KF2_REMASTER_SHADOW_BIAS=6 KF2_REMASTER_SHADOW_OFFSET=1.5 KF2_REMASTER_SHADOW_SOFT=1.25
///
/// The pack's <c>areas/&lt;n&gt;/lights.json</c>, behind the area's fingerprint. Before
/// each <c>DrawOTag</c> -- the camera block then holds the view the table was built
/// with -- every light is taken into the GTE's view space, culled against the eye and
/// the draw window, the nearest <see cref="RemasterUniforms.MaxLights"/> kept, and
/// the list published for the prim shader, which adds each one to the lit colour
/// before the depth cue. Flicker steps on the world tick, so it holds while the world
/// is paused. Needs per-pixel lighting and the C# assemblers, whose records carry the
/// colour a light is added in. See "Phase 2, the first slice" in docs/REMASTER.md.
///
/// A glowing material also gives off a light: one per tile half and material, at the
/// glowing faces' centroid off their lit side, found once when the area applies, and
/// one per glowing model drawn, each frame. Authored lights take the slots first. See
/// "The glow is a light source" in docs/REMASTER.md.
///
/// The nearest <see cref="RemasterUniforms.MaxShadows"/> authored lights sent that
/// cast shadows each keep a slot while they stay in the list, so the backend draws a
/// light's cubemap (runtime <c>0077</c>) once and again only when it moves. The map
/// casts, from the retained static mesh this asks for while any light wants it, and
/// so do the frame's models, captured by <see cref="Kf2.RetainedModels"/>: a light
/// with a model in reach is drawn again whenever those models move. See "Shadows, the
/// first slice" and "Shadows, the second slice" in docs/REMASTER.md.
/// </summary>
public sealed class Lights : IRemasterFeature
{
    public string Id => "lights";

    static bool _on = true;

    /// <summary>The area's lights, resolved; the shader list is built from these.</summary>
    static Pack.Light[] _lights = [];

    /// <summary>Why the area's lights are not applied, or null.</summary>
    public static string? Refused { get; private set; }

    public static int Authored => _lights.Length;

    /// <summary>The lights the area's glowing tile faces give off.</summary>
    static Pack.Light[] _derived = [];
    public static int Derived => _derived.Length;
    public static IReadOnlyList<Pack.Light> DerivedLights => _derived;

    /// <summary>The last frame's lights from glowing models.</summary>
    public static int ModelLights { get; private set; }

    /// <summary>The frame's candidates: authored, then derived, then models'.</summary>
    static Pack.Light[] _frame = new Pack.Light[64];

    /// <summary>The material a candidate came from, which it does not light; 0 for
    /// an authored light.</summary>
    static byte[] _frameSrc = new byte[64];

    /// <summary>The tile glows' materials, beside <see cref="_derived"/>.</summary>
    static byte[] _derivedSrc = [];

    /// <summary>The last frame's count sent, and culled.</summary>
    public static int Sent { get; private set; }
    public static int Culled { get; private set; }

    /// <summary>Frames the list was published on; never reset.</summary>
    public static long Frames;

    static long _ticks, _tickFrame = -1;

    /// <summary>The world tick the flicker is evaluated at.</summary>
    public static long Ticks => _ticks;

    int _version = -1, _settle = -1, _surfaces = -1, _switches = -1;

    /// <summary>How far past the eye a light may sit and still reach the draw window:
    /// twelve tiles each way.</summary>
    const float Reach = 12f * Identity.TileUnits;

    public static void Configure(string? on)
    {
        if (!string.IsNullOrWhiteSpace(on)) _on = on.Trim() != "0";
    }

    static bool _shadows = true;

    /// <summary>Whether authored lights cast shadows at all.</summary>
    public static bool ShadowsOn => _shadows;

    public static void ConfigureShadows(string? on, string? size, string? bias, string? offset, string? soft, string? models)
    {
        if (!string.IsNullOrWhiteSpace(on)) _shadows = on.Trim() != "0";
        if (!string.IsNullOrWhiteSpace(models)) RetainedScene.ShadowModels = models.Trim() != "0";
        static bool F(string? s, out float v)
            => float.TryParse(s, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v);
        if (int.TryParse(size, out int n) && n >= 64) RemasterUniforms.ShadowSize = Math.Clamp(n, 64, 4096);
        if (F(bias, out float b)) RemasterUniforms.ShadowBias = b;
        if (F(offset, out float o)) RemasterUniforms.ShadowOffset = o;
        if (F(soft, out float k)) RemasterUniforms.ShadowSoft = Math.Max(0f, k);
    }

    static int _shadowSwitches;

    /// <summary>Shadows on or off for the session, from the shell.</summary>
    public static void SetShadows(bool on)
    {
        _shadows = on;
        _shadowSwitches++;
    }

    /// <summary>The light each shadow slot holds, by name.</summary>
    static readonly string?[] _slotName = new string?[RemasterUniforms.MaxShadows];

    public static void Install() => HookAttach.OnOverlayLoad("remaster lights", Attach);

    static readonly (string Overlay, uint Addr)[] DrawOTag =
        [("open", 0x80016078), ("game", 0x80060818), ("end", 0x80013D80)];

    static readonly ModInfo _self = new()
    {
        Id = "kf2.remaster.lights",
        Name = "Remaster lights",
        Version = "1.0",
        Description = "Publishes a pack's authored lights, in view space, before each DrawOTag.",
    };

    static readonly HashSet<string> _hooked = new();

    static bool Attach()
    {
        SymbolRegistry.Build();
        var pre = typeof(Lights).GetMethod(nameof(BeforeDrawOTag), BindingFlags.Public | BindingFlags.Static)!;
        bool queued = false;
        foreach (var (overlay, addr) in DrawOTag)
        {
            if (_hooked.Contains(overlay)) continue;
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null) continue;
            if (HookManager.AddPre(_self, target, pre)) queued = true;
        }
        if (queued) HookManager.Commit();
        foreach (var (overlay, addr) in DrawOTag)
            if (SymbolRegistry.Resolve(overlay, null, addr) is { } t && HookAttach.Installed(t)) _hooked.Add(overlay);
        // GAME.EXE's is the one that draws an area.
        return _hooked.Contains("game");
    }

    public void OnFrame()
    {
        bool want = _on && Host.Enabled && Identity.Settled;
        if (!want)
        {
            if (_lights.Length > 0 || RemasterUniforms.Enabled) Clear();
            _version = _settle = _surfaces = -1;
            return;
        }
        // Derived lights follow the surfaces, which follow the tile table.
        if (_version == Pack.Version && _settle == Identity.Settles && _surfaces == Surfaces.Serial
            && _switches == _shadowSwitches) return;
        _switches = _shadowSwitches;
        _version = Pack.Version;
        _settle = Identity.Settles;
        _surfaces = Surfaces.Serial;
        Resolve();
    }

    public void Detach()
    {
        Clear();
        _version = _settle = _surfaces = -1;
    }

    static void Clear()
    {
        _lights = [];
        _derived = [];
        _derivedSrc = [];
        ModelLights = 0;
        Refused = null;
        RemasterUniforms.Enabled = false;
        RemasterUniforms.Publish(0);
        RetainedScene.ShadowsWanted = false;
        ClearSlots();
        PolyAssembler.KeepForLights = false;
        PolyAssembler.Keep();
        Sent = Culled = 0;
    }

    static void Resolve()
    {
        int area = Identity.Area;
        string? fp = Pack.AreaFingerprint(area);
        if (fp != null && fp != Identity.FingerprintText)
        {
            Clear();
            Refused = $"area {area} was authored against fingerprint {fp}; this one is {Identity.FingerprintText}";
            return;
        }
        Refused = null;
        _lights = Pack.Lights(area).Where(l => !l.Off && l.Radius > 0f && l.Intensity != 0f).ToArray();
        _derived = Runtime.Mem is { } mem ? TileGlow(mem, out _derivedSrc) : [];
        bool any = _lights.Length > 0 || _derived.Length > 0 || Surfaces.ModelsGiveLight;
        RemasterUniforms.Enabled = any;
        RetainedScene.ShadowsWanted = _shadows && _lights.Any(l => l.Shadows);
        ClearSlots();
        PolyAssembler.KeepForLights = any;
        PolyAssembler.Keep();
        if (!any) { RemasterUniforms.Publish(0); Sent = Culled = ModelLights = 0; }
    }

    /// <summary>How far off its surface a glow's light sits, along the lit side.</summary>
    const float GlowOffset = 192f;

    /// <summary>How far above a model's origin (its feet) its glow's light sits.</summary>
    const float ModelGlowHeight = 384f;

    /// <summary>A glow's point light; its direction, unused by a point, holds the
    /// surface's normal for the shell to report.</summary>
    static Pack.Light GlowLight(string name, Vector3 at, byte id, Vector3 normal)
    {
        var c = Surfaces.GlowLight[id];
        float s = MathF.Max(c.X, MathF.Max(c.Y, c.Z));
        return new Pack.Light(name, false, at, c / s, s, Surfaces.GlowRadius[id],
                              normal, 0f, 0f, 0f, 0f, false, Shadows: false);
    }

    /// <summary>
    /// A light for every tile half and material that glows and gives light: at the
    /// area-weighted centroid of those faces, <see cref="GlowOffset"/> out along their
    /// summed normal. A face's world corners are the half's placement (the tile centre,
    /// the floor at <c>-(h &lt;&lt; 7)</c>) plus its mesh vertex turned by the record's
    /// quarter turn, as <c>func_80014B88</c> turns the view matrix; its normal is the
    /// one the game lights it with, turned the same way.
    /// </summary>
    static Pack.Light[] TileGlow(IMemory m, out byte[] sources)
    {
        sources = [];
        if (!Surfaces.AnyGivesLight() || Faces.TileTable == 0) return [];
        uint table = Faces.TileTable;
        var list = new List<Pack.Light>();
        var src = new List<byte>();
        var sum = new Dictionary<byte, (Vector3 C, Vector3 N, float A)>();
        for (int z = 0; z < Identity.Span; z++)
        for (int x = 0; x < Identity.Span; x++)
        for (int half = 0; half < 2; half++)
        {
            uint rec = Identity.HalfRecord(x, z, half);
            int model = m.ReadU8(rec);
            if (model >= 240 || !Surfaces.Authored(x, z, half, model)) continue;
            var faces = Faces.Mesh(m, model);
            if (faces == null) continue;
            sum.Clear();
            uint header = table + 0xCu + (uint)model * 28u;
            uint verts = table + 0xCu + m.ReadU32(header);
            uint normals = table + 0xCu + m.ReadU32(header + 8u);
            uint f = table + 0xCu + m.ReadU32(header + 0x10u);
            int rot = m.ReadU8(rec + 2u) & 3;
            var pos = new Vector3(x * Identity.TileUnits + Identity.TileUnits / 2, -(m.ReadU8(rec + 1u) << 7),
                                  z * Identity.TileUnits + Identity.TileUnits / 2);
            for (int i = 0; i < faces.Length; i++, f = f + 4u + ((m.ReadU32(f) >> 6) & 0x3FCu))
            {
                if (faces[i].Verts.Length == 0) continue;
                byte id = Surfaces.FaceOf(x, z, half, model, i);
                if (id == 0) id = Surfaces.MeshFaceMaterial(faces[i]);
                if (!Surfaces.GivesLight(id)) continue;
                var v = faces[i].Verts;
                Span<Vector3> w = stackalloc Vector3[4];
                for (int k = 0; k < v.Length; k++) w[k] = pos + Turn(Vec(m, verts + (uint)v[k]), rot);
                // A quad's corners are 0 1 3 2 around.
                var cross = v.Length == 4
                    ? Vector3.Cross(w[3] - w[0], w[2] - w[1])
                    : Vector3.Cross(w[1] - w[0], w[2] - w[0]);
                float area = cross.Length() * 0.5f;
                if (area <= 0f) continue;
                var centre = Vector3.Zero;
                for (int k = 0; k < v.Length; k++) centre += w[k];
                centre /= v.Length;
                uint ni = m.ReadU16(f + 4u + (v.Length == 4 ? 0x10u : 0x0Cu));
                var n = Turn(Vec(m, normals + ni), rot);
                sum.TryGetValue(id, out var acc);
                sum[id] = (acc.C + centre * area, acc.N + n * area, acc.A + area);
            }
            foreach (var (id, acc) in sum)
            {
                var n = acc.N.LengthSquared() > 1e-6f ? Vector3.Normalize(acc.N) : -Vector3.UnitY;
                var at = acc.C / acc.A + n * GlowOffset;
                list.Add(GlowLight($"glow {new TileKey(Identity.Area, x, z, half)} {Surfaces.IdNames[id]}", at, id, n));
                src.Add(id);
            }
        }
        sources = src.ToArray();
        return list.ToArray();
    }

    static Vector3 Vec(IMemory m, uint a)
        => new((short)m.ReadU16(a), (short)m.ReadU16(a + 2u), (short)m.ReadU16(a + 4u));

    /// <summary>A half's quarter turn, as <c>func_80014B88</c> applies it to the matrix.</summary>
    static Vector3 Turn(Vector3 v, int rot) => rot switch
    {
        1 => new Vector3(v.Z, v.Y, -v.X),
        2 => new Vector3(-v.X, v.Y, -v.Z),
        3 => new Vector3(-v.Z, v.Y, v.X),
        _ => v,
    };

    /// <summary>The frame's view: <c>v = R (w - cam) + T</c>, as <c>func_80031950</c>
    /// feeds the tile walk.</summary>
    public struct View
    {
        public Matrix4x4 R;
        public Vector3 T, Cam;
        public float H, Cx, Cy;

        public readonly Vector3 ToView(Vector3 w)
        {
            var d = w - Cam;
            return new Vector3(R.M11 * d.X + R.M12 * d.Y + R.M13 * d.Z,
                               R.M21 * d.X + R.M22 * d.Y + R.M23 * d.Z,
                               R.M31 * d.X + R.M32 * d.Y + R.M33 * d.Z) + T;
        }

        public readonly Vector3 Rotate(Vector3 d)
            => new(R.M11 * d.X + R.M12 * d.Y + R.M13 * d.Z,
                   R.M21 * d.X + R.M22 * d.Y + R.M23 * d.Z,
                   R.M31 * d.X + R.M32 * d.Y + R.M33 * d.Z);

        public readonly Vector3 ToWorld(Vector3 v)
        {
            var d = v - T;
            return Cam + new Vector3(R.M11 * d.X + R.M21 * d.Y + R.M31 * d.Z,
                                     R.M12 * d.X + R.M22 * d.Y + R.M32 * d.Z,
                                     R.M13 * d.X + R.M23 * d.Y + R.M33 * d.Z);
        }

        /// <summary>A world point to game pixels and its view depth; false behind the eye.</summary>
        public readonly bool Project(Vector3 w, out Vector2 screen, out float z)
        {
            var p = ToView(w);
            z = p.Z;
            screen = default;
            if (p.Z < 16f) return false;
            screen = new Vector2(Cx + H * p.X / p.Z, Cy + H * p.Y / p.Z);
            return true;
        }

        /// <summary>The world point at game pixel <paramref name="s"/> and view depth <paramref name="z"/>.</summary>
        public readonly Vector3 Unproject(Vector2 s, float z)
            => ToWorld(new Vector3((s.X - Cx) * z / H, (s.Y - Cy) * z / H, z));
    }

    public static View ReadView(IMemory m)
    {
        const uint vm = CameraBlock.ViewMatrix;
        float E(uint off) => (short)m.ReadU16(vm + off) / 4096f;
        var cam = Camera.Read(m);
        return new View
        {
            R = new Matrix4x4(E(0), E(2), E(4), 0, E(6), E(8), E(10), 0, E(12), E(14), E(16), 0, 0, 0, 0, 1),
            T = new Vector3((int)m.ReadU32(vm + 0x14), (int)m.ReadU32(vm + 0x18), (int)m.ReadU32(vm + 0x1C)),
            Cam = new Vector3(cam.X, cam.Y, cam.Z),
            H = Math.Max(1f, GteDepth.ProjH),
            Cx = GteDepth.ProjCx,
            Cy = GteDepth.ProjCy,
        };
    }

    static readonly (float Key, int Index)[] _order = new (float, int)[256];

    /// <summary>Authored lights sort ahead of every derived one.</summary>
    const float DerivedRank = 1e9f;

    public static void BeforeDrawOTag(CpuContext c, IMemory m)
    {
        // The world tick every pulse and flicker is evaluated on.
        if (FramePacing.FirstWalkOfTick(ref _tickFrame)) _ticks++;
        if (!RemasterUniforms.Enabled) return;

        int total = Gather();
        var v = ReadView(m);
        int n = 0, culled = 0;
        for (int i = 0; i < total && n < _order.Length; i++)
        {
            ref readonly var l = ref _frame[i];
            var p = v.ToView(l.Position);
            float r = l.Radius;
            // Behind the eye, or past the draw window in any direction.
            if (p.Z + r < 0f || p.Length() - r > Reach) { culled++; continue; }
            _order[n++] = (MathF.Max(p.Length() - r, 0f) + (i < _lights.Length ? 0f : DerivedRank), i);
        }
        Array.Sort(_order, 0, n, Comparer<(float Key, int Index)>.Create((a, b) => a.Key.CompareTo(b.Key)));
        int sent = Math.Min(n, RemasterUniforms.MaxLights);
        culled += n - sent;

        double t = _ticks / 20.0;
        for (int k = 0; k < sent; k++)
        {
            ref readonly var l = ref _frame[_order[k].Index];
            var p = v.ToView(l.Position);
            int o = k * 4;
            RemasterUniforms.LightPos[o] = p.X;
            RemasterUniforms.LightPos[o + 1] = p.Y;
            RemasterUniforms.LightPos[o + 2] = p.Z;
            RemasterUniforms.LightPos[o + 3] = l.Radius;
            byte from = _frameSrc[_order[k].Index];
            float s = l.Intensity * Flicker(l, t, _order[k].Index) * (from != 0 ? Surfaces.Pulse[from] : 1f);
            RemasterUniforms.LightCol[o] = l.Colour.X * s;
            RemasterUniforms.LightCol[o + 1] = l.Colour.Y * s;
            RemasterUniforms.LightCol[o + 2] = l.Colour.Z * s;
            if (l.Spot)
            {
                var d = v.Rotate(l.Direction.LengthSquared() > 1e-8f ? Vector3.Normalize(l.Direction) : Vector3.UnitY);
                float outer = Math.Clamp(l.ConeOuter, 1f, 89f), inner = Math.Clamp(l.ConeInner, 0f, outer);
                RemasterUniforms.LightCol[o + 3] = MathF.Cos(inner * MathF.PI / 180f);
                RemasterUniforms.LightDir[o] = d.X;
                RemasterUniforms.LightDir[o + 1] = d.Y;
                RemasterUniforms.LightDir[o + 2] = d.Z;
                RemasterUniforms.LightDir[o + 3] = MathF.Cos(outer * MathF.PI / 180f);
                // Equal cones would make the edge a step smoothstep cannot take.
                if (RemasterUniforms.LightDir[o + 3] >= RemasterUniforms.LightCol[o + 3])
                    RemasterUniforms.LightDir[o + 3] = RemasterUniforms.LightCol[o + 3] - 1e-4f;
            }
            else
            {
                RemasterUniforms.LightCol[o + 3] = -1f;
                RemasterUniforms.LightDir[o] = RemasterUniforms.LightDir[o + 1] = RemasterUniforms.LightDir[o + 2] = 0f;
                // A point's outer cosine is -2; below it, the material it does not light.
                RemasterUniforms.LightDir[o + 3] = -2f - from;
            }
        }
        AssignShadows(sent, v);
        RemasterUniforms.Publish(sent);
        Sent = sent;
        Culled = culled;
        Frames++;
    }

    static void ClearSlots()
    {
        Array.Fill(_slotName, null);
        Array.Clear(RemasterUniforms.ShadowLight);
        Array.Fill(RemasterUniforms.LightShadow, -1);
    }

    /// <summary>A slot for each of the nearest shadowed authored lights sent, a light
    /// keeping the slot it had; the rest of the list unshadowed.</summary>
    static void AssignShadows(int sent, in View v)
    {
        Array.Fill(RemasterUniforms.LightShadow, -1);
        const int N = RemasterUniforms.MaxShadows;
        Span<bool> held = stackalloc bool[N];
        Span<int> pending = stackalloc int[N];
        int np = 0;
        for (int k = 0; k < sent && RetainedScene.ShadowsWanted; k++)
        {
            int i = _order[k].Index;
            if (i >= _lights.Length || !_frame[i].Shadows) continue;
            int s = Array.IndexOf(_slotName, _frame[i].Name);
            if (s >= 0) { held[s] = true; RemasterUniforms.LightShadow[k] = s; }
            else if (np < N) pending[np++] = k;
        }
        for (int p = 0; p < np; p++)
        {
            int s = held.IndexOf(false);
            if (s < 0) break;
            held[s] = true;
            _slotName[s] = _frame[_order[pending[p]].Index].Name;
            RemasterUniforms.LightShadow[pending[p]] = s;
        }
        for (int s = 0; s < N; s++)
            if (!held[s]) { _slotName[s] = null; RemasterUniforms.ShadowLight[s * 4 + 3] = 0f; }
        for (int k = 0; k < sent; k++)
        {
            int s = RemasterUniforms.LightShadow[k];
            if (s < 0) continue;
            ref readonly var l = ref _frame[_order[k].Index];
            RemasterUniforms.ShadowLight[s * 4] = l.Position.X;
            RemasterUniforms.ShadowLight[s * 4 + 1] = l.Position.Y;
            RemasterUniforms.ShadowLight[s * 4 + 2] = l.Position.Z;
            RemasterUniforms.ShadowLight[s * 4 + 3] = l.Radius;
        }
        var r = v.R;
        ReadOnlySpan<float> rows = [r.M11, r.M12, r.M13, r.M21, r.M22, r.M23, r.M31, r.M32, r.M33];
        rows.CopyTo(RemasterUniforms.ToWorld);
        // The walk has submitted every model by now: these are the ones that cast.
        RemasterUniforms.ShadowFrame = RetainedScene.Serial;
    }

    /// <summary>The frame's candidates into <see cref="_frame"/>: the authored lights,
    /// the tile glows, and a light per glowing model the last walk drew.</summary>
    static int Gather()
    {
        int n = 0;
        void Add(in Pack.Light l, byte from)
        {
            if (n == _frame.Length) { Array.Resize(ref _frame, n * 2); Array.Resize(ref _frameSrc, n * 2); }
            _frameSrc[n] = from;
            _frame[n++] = l;
        }
        foreach (var l in _lights) Add(l, 0);
        for (int i = 0; i < _derived.Length; i++) Add(_derived[i], _derivedSrc[i]);
        int models = 0;
        if (Surfaces.ModelsGiveLight)
            foreach (ref readonly var d in ModelWalk.Scene)
            {
                byte id = Surfaces.EnterModel(d.Kind, d.Model);
                if (!Surfaces.GivesLight(id)) continue;
                Add(GlowLight("glow model", new Vector3(d.X, d.Y - ModelGlowHeight, d.Z), id, -Vector3.UnitY), id);
                models++;
            }
        ModelLights = models;
        return n;
    }

    /// <summary>1 less up to the light's amount, varying smoothly at about its rate:
    /// value noise on the world clock, a different phase per light.</summary>
    static float Flicker(in Pack.Light l, double t, int seed)
    {
        if (l.FlickerAmount <= 0f || l.FlickerHz <= 0f) return 1f;
        double x = t * l.FlickerHz;
        long k = (long)Math.Floor(x);
        float f = (float)(x - k);
        f = f * f * (3f - 2f * f);
        float a = Hash(k, seed), b = Hash(k + 1, seed);
        return 1f - Math.Clamp(l.FlickerAmount, 0f, 1f) * (a + (b - a) * f);
    }

    /// <summary>Smooth value noise, 0..1, at <paramref name="x"/> cycles.</summary>
    public static float Noise(double x, int seed)
    {
        long k = (long)Math.Floor(x);
        float f = (float)(x - k);
        f = f * f * (3f - 2f * f);
        float a = Hash(k, seed), b = Hash(k + 1, seed);
        return a + (b - a) * f;
    }

    static float Hash(long k, int seed)
    {
        ulong h = (ulong)k * 0x9E3779B97F4A7C15ul ^ (ulong)(seed + 1) * 0xC2B2AE3D27D4EB4Ful;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ul;
        h ^= h >> 32;
        return (h & 0xFFFFFF) / (float)0x1000000;
    }

    public string Probe()
        => Refused != null ? $"lights refused: {Refused}"
         : $"lights {Authored} authored, {Derived} from tile glow, {ModelLights} from model glow, {Sent} sent, {Culled} culled; " +
           $"{RemasterUniforms.Uploads} upload(s), {RemasterUniforms.LitBatches} lit batch(es)" +
           $"; {RemasterUniforms.ShadowsReady} shadow cubemap(s) ready, {RemasterUniforms.ShadowRenders} drawn, " +
           $"{RemasterUniforms.ShadowTriangles} triangle(s) into them; models {(RetainedScene.ShadowModels ? "cast" : "do not cast")}, " +
           $"{RemasterUniforms.ShadowCasters} triangle(s) in reach, {RemasterUniforms.ShadowModelRenders} redrawn for them, " +
           $"{RemasterUniforms.ShadowModelTriangles} model triangle(s) into them" +
           (RemasterUniforms.Supported ? "" : " (the backend has no light term)") +
           (Authored > 0 && !PerPixelLighting.Enabled ? " (per-pixel lighting is off, so nothing is lit)" : "");
}
