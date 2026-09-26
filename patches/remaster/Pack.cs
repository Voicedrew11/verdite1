using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kf2.Remaster;

/// <summary>
/// The working pack: the documents the editor writes and the features read.
///
/// A remaster pack is an upstream asset pack (<c>pack.json</c>) with a
/// <c>remaster/</c> directory beside it: <c>materials.json</c>, the named material
/// library, and <c>areas/&lt;n&gt;/surfaces.json</c>: under <c>tiles</c>, a material for
/// a whole half and a face list for the mesh it draws; under <c>meshes</c>, the same
/// for a mesh wherever the area uses it; under <c>models</c>, a material for a model
/// (its table's kind and its id) wherever the area draws it. A face list carries the
/// mesh index and the mesh's hash it was authored against. <c>areas/&lt;n&gt;/lights.json</c> holds the
/// area's authored lights, each named, in world units with up at -Y.
/// Documents are kept as JSON trees, so a field this version does not know survives a
/// round trip. Only the working pack is read in this phase; layering other packs
/// under it is later. See "Data model and file format" in docs/REMASTER.md.
///
/// Everything here runs on the game thread except the file watch, which parses on
/// its own thread and hands the result over at the next <see cref="Poll"/>.
/// </summary>
public static class Pack
{
    public const int FormatVersion = 1;
    public const string GameId = "SLUS-00158";

    public static string Root { get; private set; } = Path.GetFullPath(Path.Combine("packs", "working"));
    static string RemasterDir => Path.Combine(Root, "remaster");
    static string MaterialsPath => Path.Combine(RemasterDir, "materials.json");
    static string TexturesPath => Path.Combine(RemasterDir, "textures.json");
    static string SurfacesPath(int area) => Path.Combine(RemasterDir, "areas", area.ToString(), "surfaces.json");
    static string LightsPath(int area) => Path.Combine(RemasterDir, "areas", area.ToString(), "lights.json");
    static string AtmospherePath(int area) => Path.Combine(RemasterDir, "areas", area.ToString(), "atmosphere.json");

    static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    /// <summary>Bumped on every change, loaded or edited; a feature re-applies when it moves.</summary>
    public static int Version { get; private set; }

    public static bool Dirty { get; private set; }
    public static string? LastError { get; private set; }
    public static DateTime? SavedAt { get; private set; }

    static Set _set = Set.Empty();

    sealed class Set
    {
        public JsonObject Materials = null!;
        public JsonObject Textures = null!;
        public readonly Dictionary<int, JsonObject> Surfaces = new();
        public readonly Dictionary<int, JsonObject> Lights = new();
        public readonly Dictionary<int, JsonObject> Atmosphere = new();

        public static Set Empty() => new() { Materials = NewMaterials(), Textures = NewTextures() };
    }

    public static void Configure(string? root)
    {
        if (!string.IsNullOrWhiteSpace(root)) Root = Path.GetFullPath(root);
    }

    // ---- load, save, watch -------------------------------------------------

    public static void Load()
    {
        try
        {
            _set = Read();
            LastError = null;
        }
        catch (Exception e)
        {
            LastError = e.Message;
            Console.Error.WriteLine($"[KF2] remaster: cannot read {Root}: {e.Message}");
        }
        Dirty = false;
        _undo.Clear();
        _redo.Clear();
        Version++;
    }

    static Set Read()
    {
        var s = Set.Empty();
        if (File.Exists(MaterialsPath)) s.Materials = Migrate(ParseObject(MaterialsPath), "materials");
        if (File.Exists(TexturesPath)) s.Textures = Migrate(ParseObject(TexturesPath), "textures");
        var areas = Path.Combine(RemasterDir, "areas");
        if (Directory.Exists(areas))
            foreach (var dir in Directory.EnumerateDirectories(areas))
            {
                if (!int.TryParse(Path.GetFileName(dir), out int area)) continue;
                var path = Path.Combine(dir, "surfaces.json");
                if (File.Exists(path)) s.Surfaces[area] = Migrate(ParseObject(path), "tiles");
                path = Path.Combine(dir, "lights.json");
                if (File.Exists(path)) s.Lights[area] = Migrate(ParseObject(path), "lights");
                path = Path.Combine(dir, "atmosphere.json");
                if (File.Exists(path)) s.Atmosphere[area] = Migrate(ParseObject(path), "records");
            }
        return s;
    }

    static JsonObject ParseObject(string path)
    {
        var node = JsonNode.Parse(File.ReadAllText(path),
            documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        return node as JsonObject ?? throw new InvalidDataException($"{path}: not a JSON object");
    }

    /// <summary>Version 1 is the only one; a newer file is read as far as it goes.</summary>
    static JsonObject Migrate(JsonObject doc, string collection)
    {
        int v = Int(doc["formatVersion"]) ?? FormatVersion;
        if (v > FormatVersion)
            Console.Error.WriteLine($"[KF2] remaster: a formatVersion {v} document read by version {FormatVersion}; unknown fields are kept");
        if (collection == "materials" && doc["materials"] is not JsonObject) doc["materials"] = new JsonObject();
        if (collection == "tiles" && doc["tiles"] is not JsonArray) doc["tiles"] = new JsonArray();
        if (collection == "lights" && doc["lights"] is not JsonArray) doc["lights"] = new JsonArray();
        if (collection == "textures" && doc["textures"] is not JsonArray) doc["textures"] = new JsonArray();
        if (collection == "records" && doc["records"] is not JsonArray) doc["records"] = new JsonArray();
        return doc;
    }

    public static void Save()
    {
        try
        {
            Directory.CreateDirectory(RemasterDir);
            WriteManifest();
            _set.Materials["formatVersion"] = FormatVersion;
            Write(MaterialsPath, _set.Materials);
            if (TexturesArr.Count > 0 || File.Exists(TexturesPath))
            {
                _set.Textures["formatVersion"] = FormatVersion;
                Write(TexturesPath, _set.Textures);
            }
            foreach (var (area, doc) in _set.Surfaces)
            {
                doc["formatVersion"] = FormatVersion;
                Directory.CreateDirectory(Path.GetDirectoryName(SurfacesPath(area))!);
                Write(SurfacesPath(area), doc);
            }
            foreach (var (area, doc) in _set.Lights)
            {
                doc["formatVersion"] = FormatVersion;
                Directory.CreateDirectory(Path.GetDirectoryName(LightsPath(area))!);
                Write(LightsPath(area), doc);
            }
            foreach (var (area, doc) in _set.Atmosphere)
            {
                doc["formatVersion"] = FormatVersion;
                Directory.CreateDirectory(Path.GetDirectoryName(AtmospherePath(area))!);
                Write(AtmospherePath(area), doc);
            }
            Dirty = false;
            LastError = null;
            SavedAt = DateTime.Now;
            _ignoreUntil = Environment.TickCount64 + 1000;
        }
        catch (Exception e)
        {
            LastError = e.Message;
            Console.Error.WriteLine($"[KF2] remaster: cannot save {Root}: {e.Message}");
        }
    }

    static void Write(string path, JsonObject doc)
    {
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, doc.ToJsonString(Indented) + "\n");
        File.Move(tmp, path, overwrite: true);
    }

