using System.Numerics;
using RecompOne.Runtime;

namespace Kf2.Remaster;

/// <summary>
/// Authored materials on map tiles and models. The pack names a material for a whole
/// tile half, for faces of the mesh a half draws, for faces of a mesh wherever the area
/// uses it, and for a model wherever the area draws it; this resolves the names to
/// <see cref="SurfaceMaterial"/> ids when the area settles or the pack changes.
/// <see cref="TileWalk"/> asks for the half it is assembling, <see cref="Faces"/> for
/// each face and <see cref="ModelWalk"/> for each model, and
/// <c>PolyAssembler.SealDepth</c> writes the id into the packet's depth record. From
/// there it is the runtime's: the surface buffer carries it to the reflection pass
/// (reflectivity, F0, roughness, 0067), and the light record to the prim shader
/// (emissive, 0071).
///
/// The most specific entry wins: the half's face, the whole half, the mesh's face, the
/// whole mesh, then the art the packet draws (<c>textures.json</c>, in every area). A face list applies only to the mesh it was authored on, and only while
/// that mesh hashes as it did (<see cref="Faces.MeshHash"/>); a mismatch drops the
/// entry whole. Off, and with nothing authored for the area, every answer is 0 and the
/// record's material is the None it always was.
/// </summary>
public sealed class Surfaces : IRemasterFeature
{
    public string Id => "surfaces";

    /// <summary>One id per tile half: (z * 80 + x) * 2 + half.</summary>
    static readonly byte[] _table = new byte[Identity.Span * Identity.Span * 2];

    /// <summary>Whether <see cref="_table"/> holds anything for the area on screen.</summary>
    static bool _active;

    /// <summary>Face lists by half index, each with the mesh it was authored on.</summary>
    static readonly Dictionary<int, (int Mesh, byte[] Ids)> _tileFaces = new();

    /// <summary>Art by texture key, in every area: the key with its CLUT hash, and the
    /// key with none (any palette).</summary>
    static readonly Dictionary<TexKey, byte> _textures = new();

    /// <summary>Whether any texture rule applies, so a packet with no other material
    /// asks for its texture's.</summary>
    public static bool ByTexture { get; private set; }

    /// <summary>Texture rules applied, and packets given a material by one; never reset.</summary>
    public static int TexturesApplied { get; private set; }
    public static long TexturePackets;

    /// <summary>Models by kind and id.</summary>
    static readonly Dictionary<(ModelKind, int), byte> _models = new();

    /// <summary>Area-wide rules by mesh: a whole-mesh id and a face list.</summary>
    static readonly Dictionary<int, byte> _meshWhole = new();
    static readonly Dictionary<int, byte[]> _meshFaces = new();

    /// <summary>Whether anything for the area is authored below the whole half, so
    /// the assemblers must ask per face.</summary>
    public static bool PerFace { get; private set; }

    // The half being assembled.
    static byte _half, _meshAll;
    static byte[]? _tf, _mf;

    /// <summary>Face lists dropped because their mesh no longer hashes as it did.</summary>
    public static int MeshRefused { get; private set; }

    /// <summary>The name each authored id was given, from <see cref="SurfaceMaterial.FirstAuthored"/>.</summary>
    public static readonly string?[] IdNames = new string?[SurfaceMaterial.Count];

    /// <summary>The light a glowing id gives off (colour times intensity), and how far
    /// it reaches; a radius of 0 is none. Read by <see cref="Lights"/>.</summary>
    public static readonly Vector3[] GlowLight = new Vector3[SurfaceMaterial.Count];
    public static readonly float[] GlowRadius = new float[SurfaceMaterial.Count];

    /// <summary>Whether an id gives off a light.</summary>
    public static bool GivesLight(byte id) => id != 0 && GlowRadius[id] > 0f;

    /// <summary>Each id's glow before its pulse, and the pulse: amount, rate, and
    /// whether it flickers (value noise) or breathes (a sine).</summary>
    static readonly Vector3[] _baseGlow = new Vector3[SurfaceMaterial.Count];
    static readonly (float Amount, float Hz, bool Flicker)[] _pulse = new (float, float, bool)[SurfaceMaterial.Count];
    static bool _anyPulse;
    static long _pulseTick = -1;