    /// <summary>Upstream's manifest, written once, so the folder is a pack its loader
    /// accepts; the working pack carries no textures of its own yet.</summary>
    static void WriteManifest()
    {
        var path = Path.Combine(Root, "pack.json");
        if (File.Exists(path)) return;
        var doc = new JsonObject
        {
            ["formatVersion"] = 1,
            ["id"] = "working",
            ["name"] = "Working pack",
            ["description"] = "What the remaster editor saves. Holds identifiers and the author's own values, never disc data.",
            ["priority"] = 1000,
            ["game"] = new JsonObject { ["id"] = GameId, ["strict"] = true },
        };
        File.WriteAllText(path, doc.ToJsonString(Indented) + "\n");
    }

    static FileSystemWatcher? _watch;
    static volatile Set? _pending;
    static long _ignoreUntil;
    static long _changedAt;

    /// <summary>Watch the remaster directory; a change is parsed off the game thread
    /// and swapped in at the next <see cref="Poll"/>. A save of ours is ignored.</summary>
    public static void Watch()
    {
        try
        {
            Directory.CreateDirectory(RemasterDir);
            _watch = new FileSystemWatcher(RemasterDir, "*.json") { IncludeSubdirectories = true };
            _watch.Changed += (_, _) => Touched();
            _watch.Created += (_, _) => Touched();
            _watch.Deleted += (_, _) => Touched();
            _watch.Renamed += (_, _) => Touched();
            _watch.EnableRaisingEvents = true;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[KF2] remaster: not watching {RemasterDir}: {e.Message}");
        }
    }

    static void Touched()
    {
        long now = Environment.TickCount64;
        if (now < Interlocked.Read(ref _ignoreUntil)) return;
        Interlocked.Exchange(ref _changedAt, now);
        // An editor writes a file in several steps; parse once it has been quiet.
        Task.Delay(250).ContinueWith(_ =>
        {
            if (Environment.TickCount64 - Interlocked.Read(ref _changedAt) < 240) return;
            try { _pending = Read(); }
            catch (Exception e) { LastError = e.Message; }
        });
    }

    /// <summary>On the game thread, once a frame.</summary>
    public static void Poll()
    {
        var p = _pending;
        if (p == null) return;
        _pending = null;
        if (Dirty)
        {
            LastError = "the files changed on disk while there were unsaved edits; kept the edits (Reload to take the files)";
            return;
        }
        _set = p;
        LastError = null;
        _undo.Clear();
        _redo.Clear();
        Version++;
        Console.WriteLine($"[KF2] remaster: reloaded {Root}");
    }

    // ---- materials ---------------------------------------------------------

    /// <summary>A named material. Emissive is a linear colour and its strength, in the
    /// game's light units: strength 1 lights a surface as its own RGBC at full.
    /// <c>GlowAdditive</c> (<c>"glowMode": "additive"</c>, the default) adds the glow
    /// past the texture, <c>"lit"</c> to the lit colour under it; <c>GlowUnfogged</c>
    /// (<c>"glowFog": false</c>) keeps an additive glow out of the fog. The material
    /// gives off a light of the emissive colour at <c>Light</c> intensity,
    /// <c>GlowRadius</c> across, whether or not the surface itself glows; with no
    /// <c>"light"</c> it is <c>"glowLight"</c> (0.5) times the glow's strength, as it
    /// was first written. <c>Pulse*</c> vary the glow and its light on the world
    /// tick. <c>Metalness</c> makes it a mirror tinted by its colour: the reflection
    /// and highlight take that colour, the reflectivity is at least the metalness, F0
    /// rises to it, and the surface's own colour is darkened, <c>Specular</c> is the highlight authored lights leave, and
    /// <c>Occlusion</c> how much the occlusion pass darkens it (1, or 0 on a glow,
    /// unless set).</summary>
    public readonly record struct Material(string Name, float Reflectivity, float F0, float Roughness,
                                           Vector3 Emissive, float EmissiveStrength,
                                           bool GlowAdditive, float Light, float GlowRadius,
                                           bool GlowUnfogged, float PulseAmount, float PulseHz, bool PulseFlicker,
                                           float Metalness, float Specular, float Occlusion);

    public const float DefaultGlowLight = 0.5f, DefaultGlowRadius = 2048f, MaxGlowRadius = 8192f, MaxLight = 4f;

    static JsonObject NewMaterials() => new() { ["formatVersion"] = FormatVersion, ["materials"] = new JsonObject() };

    static JsonObject MaterialsObj => (JsonObject)_set.Materials["materials"]!;

    public static IEnumerable<Material> Materials()
    {
        foreach (var (name, node) in MaterialsObj)
            if (node is JsonObject o)
            {
                float strength = Num(o, "emissiveStrength");
                yield return new Material(name, Num(o, "reflectivity"), Num(o, "f0"), Num(o, "roughness"),
                                          Vec(o["emissive"], Vector3.One), strength,
                                          Str(o["glowMode"]) != "lit",
                                          NumOr(o["light"], NumOr(o["glowLight"], DefaultGlowLight) * strength),
                                          NumOr(o["glowRadius"], DefaultGlowRadius),
                                          o["glowFog"] is JsonValue fv && fv.TryGetValue(out bool fog) && !fog,
                                          Num(o, "pulseAmount"), NumOr(o["pulseHz"], 1f),
                                          Str(o["pulseStyle"]) == "flicker",
                                          Num(o, "metalness"), Num(o, "specular"),
                                          NumOr(o["occlusion"], strength > 0f ? 0f : 1f));
            }
    }