    /// <summary>What an id's pulse multiplies its glow and its light by this tick.</summary>
    public static readonly float[] Pulse = Filled(1f);

    static float[] Filled(float v)
    {
        var a = new float[SurfaceMaterial.Count];
        Array.Fill(a, v);
        return a;
    }

    /// <summary>Bumped each time the area's surfaces are applied or cleared.</summary>
    public static int Serial { get; private set; }

    /// <summary>Whether any model rule's material gives off a light.</summary>
    public static bool ModelsGiveLight { get; private set; }

    /// <summary>Why the area's surfaces are not applied, or null.</summary>
    public static string? Refused { get; private set; }

    /// <summary>Library entries past the ids 0067's table has room for.</summary>
    public static int Unallocated { get; private set; }

    public static int TilesApplied { get; private set; }

    /// <summary>Packets sealed with an authored material; never reset.</summary>
    public static long Packets;

    int _version = -1, _settle = -1, _tableSerial = -1;

    public void OnFrame()
    {
        if (!Host.Enabled || !Identity.Settled)
        {
            if (_active) Clear();
            _settle = _tableSerial = -1;
            TilesApplied = 0;
            if (!Host.Enabled) ClearIds();
            return;
        }
        if (_anyPulse) StepPulse();
        // A face list is checked against its mesh, which is readable once the tile walk
        // has noted the table.
        if (_version == Pack.Version && _settle == Identity.Settles && _tableSerial == Faces.TableSerial) return;
        _version = Pack.Version;
        _settle = Identity.Settles;
        _tableSerial = Faces.TableSerial;
        Apply();
    }

    public void Detach()
    {
        Clear();
        ClearIds();
        _version = _settle = _tableSerial = -1;
    }

    static void Clear()
    {
        Array.Clear(_table);
        _tileFaces.Clear();
        _meshWhole.Clear();
        _meshFaces.Clear();
        _models.Clear();
        _textures.Clear();
        _active = PerFace = ModelsGiveLight = ByTexture = false;
        TexturesApplied = 0;
        Serial++;
        PolyAssembler.KeepForGlow = false;
        PolyAssembler.Keep();
        _tf = _mf = null;
        _half = _meshAll = 0;
    }

    static bool _idsSet;

    static void ClearIds()
    {
        for (int i = SurfaceMaterial.FirstAuthored; i < SurfaceMaterial.Count; i++)
        {
            SurfaceMaterial.Reflectivity[i] = 0f;
            SurfaceMaterial.F0[i] = 0f;
            SurfaceMaterial.Roughness[i] = 0f;
            SurfaceMaterial.Emissive[i * 3] = SurfaceMaterial.Emissive[i * 3 + 1] = SurfaceMaterial.Emissive[i * 3 + 2] = 0f;
            SurfaceMaterial.EmissiveAdditive[i] = false;
            SurfaceMaterial.EmissiveUnfogged[i] = false;
            SurfaceMaterial.Metalness[i] = 0f;
            SurfaceMaterial.Specular[i] = 0f;
            SurfaceMaterial.Occlusion[i] = 1f;
            _baseGlow[i] = Vector3.Zero;
            _pulse[i] = default;
            Pulse[i] = 1f;
            GlowLight[i] = Vector3.Zero;
            GlowRadius[i] = 0f;
            IdNames[i] = null;
        }
        if (_idsSet) SurfaceMaterial.Changed();
        _idsSet = false;
        _anyPulse = false;
    }