    /// <summary>A true/false field, or its removal with null.</summary>
    public static void SetFlag(string name, string field, bool? value)
    {
        if (MaterialsObj[name] is not JsonObject o) return;
        bool? old = o[field] is JsonValue v && v.TryGetValue(out bool b) ? b : null;
        if (old == value) return;
        void Put(bool? x)
        {
            if (MaterialsObj[name] is not JsonObject m) return;
            if (x == null) m.Remove(field);
            else m[field] = x.Value;
        }
        Edit($"{name}.{field} = {value?.ToString() ?? "default"}", () => Put(value), () => Put(old));
    }

    public static bool HasMaterial(string name) => MaterialsObj[name] is JsonObject;

    static string? Str(JsonNode? n) => n is JsonValue v && v.TryGetValue(out string? s) ? s : null;
    static int? Int(JsonNode? n) => n is JsonValue v && v.TryGetValue(out int i) ? i : null;

    static float Num(JsonObject o, string field)
        => o[field] is JsonValue v && v.TryGetValue(out double d) ? (float)d : 0f;

    public static void AddMaterial(string name)
    {
        if (string.IsNullOrWhiteSpace(name) || HasMaterial(name)) return;
        Edit($"add material {name}",
            () => MaterialsObj[name] = new JsonObject { ["reflectivity"] = 0.3, ["f0"] = 0.04 },
            () => MaterialsObj.Remove(name));
    }

    /// <summary>Removing a material leaves the tiles that name it naming nothing, and
    /// they are kept: undo puts it back and they resolve again.</summary>
    public static void RemoveMaterial(string name)
    {
        if (MaterialsObj[name] is not JsonObject o) return;
        var copy = o.DeepClone();
        Edit($"remove material {name}", () => MaterialsObj.Remove(name), () => MaterialsObj[name] = copy.DeepClone());
    }

    public static float GetField(string name, string field)
        => MaterialsObj[name] is JsonObject o ? Num(o, field) : 0f;

    public static string? GetText(string name, string field)
        => MaterialsObj[name] is JsonObject o ? Str(o[field]) : null;

    /// <summary>A text field change, or its removal with null.</summary>
    public static void SetText(string name, string field, string? value)
    {
        if (MaterialsObj[name] is not JsonObject) return;
        string? old = GetText(name, field);
        if (old == value) return;
        void Put(string? v)
        {
            if (MaterialsObj[name] is not JsonObject o) return;
            if (v == null) o.Remove(field);
            else o[field] = v;
        }
        Edit($"{name}.{field} = {value ?? "default"}", () => Put(value), () => Put(old));
    }

    /// <summary>A field change. <paramref name="from"/> is what an undo returns to, so a
    /// slider dragged over many frames is one entry.</summary>
    public static void SetField(string name, string field, float value, float? from = null)
    {
        if (MaterialsObj[name] is not JsonObject) return;
        float old = from ?? GetField(name, field);
        Edit($"{name}.{field} = {value:0.###}",
            () => ((JsonObject)MaterialsObj[name]!)[field] = Math.Round(value, 4),
            () => { if (MaterialsObj[name] is JsonObject o) o[field] = Math.Round(old, 4); });
    }

    /// <summary>A live change while a slider is held, with no undo entry of its own.</summary>
    public static void Preview(string name, string field, float value)
    {
        if (MaterialsObj[name] is not JsonObject o) return;
        o[field] = Math.Round(value, 4);
        Dirty = true;
        Version++;
    }

    public static Vector3 GetColour(string name, string field)
        => MaterialsObj[name] is JsonObject o ? Vec(o[field], Vector3.One) : Vector3.One;

    /// <summary>A colour field change; <paramref name="from"/> as for <see cref="SetField"/>.</summary>
    public static void SetColourField(string name, string field, Vector3 value, Vector3? from = null)
    {
        if (MaterialsObj[name] is not JsonObject) return;
        var old = from ?? GetColour(name, field);
        Edit($"{name}.{field}",
            () => ((JsonObject)MaterialsObj[name]!)[field] = Arr(value),
            () => { if (MaterialsObj[name] is JsonObject o) o[field] = Arr(old); });
    }

    public static void PreviewColour(string name, string field, Vector3 value)
    {
        if (MaterialsObj[name] is not JsonObject o) return;
        o[field] = Arr(value);
        Dirty = true;
        Version++;
    }

    // ---- surfaces ----------------------------------------------------------

    /// <summary>An area's surfaces document, made on first write.</summary>
    static JsonObject SurfacesDoc(int area, string fingerprint) => AreaDoc(_set.Surfaces, "tiles", area, fingerprint);

    static JsonObject AreaDoc(Dictionary<int, JsonObject> docs, string collection, int area, string fingerprint)
    {
        if (docs.TryGetValue(area, out var doc)) return doc;
        doc = new JsonObject
        {
            ["formatVersion"] = FormatVersion,
            ["area"] = area,
            ["fingerprint"] = fingerprint,
            [collection] = new JsonArray(),
        };
        docs[area] = doc;
        return doc;
    }

    /// <summary>The areas the pack holds documents for.</summary>
    public static IEnumerable<int> Areas() => _set.Surfaces.Keys.Union(_set.Lights.Keys).Union(_set.Atmosphere.Keys).Order();

    /// <summary>The fingerprint an area's documents were authored against, or null. Each
    /// document carries its own; the first that names one answers.</summary>
    public static string? AreaFingerprint(int area)
        => (_set.Surfaces.TryGetValue(area, out var d) ? Str(d["fingerprint"]) : null)
        ?? (_set.Lights.TryGetValue(area, out var l) ? Str(l["fingerprint"]) : null)
        ?? (_set.Atmosphere.TryGetValue(area, out var a) ? Str(a["fingerprint"]) : null);

    public static IEnumerable<(TileKey Key, string Material)> Tiles(int area)
    {
        if (!_set.Surfaces.TryGetValue(area, out var doc) || doc["tiles"] is not JsonArray tiles) yield break;
        foreach (var n in tiles)
        {
            if (n is not JsonObject t) continue;
            if (t["x"] is not JsonValue xv || !xv.TryGetValue(out int x)) continue;
            if (t["z"] is not JsonValue zv || !zv.TryGetValue(out int z)) continue;
            int half = Str(t["half"]) == "upper" ? TileKey.Upper : TileKey.Lower;
            if (t["material"] is not JsonValue mv || !mv.TryGetValue(out string? m) || m == null) continue;
            yield return (new TileKey(area, x, z, half), m);
        }
    }

    public static string? TileMaterial(TileKey k)
    {
        foreach (var (key, m) in Tiles(k.Area))
            if (key == k) return m;
        return null;
    }

    /// <summary>Assign a material to a tile half, or clear it with null.</summary>
    public static void SetTile(TileKey k, string? material, string fingerprint)
    {
        string? old = TileMaterial(k);
        if (old == material) return;
        Edit($"{k} = {material ?? "none"}",
            () => PutTile(k, material, fingerprint),
            () => PutTile(k, old, fingerprint));
    }

    static void PutTile(TileKey k, string? material, string fingerprint)
    {
        var tiles = (JsonArray)SurfacesDoc(k.Area, fingerprint)["tiles"]!;
        var t = FindTile(tiles, k);
        if (t == null)
        {
            if (material != null)
                tiles.Add(new JsonObject { ["x"] = k.X, ["z"] = k.Z, ["half"] = k.HalfName, ["material"] = material });
            return;
        }
        if (material == null) t.Remove("material");
        else t["material"] = material;
        Prune(tiles, t);
    }

    static JsonObject? FindTile(JsonArray tiles, TileKey k)
    {
        foreach (var n in tiles)
            if (n is JsonObject t && Int(t["x"]) == k.X && Int(t["z"]) == k.Z
                && (Str(t["half"]) == "upper" ? 1 : 0) == k.Half)
                return t;
        return null;
    }

    static JsonObject? FindMesh(JsonArray meshes, int mesh)
    {
        foreach (var n in meshes)
            if (n is JsonObject o && Int(o["mesh"]) == mesh) return o;
        return null;
    }

    /// <summary>An entry with nothing left in it goes, and an empty face list with its mesh.</summary>
    static void Prune(JsonArray list, JsonObject e)
    {
        bool tile = e["x"] != null;
        if (e["faces"] is JsonObject f && f.Count == 0)
        {
            e.Remove("faces");
            // A tile entry's mesh belongs to its face list; a mesh entry's is its key.
            if (tile) { e.Remove("mesh"); e.Remove("meshHash"); }
        }
        if (e["material"] == null && e["faces"] == null) list.Remove(e);
    }

    // ---- faces -------------------------------------------------------------

    public readonly record struct TileFaces(TileKey Tile, int Mesh, string? Hash, List<(int Face, string Material)> Faces);
    public readonly record struct MeshRule(int Mesh, string? Hash, string? Material, List<(int Face, string Material)> Faces);

    static List<(int, string)> FaceList(JsonNode? node)
    {
        var list = new List<(int, string)>();
        if (node is not JsonObject o) return list;
        foreach (var (key, v) in o)
            if (int.TryParse(key, out int f) && Str(v) is { } m) list.Add((f, m));
        return list;
    }

    /// <summary>The face lists authored on tile halves.</summary>
    public static IEnumerable<TileFaces> TileFaceLists(int area)
    {
        if (!_set.Surfaces.TryGetValue(area, out var doc) || doc["tiles"] is not JsonArray tiles) yield break;
        foreach (var n in tiles)
        {
            if (n is not JsonObject t || t["faces"] is not JsonObject) continue;
            if (Int(t["x"]) is not { } x || Int(t["z"]) is not { } z || Int(t["mesh"]) is not { } mesh) continue;
            int half = Str(t["half"]) == "upper" ? TileKey.Upper : TileKey.Lower;
            yield return new TileFaces(new TileKey(area, x, z, half), mesh, Str(t["meshHash"]), FaceList(t["faces"]));
        }
    }

    /// <summary>The area-wide rules, by mesh.</summary>
    public static IEnumerable<MeshRule> MeshRules(int area)
    {
        if (!_set.Surfaces.TryGetValue(area, out var doc) || doc["meshes"] is not JsonArray meshes) yield break;
        foreach (var n in meshes)
            if (n is JsonObject o && Int(o["mesh"]) is { } mesh)
                yield return new MeshRule(mesh, Str(o["meshHash"]), Str(o["material"]), FaceList(o["faces"]));
    }

    /// <summary>The material a face is given at one scope: on its tile half, or on its
    /// mesh area-wide. Null when that scope names none.</summary>
    public static string? FaceMaterial(FaceRef f, bool meshScope)
    {
        if (!_set.Surfaces.TryGetValue(f.Tile.Area, out var doc)) return null;
        JsonObject? e = meshScope
            ? doc["meshes"] is JsonArray ms ? FindMesh(ms, f.Mesh) : null
            : doc["tiles"] is JsonArray ts ? FindTile(ts, f.Tile) : null;
        if (e == null || Int(e["mesh"]) != f.Mesh || e["faces"] is not JsonObject faces) return null;
        return Str(faces[f.Face.ToString()]);
    }

    /// <summary>The material a mesh is given everywhere in the area, or null.</summary>
    public static string? MeshMaterial(int area, int mesh)
        => _set.Surfaces.TryGetValue(area, out var doc) && doc["meshes"] is JsonArray ms && FindMesh(ms, mesh) is { } e
            ? Str(e["material"]) : null;