    static void Apply()
    {
        Clear();
        ClearIds();
        MeshRefused = 0;

        // Ids are handed out by name at load time, so two packs never collide on a
        // number. 0067's table has 256 and the runtime keeps four.
        var ids = new Dictionary<string, byte>();
        int next = SurfaceMaterial.FirstAuthored;
        int over = 0;
        foreach (var m in Pack.Materials())
        {
            if (next >= SurfaceMaterial.Count) { over++; continue; }
            ids[m.Name] = (byte)next;
            IdNames[next] = m.Name;
            SurfaceMaterial.Reflectivity[next] = Math.Clamp(m.Reflectivity, 0f, 1f);
            SurfaceMaterial.F0[next] = Math.Clamp(m.F0, 0f, 1f);
            SurfaceMaterial.Roughness[next] = Math.Clamp(m.Roughness, 0f, 1f);
            var colour = Vector3.Clamp(m.Emissive, Vector3.Zero, Vector3.One);
            var e = colour * Math.Clamp(m.EmissiveStrength, 0f, 4f);
            _baseGlow[next] = e;
            SurfaceMaterial.Emissive[next * 3] = e.X;
            SurfaceMaterial.Emissive[next * 3 + 1] = e.Y;
            SurfaceMaterial.Emissive[next * 3 + 2] = e.Z;
            SurfaceMaterial.EmissiveAdditive[next] = m.GlowAdditive;
            SurfaceMaterial.EmissiveUnfogged[next] = m.GlowAdditive && m.GlowUnfogged;
            SurfaceMaterial.Metalness[next] = Math.Clamp(m.Metalness, 0f, 1f);
            SurfaceMaterial.Specular[next] = Math.Clamp(m.Specular, 0f, 1f);
            SurfaceMaterial.Occlusion[next] = Math.Clamp(m.Occlusion, 0f, 1f);
            // The light is its own: a material may give light with no glow at all.
            GlowLight[next] = colour * Math.Clamp(m.Light, 0f, Pack.MaxLight);
            GlowRadius[next] = GlowLight[next] != Vector3.Zero ? Math.Clamp(m.GlowRadius, 0f, Pack.MaxGlowRadius) : 0f;
            _pulse[next] = (Math.Clamp(m.PulseAmount, 0f, 1f), Math.Max(m.PulseHz, 0f), m.PulseFlicker);
            if (_pulse[next].Amount > 0f && _pulse[next].Hz > 0f) _anyPulse = true;
            next++;
        }
        Unallocated = over;
        _pulseTick = -1;
        SurfaceMaterial.Changed();
        _idsSet = true;

        // Texture rules name content, so they hold in every area and need no fingerprint.
        foreach (var r in Pack.TextureRules())
        {
            if (!ids.TryGetValue(r.Material, out byte tid)) continue;
            _textures[r.Key] = tid;
        }
        TexturesApplied = _textures.Count;
        ByTexture = _textures.Count > 0;

        int area = Identity.Area;
        string? fp = Pack.AreaFingerprint(area);
        if (fp != null && fp != Identity.FingerprintText)
        {
            Refused = $"area {area} was authored against fingerprint {fp}; this one is {Identity.FingerprintText}";
            TilesApplied = 0;
            _active = ByTexture;
            KeepRecords();
            Serial++;
            return;
        }
        Refused = null;

        var mem = Runtime.Mem;
        int n = 0;
        foreach (var (k, name) in Pack.Tiles(area))
        {
            if (!ids.TryGetValue(name, out byte id)) continue;
            _table[Index(k.X, k.Z, k.Half)] = id;
            n++;
        }
        foreach (var e in Pack.TileFaceLists(area))
        {
            if (mem == null || !Gate(mem, e.Mesh, e.Hash)) { MeshRefused++; continue; }
            var list = Ids(e.Faces, ids);
            if (list == null) continue;
            _tileFaces[Index(e.Tile.X, e.Tile.Z, e.Tile.Half)] = (e.Mesh, list);
            n++;
        }
        foreach (var e in Pack.MeshRules(area))
        {
            if (mem == null || !Gate(mem, e.Mesh, e.Hash)) { MeshRefused++; continue; }
            if (e.Material != null && ids.TryGetValue(e.Material, out byte all)) _meshWhole[e.Mesh] = all;
            if (Ids(e.Faces, ids) is { } list) _meshFaces[e.Mesh] = list;
            n++;
        }
        foreach (var r in Pack.ModelRules(area))
        {
            if (!ids.TryGetValue(r.Material, out byte id)) continue;
            _models[(r.Model.Kind, r.Model.Model)] = id;
            if (GivesLight(id)) ModelsGiveLight = true;
            n++;
        }
        TilesApplied = n;
        PerFace = _tileFaces.Count > 0 || _meshWhole.Count > 0 || _meshFaces.Count > 0;
        _active = n > 0 || ByTexture;
        KeepRecords();
        Serial++;
    }