    /// <summary>Give faces a material, or clear them with null, as one undo entry. On the
    /// half's mesh, or with <paramref name="meshScope"/> on every tile of the area using
    /// that mesh. A face list authored on another mesh, or against another hash of this
    /// one, is replaced rather than merged.</summary>
    public static void SetFaces(IReadOnlyList<FaceRef> faces, string? material, bool meshScope,
                                Func<int, string?> hashOf, string fingerprint)
    {
        if (faces.Count == 0) return;
        int area = faces[0].Tile.Area;
        string label = $"{(faces.Count == 1 ? faces[0].ToString() : $"{faces.Count} faces")}" +
                       $"{(meshScope ? " (mesh)" : "")} = {material ?? "none"}";
        EditArea(area, fingerprint, label, doc =>
        {
            foreach (var f in faces)
            {
                string? hash = hashOf(f.Mesh);
                if (hash == null) continue;
                JsonArray list;
                JsonObject? e;
                if (meshScope)
                {
                    list = doc["meshes"] as JsonArray ?? (JsonArray)(doc["meshes"] = new JsonArray());
                    e = FindMesh(list, f.Mesh);
                    if (e == null)
                    {
                        if (material == null) continue;
                        list.Add(e = new JsonObject { ["mesh"] = f.Mesh, ["meshHash"] = hash });
                    }
                }
                else
                {
                    list = (JsonArray)doc["tiles"]!;
                    e = FindTile(list, f.Tile);
                    if (e == null)
                    {
                        if (material == null) continue;
                        list.Add(e = new JsonObject { ["x"] = f.Tile.X, ["z"] = f.Tile.Z, ["half"] = f.Tile.HalfName });
                    }
                }
                if (Int(e["mesh"]) != f.Mesh || Str(e["meshHash"]) != hash || e["faces"] is not JsonObject)
                {
                    e["mesh"] = f.Mesh;
                    e["meshHash"] = hash;
                    e["faces"] = new JsonObject();
                }
                var fo = (JsonObject)e["faces"]!;
                if (material == null) fo.Remove(f.Face.ToString());
                else fo[f.Face.ToString()] = material;
                Prune(list, e);
            }
        });
    }

    /// <summary>Give a mesh a material wherever the area uses it, or clear it.</summary>
    public static void SetMeshMaterial(int area, int mesh, string hash, string? material, string fingerprint)
    {
        if (MeshMaterial(area, mesh) == material) return;
        EditArea(area, fingerprint, $"mesh {mesh} = {material ?? "none"}", doc =>
        {
            var list = doc["meshes"] as JsonArray ?? (JsonArray)(doc["meshes"] = new JsonArray());
            var e = FindMesh(list, mesh);
            if (e == null)
            {
                if (material == null) return;
                list.Add(e = new JsonObject { ["mesh"] = mesh, ["meshHash"] = hash });
            }
            if (Str(e["meshHash"]) != hash) { e["meshHash"] = hash; e.Remove("faces"); }
            if (material == null) e.Remove("material");
            else e["material"] = material;
            Prune(list, e);
        });
    }

    // ---- textures ----------------------------------------------------------

    /// <summary>A material for every face that draws a piece of art, in every area:
    /// <c>remaster/textures.json</c>. A key with no CLUT hash matches the art under any
    /// palette. Keyed on content, so it needs no area fingerprint.</summary>
    public readonly record struct TextureRule(TexKey Key, string Material, string? Note);

    static JsonObject NewTextures() => new() { ["formatVersion"] = FormatVersion, ["textures"] = new JsonArray() };

    static JsonArray TexturesArr => (JsonArray)_set.Textures["textures"]!;

    static TexKey? KeyOf(JsonObject o)
    {
        if (Str(o["index"]) is not { } i) return null;
        return TexKey.TryParse(Str(o["clut"]) is { } c ? $"texture:{i}:{c}" : $"texture:{i}", out var k) ? k : null;
    }

    static JsonObject? FindTexture(TexKey k)
    {
        foreach (var n in TexturesArr)
            if (n is JsonObject o && KeyOf(o) == k) return o;
        return null;
    }

    public static IEnumerable<TextureRule> TextureRules()
    {
        foreach (var n in TexturesArr)
            if (n is JsonObject o && KeyOf(o) is { } k && Str(o["material"]) is { } m)
                yield return new TextureRule(k, m, Str(o["note"]));
    }

    public static string? TextureMaterial(TexKey k) => FindTexture(k) is { } e ? Str(e["material"]) : null;

    /// <summary>Give a piece of art a material everywhere, or clear it.</summary>
    public static void SetTextureMaterial(TexKey k, string? material, string? note = null)
    {
        if (TextureMaterial(k) == material) return;
        var before = (JsonObject)_set.Textures.DeepClone();
        Edit($"{k} = {material ?? "none"}", () =>
        {
            var e = FindTexture(k);
            if (e == null)
            {
                if (material == null) return;
                var o = new JsonObject { ["index"] = k.Index.ToString("x16") };
                if (k.Clut != 0) o["clut"] = k.Clut.ToString("x16");
                if (note != null) o["note"] = note;
                TexturesArr.Add(e = o);
            }
            if (material == null) TexturesArr.Remove(e);
            else e["material"] = material;
        }, () => _set.Textures = (JsonObject)before.DeepClone());
    }

    // ---- models ------------------------------------------------------------

    /// <summary>A material for every draw of one model in the area: the table it comes
    /// out of (a creature and an object may share an id and not a mesh) and its id.</summary>
    public readonly record struct ModelRule(ModelKey Model, string Material);

    static JsonObject? FindModel(JsonArray models, ModelKey k)
    {
        foreach (var n in models)
            if (n is JsonObject o && Int(o["model"]) == k.Model && Str(o["kind"]) == k.KindName) return o;
        return null;
    }

    public static IEnumerable<ModelRule> ModelRules(int area)
    {
        if (!_set.Surfaces.TryGetValue(area, out var doc) || doc["models"] is not JsonArray models) yield break;
        foreach (var n in models)
            if (n is JsonObject o && Int(o["model"]) is { } id && ModelKey.ParseKind(Str(o["kind"])) is { } kind
                && Str(o["material"]) is { } m)
                yield return new ModelRule(new ModelKey(area, kind, id), m);
    }

    public static string? ModelMaterial(ModelKey k)
        => _set.Surfaces.TryGetValue(k.Area, out var doc) && doc["models"] is JsonArray ms && FindModel(ms, k) is { } e
            ? Str(e["material"]) : null;