    /// <summary>A glowing face near the eye is unfogged, and it glows only through its
    /// record. A highlight is on the record too, so an unfogged face needs one for it.</summary>
    static void KeepRecords()
    {
        PolyAssembler.KeepForGlow = _active && (SurfaceMaterial.AnyEmissive || SurfaceMaterial.AnySpecular);
        PolyAssembler.Keep();
    }

    /// <summary>The material of a piece of art: its own palette's rule, else any palette's.</summary>
    public static byte TextureId(TexKey k)
    {
        if (_textures.TryGetValue(k, out byte id)) return id;
        return k.Clut != 0 && _textures.TryGetValue(k.AnyClut, out id) ? id : (byte)0;
    }

    /// <summary>SealDepth, for a packet nothing more specific named: its texture's id.</summary>
    public static byte PacketMaterial(RecompOne.Runtime.Memory.IMemory m, uint pkt, uint cmd)
    {
        if (!TextureKeys.OfPacket(m, pkt, cmd, out var k)) return 0;
        byte id = TextureId(k);
        if (id != 0) TexturePackets++;
        return id;
    }

    /// <summary>A mesh face's texture id, from the face as the model table holds it.</summary>
    public static byte MeshFaceMaterial(in Faces.MeshFace f)
        => ByTexture && f.Verts.Length > 0
           && TextureKeys.Of(f.Tpage, f.Clut, (int)(f.Rect & 0xFF), (int)((f.Rect >> 8) & 0xFF),
                             (int)((f.Rect >> 16) & 0xFF), (int)(f.Rect >> 24), out var k)
            ? TextureId(k) : (byte)0;

    /// <summary>Once per world tick: every pulsing id's glow times its pulse, so it
    /// holds while the world is paused, as a light's flicker does.</summary>
    static void StepPulse()
    {
        long tick = Lights.Ticks;
        if (tick == _pulseTick) return;
        _pulseTick = tick;
        double t = tick / 20.0;
        for (int i = SurfaceMaterial.FirstAuthored; i < SurfaceMaterial.Count; i++)
        {
            var (amount, hz, flicker) = _pulse[i];
            if (amount <= 0f || hz <= 0f) continue;
            float wave = flicker ? Lights.Noise(t * hz, i) : 0.5f - 0.5f * MathF.Cos((float)(t * hz * 2.0 * Math.PI));
            float k = 1f - amount * wave;
            Pulse[i] = k;
            var e = _baseGlow[i] * k;
            SurfaceMaterial.Emissive[i * 3] = e.X;
            SurfaceMaterial.Emissive[i * 3 + 1] = e.Y;
            SurfaceMaterial.Emissive[i * 3 + 2] = e.Z;
        }
        SurfaceMaterial.Changed();
    }

    static bool Gate(RecompOne.Runtime.Memory.IMemory m, int mesh, string? hash)
        => hash != null && Faces.MeshHash(m, mesh) == hash;

    /// <summary>A face list's names as ids, indexed by face; null when none resolves.</summary>
    static byte[]? Ids(IEnumerable<(int Face, string Material)> faces, Dictionary<string, byte> ids)
    {
        byte[]? list = null;
        foreach (var (f, name) in faces)
        {
            if (f is < 0 or > 4095 || !ids.TryGetValue(name, out byte id)) continue;
            if (list == null || list.Length <= f) Array.Resize(ref list, f + 1);
            list[f] = id;
        }
        return list;
    }

    static int Index(int x, int z, int half) => (z * Identity.Span + x) * 2 + half;

    /// <summary>The half whose record is at <paramref name="rec"/> is about to draw mesh
    /// <paramref name="model"/>: its id where no face says otherwise.</summary>
    public static byte EnterHalf(uint rec, int model)
    {
        _tf = _mf = null;
        _half = _meshAll = 0;
        if (!_active || !Identity.FromRecord(rec, out int x, out int z, out int half)) return 0;
        int i = Index(x, z, half);
        _half = _table[i];
        if (PerFace)
        {
            if (_tileFaces.TryGetValue(i, out var t) && t.Mesh == model) _tf = t.Ids;
            _meshFaces.TryGetValue(model, out _mf);
            _meshWhole.TryGetValue(model, out _meshAll);
        }
        return _half != 0 ? _half : _meshAll;
    }

    /// <summary>The id of face <paramref name="f"/> of the half being assembled; -1 is a
    /// face that could not be named.</summary>
    public static byte Face(int f)
    {
        byte id = 0;
        if (_tf != null && (uint)f < (uint)_tf.Length) id = _tf[f];
        if (id == 0) id = _half;
        if (id == 0 && _mf != null && (uint)f < (uint)_mf.Length) id = _mf[f];
        return id != 0 ? id : _meshAll;
    }

    /// <summary>The id of face <paramref name="f"/> of mesh <paramref name="model"/> drawn
    /// by the half at (<paramref name="x"/>, <paramref name="z"/>): what <see cref="EnterHalf"/>
    /// and <see cref="Face"/> answer during the walk, without touching their state.</summary>
    public static byte FaceOf(int x, int z, int half, int model, int f)
    {
        if (!_active) return 0;
        int i = Index(x, z, half);
        byte id = 0;
        if (PerFace && _tileFaces.TryGetValue(i, out var t) && t.Mesh == model && (uint)f < (uint)t.Ids.Length) id = t.Ids[f];
        if (id == 0) id = _table[i];
        if (id == 0 && PerFace && _meshFaces.TryGetValue(model, out var mf) && (uint)f < (uint)mf.Length) id = mf[f];
        if (id == 0 && PerFace) _meshWhole.TryGetValue(model, out id);
        return id;
    }

    /// <summary>Whether anything is authored that could name a face of this half.</summary>
    public static bool Authored(int x, int z, int half, int model)
    {
        if (!_active) return false;
        if (ByTexture) return true;
        int i = Index(x, z, half);
        return _table[i] != 0 || (PerFace && (_tileFaces.ContainsKey(i) || _meshWhole.ContainsKey(model) || _meshFaces.ContainsKey(model)));
    }

    /// <summary>Whether anything authored for the area gives off a light.</summary>
    public static bool AnyGivesLight()
    {
        if (!_active) return false;
        for (int i = SurfaceMaterial.FirstAuthored; i < SurfaceMaterial.Count; i++)
            if (GivesLight((byte)i)) return true;
        return false;
    }

    /// <summary>The model about to be assembled: its id, or 0.</summary>
    public static byte EnterModel(ModelKind kind, int model)
        => _active && _models.Count > 0 && _models.TryGetValue((kind, model), out byte id) ? id : (byte)0;

    public static byte IdOf(string? name)
    {
        if (name == null) return 0;
        for (int i = SurfaceMaterial.FirstAuthored; i < SurfaceMaterial.Count; i++)
            if (IdNames[i] == name) return (byte)i;
        return 0;
    }

    public string Probe()
        => $"surfaces {(Refused != null ? "refused: " + Refused : $"{TilesApplied} entries applied")}" +
           (MeshRefused > 0 ? $", {MeshRefused} dropped for a changed mesh" : "") + ", " +
           (TexturesApplied > 0 ? $"{TexturesApplied} texture rule(s), {TexturePackets} packet(s) by texture, " +
                                  $"{TextureKeys.Lookups} lookup(s) {TextureKeys.Hashed} hashed {TextureKeys.NoUpload} not in an upload, " : "") +
           $"{Packets} packet(s) sealed with a material" +
           (Unallocated > 0 ? $", {Unallocated} material(s) past the id table" : "");
}