    /// <summary>Give a model a material wherever the area draws it, or clear it.</summary>
    public static void SetModelMaterial(ModelKey k, string? material, string fingerprint)
    {
        if (ModelMaterial(k) == material) return;
        EditArea(k.Area, fingerprint, $"{k} = {material ?? "none"}", doc =>
        {
            var list = doc["models"] as JsonArray ?? (JsonArray)(doc["models"] = new JsonArray());
            var e = FindModel(list, k);
            if (e == null)
            {
                if (material == null) return;
                list.Add(e = new JsonObject { ["kind"] = k.KindName, ["model"] = k.Model });
            }
            if (material == null) list.Remove(e);
            else e["material"] = material;
            if (list.Count == 0) doc.Remove("models");
        });
    }

    /// <summary>An edit to one area's document, undone by putting the document back.</summary>
    static void EditArea(int area, string fingerprint, string label, Action<JsonObject> change,
                         bool lights = false)
    {
        var docs = lights ? _set.Lights : _set.Surfaces;
        var before = docs.TryGetValue(area, out var d) ? (JsonObject)d.DeepClone() : null;
        Edit(label,
            () => change(AreaDoc(lights ? _set.Lights : _set.Surfaces, lights ? "lights" : "tiles", area, fingerprint)),
            () =>
            {
                var now = lights ? _set.Lights : _set.Surfaces;
                if (before == null) now.Remove(area);
                else now[area] = (JsonObject)before.DeepClone();
            });
    }

    // ---- lights ------------------------------------------------------------

    /// <summary>One authored light, as the document holds it. Position is world units,
    /// up at -Y; colour is linear 0..1 per channel; cone is inner and outer half-angles
    /// in degrees; flicker scales the intensity by up to <c>FlickerAmount</c>, varying
    /// at about <c>FlickerHz</c>. A light casts shadows unless <c>"shadows": false</c>.</summary>
    public readonly record struct Light(
        string Name, bool Spot, Vector3 Position, Vector3 Colour, float Intensity, float Radius,
        Vector3 Direction, float ConeInner, float ConeOuter, float FlickerAmount, float FlickerHz, bool Off,
        bool Shadows = true);

    static Vector3 Vec(JsonNode? n, Vector3 fallback)
    {
        if (n is not JsonArray a || a.Count < 3) return fallback;
        float C(int i) => a[i] is JsonValue v && v.TryGetValue(out double d) ? (float)d : 0f;
        return new Vector3(C(0), C(1), C(2));
    }

    static float NumOr(JsonNode? n, float fallback)
        => n is JsonValue v && v.TryGetValue(out double d) ? (float)d : fallback;

    static JsonObject? LightsDoc(int area) => _set.Lights.TryGetValue(area, out var d) ? d : null;

    static JsonObject? FindLight(int area, string name)
    {
        if (LightsDoc(area)?["lights"] is not JsonArray list) return null;
        foreach (var n in list)
            if (n is JsonObject o && Str(o["name"]) == name) return o;
        return null;
    }

    static Light ParseLight(JsonObject o, string name)
    {
        var cone = o["cone"] as JsonArray;
        var flicker = o["flicker"] as JsonObject;
        return new Light(
            name,
            Str(o["type"]) == "spot",
            Vec(o["position"], Vector3.Zero),
            Vec(o["colour"], Vector3.One),
            NumOr(o["intensity"], 1f),
            NumOr(o["radius"], 4096f),
            Vec(o["direction"], new Vector3(0, 1, 0)),
            NumOr(cone?.Count > 0 ? cone[0] : null, 20f),
            NumOr(cone?.Count > 1 ? cone[1] : null, 35f),
            NumOr(flicker?["amount"], 0f),
            NumOr(flicker?["hz"], 0f),
            o["enabled"] is JsonValue ev && ev.TryGetValue(out bool en) && !en,
            !(o["shadows"] is JsonValue sv && sv.TryGetValue(out bool sh) && !sh));
    }

    /// <summary>The area's lights, in document order.</summary>
    public static IEnumerable<Light> Lights(int area)
    {
        if (LightsDoc(area)?["lights"] is not JsonArray list) yield break;
        foreach (var n in list)
            if (n is JsonObject o && Str(o["name"]) is { } name)
                yield return ParseLight(o, name);
    }

    public static Light? GetLight(int area, string name)
        => FindLight(area, name) is { } o ? ParseLight(o, name) : null;

    /// <summary>A name not yet used in the area.</summary>
    public static string FreeLightName(int area, string stem = "light")
    {
        for (int i = 1; ; i++)
            if (FindLight(area, $"{stem} {i}") == null) return $"{stem} {i}";
    }

    static JsonArray Arr(Vector3 v) => new(Math.Round(v.X, 3), Math.Round(v.Y, 3), Math.Round(v.Z, 3));

    public static bool AddLight(int area, string fingerprint, string name, Vector3 position)
    {
        if (string.IsNullOrWhiteSpace(name) || FindLight(area, name) != null) return false;
        EditArea(area, fingerprint, $"add light {name}", doc => ((JsonArray)doc["lights"]!).Add(new JsonObject
        {
            ["name"] = name,
            ["type"] = "point",
            ["position"] = Arr(new Vector3(MathF.Round(position.X), MathF.Round(position.Y), MathF.Round(position.Z))),
            ["colour"] = Arr(new Vector3(1f, 0.62f, 0.3f)),
            ["intensity"] = 1.0,
            ["radius"] = 4096,
        }), lights: true);
        return true;
    }

    public static void RemoveLight(int area, string name)
    {
        if (FindLight(area, name) == null) return;
        EditArea(area, AreaFingerprint(area) ?? "", $"remove light {name}", doc =>
        {
            var list = (JsonArray)doc["lights"]!;
            foreach (var n in list)
                if (n is JsonObject o && Str(o["name"]) == name) { list.Remove(o); break; }
        }, lights: true);
    }

    /// <summary>The light's document fields as they stand, for an undo to return to.</summary>
    public static JsonObject? LightSnapshot(int area, string name) => FindLight(area, name)?.DeepClone() as JsonObject;

    /// <summary>A change to one light as one undo entry. <paramref name="before"/> is what
    /// undo puts back, so a gizmo drag or a slider held over many frames is one entry.</summary>
    public static void SetLight(int area, string name, string label, Action<JsonObject> change, JsonObject? before = null)
    {
        var cur = FindLight(area, name);
        if (cur == null) return;
        var from = before ?? (JsonObject)cur.DeepClone();
        if (before != null) Put(area, name, before);
        Edit($"{name}: {label}", () => { if (FindLight(area, name) is { } o) change(o); },
            () => Put(area, name, from));
    }

    /// <summary>What previews did to a light since <paramref name="before"/>, as one undo entry.</summary>
    public static void CommitLight(int area, string name, string label, JsonObject before)
    {
        if (FindLight(area, name) is not { } cur) return;
        var after = (JsonObject)cur.DeepClone();
        Edit($"{name}: {label}", () => Put(area, name, after), () => Put(area, name, before));
    }

    /// <summary>A live change with no undo entry of its own.</summary>
    public static void PreviewLight(int area, string name, Action<JsonObject> change)
    {
        if (FindLight(area, name) is not { } o) return;
        change(o);
        Dirty = true;
        Version++;
    }

    /// <summary>Replace a light's fields with a snapshot, keeping its place in the list.</summary>
    static void Put(int area, string name, JsonObject snapshot)
    {
        if (LightsDoc(area)?["lights"] is not JsonArray list) return;
        for (int i = 0; i < list.Count; i++)
            if (list[i] is JsonObject o && Str(o["name"]) == name)
            {
                list[i] = snapshot.DeepClone();
                return;
            }
    }

    public static void SetPosition(JsonObject o, Vector3 p)
        => o["position"] = Arr(new Vector3(MathF.Round(p.X), MathF.Round(p.Y), MathF.Round(p.Z)));

    public static void SetColour(JsonObject o, Vector3 c) => o["colour"] = Arr(c);

    public static void SetDirection(JsonObject o, Vector3 d)
        => o["direction"] = Arr(d.LengthSquared() > 1e-8f ? Vector3.Normalize(d) : new Vector3(0, 1, 0));

    public static void SetNumber(JsonObject o, string field, float v) => o[field] = Math.Round(v, 4);

    public static void SetCone(JsonObject o, float inner, float outer)
    {
        outer = Math.Clamp(outer, 1f, 89f);
        inner = Math.Clamp(inner, 0f, outer);
        o["cone"] = new JsonArray(Math.Round(inner, 2), Math.Round(outer, 2));
    }

    public static void SetFlicker(JsonObject o, float amount, float hz)
    {
        if (amount <= 0f) { o.Remove("flicker"); return; }
        o["flicker"] = new JsonObject { ["amount"] = Math.Round(Math.Clamp(amount, 0f, 1f), 3), ["hz"] = Math.Round(Math.Max(hz, 0f), 2) };
    }

    // ---- atmosphere ----------------------------------------------------------

    /// <summary>
    /// An override of one of the area's light records (<c>0x801930F0 + 0x68 * Record</c>),
    /// as the document holds it; every part is optional and a missing one leaves the
    /// game's. <c>Back</c> is the back colour's three bytes; light <c>j</c> is row
    /// <c>j</c> of the light matrix (the way a face it lights fully faces) and column
    /// <c>j</c> of the colour matrix, both in the GTE's 4.12 units as floats (1.0 is
    /// 4096); <c>Fog</c> is the record's fog word: the fog starts at <c>(fog &amp;
    /// 0x7FFF) / 2</c> view units, bit <c>0x8000</c> picks the linear curve, and 32000
    /// or more draws no fog. <c>Hash</c> is the record's own bytes where the area
    /// keeps them, as authored; a record the game has since changed is refused.
    /// </summary>
    public readonly record struct RecordOverride(
        int Record, string? Hash, int[]? Back, Vector3?[] Direction, Vector3?[] Colour, int? Fog,
        float? Darkness = null);

    /// <summary>The area's own entry, <c>"record": "all"</c>, which holds only its
    /// <c>darkness</c>: 0 the game's light, 1 black, scaling every tile record's back
    /// colour and light colours after its own override. It carries no record hash; the
    /// area's fingerprint is its gate.</summary>
    public const int AllRecords = -1;

    static JsonObject? AtmosDoc(int area) => _set.Atmosphere.TryGetValue(area, out var d) ? d : null;

    static bool Matches(JsonObject o, int record)
        => record == AllRecords ? Str(o["record"]) == "all" : Int(o["record"]) == record;

    static JsonObject NewRecord(int record, string hash)
        => record == AllRecords ? new JsonObject { ["record"] = "all" } : new JsonObject { ["record"] = record, ["recordHash"] = hash };

    /// <summary>Nothing overridden: only the record's name and hash.</summary>
    static bool Empty(JsonObject o) => o.All(kv => kv.Key is "record" or "recordHash");

    static JsonObject? FindRecord(int area, int record)
    {
        if (AtmosDoc(area)?["records"] is not JsonArray list) return null;
        foreach (var n in list)
            if (n is JsonObject o && Matches(o, record)) return o;
        return null;
    }

    static RecordOverride ParseRecord(JsonObject o, int record)
    {
        int[]? back = null;
        if (o["back"] is JsonArray b && b.Count >= 3)
            back = [.. Enumerable.Range(0, 3).Select(i => Math.Clamp(Int(b[i]) ?? (int)NumOr(b[i], 0f), 0, 255))];
        var dir = new Vector3?[3];
        var col = new Vector3?[3];
        if (o["lights"] is JsonArray ls)
            for (int j = 0; j < 3 && j < ls.Count; j++)
                if (ls[j] is JsonObject l)
                {
                    if (l["direction"] is JsonArray) dir[j] = Vec(l["direction"], Vector3.Zero);
                    if (l["colour"] is JsonArray) col[j] = Vec(l["colour"], Vector3.Zero);
                }
        if (record == AllRecords)
            return new RecordOverride(record, null, null, new Vector3?[3], new Vector3?[3], null,
                o["darkness"] is JsonValue dv && dv.TryGetValue(out double dd) ? Math.Clamp((float)dd, 0f, 1f) : null);
        return new RecordOverride(record, Str(o["recordHash"]), back, dir, col, Int(o["fog"]));
    }

    /// <summary>The area's record overrides, in document order; not the whole-area one.</summary>
    public static IEnumerable<RecordOverride> Records(int area)
    {
        if (AtmosDoc(area)?["records"] is not JsonArray list) yield break;
        foreach (var n in list)
            if (n is JsonObject o && Int(o["record"]) is int r)
                yield return ParseRecord(o, r);
    }

    public static RecordOverride? GetRecord(int area, int record)
        => FindRecord(area, record) is { } o ? ParseRecord(o, record) : null;

    public static JsonObject? RecordSnapshot(int area, int record) => FindRecord(area, record)?.DeepClone() as JsonObject;

    /// <summary>A change to one record's override as one undo entry, the override made
    /// on first write with the record's hash; an override left with nothing in it is
    /// removed.</summary>
    public static void SetRecord(int area, int record, string hash, string fingerprint, string label,
                                 Action<JsonObject> change)
    {
        var from = RecordSnapshot(area, record);
        Edit($"{RecordName(record)}: {label}", () =>
            {
                var o = FindRecord(area, record);
                if (o == null)
                {
                    o = NewRecord(record, hash);
                    ((JsonArray)AreaDoc(_set.Atmosphere, "records", area, fingerprint)["records"]!).Add(o);
                }
                change(o);
                if (Empty(o)) PutRecord(area, record, null, fingerprint);
            },
            () => PutRecord(area, record, from, fingerprint));
    }

    /// <summary>A live change with no undo entry of its own; <see cref="CommitRecord"/> makes one.</summary>
    public static void PreviewRecord(int area, int record, string hash, string fingerprint, Action<JsonObject> change)
    {
        var o = FindRecord(area, record);
        if (o == null)
        {
            o = NewRecord(record, hash);
            ((JsonArray)AreaDoc(_set.Atmosphere, "records", area, fingerprint)["records"]!).Add(o);
        }
        change(o);
        Dirty = true;
        Version++;
    }

    public static void CommitRecord(int area, int record, string fingerprint, string label, JsonObject? before)
    {
        var after = RecordSnapshot(area, record);
        if (after != null && Empty(after)) after = null;
        Edit($"{RecordName(record)}: {label}", () => PutRecord(area, record, after, fingerprint),
            () => PutRecord(area, record, before, fingerprint));
    }

    public static void RemoveRecord(int area, int record)
    {
        if (FindRecord(area, record) == null) return;
        var before = RecordSnapshot(area, record);
        string fp = AreaFingerprint(area) ?? "";
        Edit($"{RecordName(record)}: reset", () => PutRecord(area, record, null, fp), () => PutRecord(area, record, before, fp));
    }

    /// <summary>Replace a record's override with a snapshot, or remove it (null),
    /// keeping its place in the list.</summary>
    static void PutRecord(int area, int record, JsonObject? snapshot, string fingerprint)
    {
        var list = (JsonArray)AreaDoc(_set.Atmosphere, "records", area, fingerprint)["records"]!;
        for (int i = 0; i < list.Count; i++)
            if (list[i] is JsonObject o && Matches(o, record))
            {
                if (snapshot == null) list.RemoveAt(i);
                else list[i] = snapshot.DeepClone();
                return;
            }
        if (snapshot != null) list.Add(snapshot.DeepClone());
    }

    public static string RecordName(int record) => record == AllRecords ? "area" : $"record {record}";

    /// <summary>The area's darkness; 0 or null removes it, leaving the game's light.</summary>
    public static void SetDarkness(JsonObject o, float? v)
    {
        if (v is { } x && x > 0.0005f) o["darkness"] = Math.Round(Math.Clamp(x, 0f, 1f), 3);
        else o.Remove("darkness");
    }

    public static void SetBack(JsonObject o, int[]? rgb)
    {
        if (rgb == null) { o.Remove("back"); return; }
        o["back"] = new JsonArray(Math.Clamp(rgb[0], 0, 255), Math.Clamp(rgb[1], 0, 255), Math.Clamp(rgb[2], 0, 255));
    }

    /// <summary>Light <paramref name="j"/>'s direction or colour; null leaves the game's.</summary>
    public static void SetRecordLight(JsonObject o, int j, string field, Vector3? v)
    {
        if (o["lights"] is not JsonArray ls) o["lights"] = ls = new JsonArray();
        while (ls.Count < 3) ls.Add(null);
        if (ls[j] is not JsonObject l) ls[j] = l = new JsonObject();
        if (v is { } x)
            l[field] = new JsonArray(Math.Round(Math.Clamp(x.X, -7.999f, 7.999f), 4),
                                     Math.Round(Math.Clamp(x.Y, -7.999f, 7.999f), 4),
                                     Math.Round(Math.Clamp(x.Z, -7.999f, 7.999f), 4));
        else l.Remove(field);
        if (l.Count == 0) ls[j] = null;
        if (ls.All(n => n == null)) o.Remove("lights");
    }

    public static void SetFog(JsonObject o, int? word)
    {
        if (word is int w) o["fog"] = Math.Clamp(w, 0, 0xFFFF);
        else o.Remove("fog");
    }

    // ---- undo --------------------------------------------------------------

    sealed record Entry(string Label, Action Apply, Action Revert);

    static readonly Stack<Entry> _undo = new(), _redo = new();

    public static string? UndoLabel => _undo.TryPeek(out var e) ? e.Label : null;
    public static string? RedoLabel => _redo.TryPeek(out var e) ? e.Label : null;

    static void Edit(string label, Action apply, Action revert)
    {
        apply();
        _undo.Push(new Entry(label, apply, revert));
        _redo.Clear();
        Dirty = true;
        Version++;
    }

    public static bool Undo()
    {
        if (!_undo.TryPop(out var e)) return false;
        e.Revert();
        _redo.Push(e);
        Dirty = true;
        Version++;
        return true;
    }

    public static bool Redo()
    {
        if (!_redo.TryPop(out var e)) return false;
        e.Apply();
        _undo.Push(e);
        Dirty = true;
        Version++;
        return true;
    }
}
