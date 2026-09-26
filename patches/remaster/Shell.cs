using System.Globalization;
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using RecompOne.Runtime;

namespace Kf2.Remaster;

/// <summary>
/// The remaster's <c>KF2_SHELL</c> verbs, run on the game thread from
/// <see cref="AgentServer"/>'s VSync queue. They go through the same document calls
/// the panel does, so the undo stack sees them:
///
///     edit [on|off|toggle]                          the editor, which pauses the world
///     select [here | tile:A:X:Z:lower|upper | model:A:KIND:ID | pick GX GY [add] | faces F,F,... | grow connected|texture|mesh]
///     set selected|tile:...|model:... material NAME|none [tile|mesh]
///     set material:NAME reflectivity|f0|roughness|metalness|specular|occlusion VALUE
///     set material:NAME emissiveStrength|light|glowRadius|pulseAmount|pulseHz VALUE
///     set material:NAME emissive R G B
///     set material:NAME glowMode additive|lit, glowFog on|off, pulseStyle breathe|flicker
///     set remaster on|off
///     pack save|reload|undo|redo|list|add NAME
///     light list|add NAME [here|pick GX GY|X Y Z]|remove NAME|select NAME|set NAME FIELD V...
///     remaster                                      the status, as the probe line has it
/// </summary>
public static class Shell
{
    public static readonly string[] Verbs = ["edit", "select", "set", "pack", "remaster", "light", "textures"];

    public static readonly string[] Help =
    [
        "edit [on|off|toggle] - the remaster editor, which pauses the world",
        "select [here | tile:A:X:Z:lower|upper | model:A:KIND:ID | pick GX GY [add] | faces F,F,... | grow connected|texture|mesh] - " +
            "a half, faces or a model; pick takes the faces or the model under game pixel GX GY (the editor must be open)",
        "set selected|tile:...|model:... material NAME|none [tile|mesh]; " +
            "set material:NAME reflectivity|f0|roughness|metalness|specular|occlusion|emissiveStrength|light|glowRadius|pulseAmount|pulseHz V; set material:NAME emissive R G B; set material:NAME glowMode additive|lit|glowFog on|off|pulseStyle breathe|flicker; set remaster on|off",
        "pack save|reload|undo|redo|list|add NAME - the working pack",
        "light list | add NAME [here | pick GX GY | X Y Z] | remove NAME | select NAME | " +
            "set NAME position X Y Z|colour R G B|intensity V|radius V|type point|spot|direction X Y Z|cone IN OUT|flicker AMOUNT HZ|enabled on|off - " +
            "the area's authored lights; pick places one short of the surface under game pixel GX GY",
        "remaster - area, fingerprint, what is applied",
        "textures [on|off|reset|save] - the texture-key census of this area: keys, art, overlapping rects, what a pack covers; save writes dump/GAME/census/area-N.json",
    ];

    public static string Run(string verb, string args)
    {
        var a = args.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        try
        {
            return verb switch
            {
                "edit" => Edit(a),
                "select" => Select(a),
                "set" => Set(a),
                "pack" => PackVerb(a),
                "remaster" => Status(),
                "light" => LightVerb(a),
                "textures" => Ok("textures", TextureCensus.Verb(a)),
                _ => Err(verb, "unknown verb"),
            };
        }
        catch (Exception e) { return Err(verb, e.Message); }
    }

    static string Edit(string[] a)
    {
        string mode = a.Length > 0 ? a[0].ToLowerInvariant() : "toggle";
        bool open = mode switch { "on" => true, "off" => false, "toggle" => !Editor.Open, _ => Editor.Open };
        if (mode is not ("on" or "off" or "toggle")) return Err("edit", "edit on|off|toggle");
        Editor.SetOpen(open);
        return Ok("edit", new JsonObject { ["open"] = open, ["pauses"] = Identity.Area >= 0 });
    }

    static string Select(string[] a)
    {
        var m = Runtime.Mem;
        if (m == null) return Err("select", "not running");
        if (a.Length == 0)
            return Ok("select", Editor.SelectedModel is { } sm ? DescribeModel(sm) : Describe(Editor.Selected));
        if (ModelKey.TryParse(a[0], out var mkey))
        {
            Editor.SelectModel(mkey);
            return Ok("select", DescribeModel(mkey));
        }
        if (a[0] == "here")
        {
            Editor.Select(Identity.PlayerTile(m));
            return Ok("select", Describe(Editor.Selected));
        }
        if (a[0] == "pick")
        {
            if (a.Length < 3 || !float.TryParse(a[1], CultureInfo.InvariantCulture, out float gx)
                || !float.TryParse(a[2], CultureInfo.InvariantCulture, out float gy))
                return Err("select", "select pick GX GY [add]");
            if (!Faces.Recording) return Err("select", "the editor is closed (edit on), so no triangles are recorded");
            var hit = Faces.PickAt(new Vector2(gx, gy), out var model, out var why);
            if (model is { } mk)
            {
                Editor.SelectModel(mk);
                return Ok("select", DescribeModel(mk));
            }
            if (hit == null) return Err("select", $"nothing picked: {why}");
            Editor.SelectFaces(hit, a.Length > 3 && a[3] == "add");
            return Ok("select", Describe(Editor.Selected));
        }
        if (a[0] == "faces")
        {
            if (Editor.Selected is not { } k0) return Err("select", "select a half first");
            int mesh = Editor.MeshOf(m, k0);
            if (mesh < 0) return Err("select", "the half draws no mesh");
            var list = new List<FaceRef>();
            foreach (var t in (a.Length > 1 ? a[1] : "").Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                if (!int.TryParse(t, out int f) || f < 0 || f >= (Faces.Mesh(m, mesh)?.Length ?? 0))
                    return Err("select", $"no face '{t}' in mesh {mesh}");
                list.Add(new FaceRef(k0, mesh, f));
            }
            if (list.Count == 0) return Err("select", "select faces F,F,...");
            Editor.SelectFaces(list, false);
            return Ok("select", Describe(Editor.Selected));
        }
        if (a[0] == "grow")
        {
            if (Editor.SelectedFaces.Count == 0) return Err("select", "select faces first");
            Editor.Grow how = (a.Length > 1 ? a[1] : "") switch
            {
                "connected" => Editor.Grow.Connected,
                "texture" => Editor.Grow.Texture,
                "mesh" => Editor.Grow.Mesh,
                _ => (Editor.Grow)(-1),
            };
            if ((int)how < 0) return Err("select", "select grow connected|texture|mesh");
            Editor.GrowSelection(m, how);
            return Ok("select", Describe(Editor.Selected));
        }
        if (!TileKey.TryParse(a[0], out var k)) return Err("select", $"cannot read '{a[0]}'");
        Editor.Select(k);
        return Ok("select", Describe(k));
    }

    static string Set(string[] a)
    {
        if (a.Length >= 2 && a[0] == "remaster")
        {
            Host.SetEnabled(a[1] is "on" or "1");
            return Ok("set", new JsonObject { ["remaster"] = Host.Enabled });
        }
        if (a.Length >= 3 && a[0].StartsWith("material:"))
        {
            string name = a[0]["material:".Length..];
            if (!Pack.HasMaterial(name)) return Err("set", $"no material '{name}'");
            if (a[1] == "emissive")
            {
                if (a.Length < 5) return Err("set", "set material:NAME emissive R G B");
                var c = new Vector3(float.Parse(a[2], CultureInfo.InvariantCulture), float.Parse(a[3], CultureInfo.InvariantCulture),
                                    float.Parse(a[4], CultureInfo.InvariantCulture));
                Pack.SetColourField(name, "emissive", Vector3.Clamp(c, Vector3.Zero, Vector3.One));
                var e = Pack.GetColour(name, "emissive");
                return Ok("set", new JsonObject { ["material"] = name, ["emissive"] = new JsonArray(e.X, e.Y, e.Z) });
            }
            if (a[1] == "glowMode")
            {
                if (a[2] is not ("additive" or "lit")) return Err("set", "glowMode additive|lit");
                Pack.SetText(name, "glowMode", a[2] == "lit" ? "lit" : null);
                return Ok("set", new JsonObject { ["material"] = name, ["glowMode"] = a[2] });
            }
            if (a[1] == "glowFog")
            {
                Pack.SetFlag(name, "glowFog", a[2] is "off" or "0" or "false" ? false : null);
                return Ok("set", new JsonObject { ["material"] = name, ["glowFog"] = a[2] });
            }
            if (a[1] == "pulseStyle")
            {
                if (a[2] is not ("breathe" or "flicker")) return Err("set", "pulseStyle breathe|flicker");
                Pack.SetText(name, "pulseStyle", a[2] == "flicker" ? "flicker" : null);
                return Ok("set", new JsonObject { ["material"] = name, ["pulseStyle"] = a[2] });
            }
            if (a[1] is not ("reflectivity" or "f0" or "roughness" or "metalness" or "specular" or "occlusion"
                          or "emissiveStrength" or "light" or "glowRadius" or "pulseAmount" or "pulseHz"))
                return Err("set", "reflectivity, f0, roughness, metalness, specular, occlusion, emissive, emissiveStrength, " +
                                  "glowMode, glowFog, light, glowRadius, pulseAmount, pulseHz or pulseStyle");
            if (!float.TryParse(a[2], CultureInfo.InvariantCulture, out float v)) return Err("set", $"cannot read '{a[2]}'");
            float max = a[1] switch
            {
                "emissiveStrength" or "light" => 4f, "glowRadius" => Pack.MaxGlowRadius, "pulseHz" => 10f, _ => 1f,
            };
            Pack.SetField(name, a[1], Math.Clamp(v, 0f, max));
            return Ok("set", new JsonObject { ["material"] = name, [a[1]] = Pack.GetField(name, a[1]) });
        }
        if (a.Length >= 3 && a[1] == "material")
        {
            var m = Runtime.Mem;
            if (m == null) return Err("set", "not running");
            if (a[0] != "selected")
            {
                if (ModelKey.TryParse(a[0], out var mk)) Editor.SelectModel(mk);
                else if (TileKey.TryParse(a[0], out var k)) Editor.Select(k);
                else return Err("set", $"cannot read '{a[0]}'");
            }
            if (Editor.Selected == null && Editor.SelectedModel == null) return Err("set", "nothing selected");
            string? name = a[2] == "none" ? null : a[2];
            if (name != null && !Pack.HasMaterial(name)) return Err("set", $"no material '{name}'");
            bool was = Editor.MeshScope;
            Editor.MeshScope = a.Length > 3 && a[3] == "mesh";
            try
            {
                if (Editor.Assign(m, name) is { } why) return Err("set", why);
            }
            finally { Editor.MeshScope = was; }
            return Ok("set", Editor.SelectedModel is { } sm ? DescribeModel(sm) : Describe(Editor.Selected));
        }
        return Err("set", "set selected|tile:...|model:... material NAME|none [tile|mesh]; " +
                          "set material:NAME reflectivity|f0|roughness|metalness|specular|occlusion|emissiveStrength|light|glowRadius|pulseAmount|pulseHz V; set material:NAME emissive R G B; set material:NAME glowMode additive|lit|glowFog on|off|pulseStyle breathe|flicker; set remaster on|off");
    }

    static string PackVerb(string[] a)
    {
        string op = a.Length > 0 ? a[0] : "list";
        switch (op)
        {
            case "save": Pack.Save(); break;
            case "reload": Pack.Load(); break;
            case "undo": if (!Pack.Undo()) return Err("pack", "nothing to undo"); break;
            case "redo": if (!Pack.Redo()) return Err("pack", "nothing to redo"); break;
            case "add":
                if (a.Length < 2) return Err("pack", "pack add NAME");
                Pack.AddMaterial(a[1]);
                break;
            case "list": break;
            default: return Err("pack", "save|reload|undo|redo|list|add NAME");
        }
        var mats = new JsonArray();
        foreach (var mat in Pack.Materials())
            mats.Add(new JsonObject
            {
                ["name"] = mat.Name, ["reflectivity"] = mat.Reflectivity, ["f0"] = mat.F0,
                ["roughness"] = mat.Roughness,
                ["emissive"] = new JsonArray(mat.Emissive.X, mat.Emissive.Y, mat.Emissive.Z),
                ["emissiveStrength"] = mat.EmissiveStrength,
                ["glowMode"] = mat.GlowAdditive ? "additive" : "lit",
                ["glowFog"] = !mat.GlowUnfogged,
                ["light"] = mat.Light,
                ["glowRadius"] = mat.GlowRadius,
                ["pulse"] = new JsonArray(mat.PulseAmount, mat.PulseHz, mat.PulseFlicker ? "flicker" : "breathe"),
                ["metalness"] = mat.Metalness,
                ["specular"] = mat.Specular,
                ["occlusion"] = mat.Occlusion,
                ["id"] = Surfaces.IdOf(mat.Name),
            });
        var tiles = new JsonArray();
        if (Identity.Area >= 0)
        {
            foreach (var (k, name) in Pack.Tiles(Identity.Area)) tiles.Add($"{k} = {name}");
            foreach (var e in Pack.TileFaceLists(Identity.Area))
                foreach (var (f, name) in e.Faces) tiles.Add($"{e.Tile}:{f} (mesh {e.Mesh}) = {name}");
            foreach (var e in Pack.MeshRules(Identity.Area))
            {
                if (e.Material != null) tiles.Add($"mesh {e.Mesh} = {e.Material}");
                foreach (var (f, name) in e.Faces) tiles.Add($"mesh {e.Mesh}:{f} = {name}");
            }
            foreach (var r in Pack.ModelRules(Identity.Area)) tiles.Add($"{r.Model} = {r.Material}");
        }
        return Ok("pack", new JsonObject
        {
            ["root"] = Pack.Root, ["dirty"] = Pack.Dirty, ["error"] = Pack.LastError,
            ["materials"] = mats, ["tiles"] = tiles,
        });
    }

    static string LightVerb(string[] a)
    {
        var m = Runtime.Mem;
        if (m == null) return Err("light", "not running");
        int area = Identity.Area;
        if (area < 0 || !Identity.Settled) return Err("light", "no settled area");
        string op = a.Length > 0 ? a[0] : "list";
        float F(int i) => float.Parse(a[i], CultureInfo.InvariantCulture);
        switch (op)
        {
            case "list":
                return Ok("light", LightList(area));
            case "add":
            {
                if (a.Length < 2) return Err("light", "light add NAME [here|pick GX GY|X Y Z]");
                string name = a[1].Replace('_', ' ');
                Vector3 pos;
                if (a.Length >= 5 && a[2] != "pick") pos = new Vector3(F(2), F(3), F(4));
                else if (a.Length >= 5 && a[2] == "pick")
                {
                    if (!Faces.Recording) return Err("light", "the editor is closed (edit on), so no triangles are recorded");
                    if (Editor.PlaceAt(m, new Vector2(F(3), F(4))) is not { } p) return Err("light", "no surface under that pixel");
                    pos = p;
                }
                else pos = Editor.PlayerLightPosition(m);
                if (!Pack.AddLight(area, Identity.FingerprintText, name, pos)) return Err("light", $"'{name}' exists");
                Editor.SelectLight(name);
                return Ok("light", LightList(area));
            }
            case "remove":
                if (a.Length < 2) return Err("light", "light remove NAME");
                Pack.RemoveLight(area, a[1].Replace('_', ' '));
                return Ok("light", LightList(area));
            case "select":
                if (a.Length < 2 || Pack.GetLight(area, a[1].Replace('_', ' ')) == null) return Err("light", "no such light");
                Editor.SelectLight(a[1].Replace('_', ' '));
                return Ok("light", LightList(area));
            case "set":
            {
                if (a.Length < 4) return Err("light", "light set NAME FIELD V...");
                string name = a[1].Replace('_', ' ');
                if (Pack.GetLight(area, name) is not { } l) return Err("light", $"no light '{name}'");
                string field = a[2];
                System.Action<JsonObject>? change = field switch
                {
                    "position" when a.Length >= 6 => o => Pack.SetPosition(o, new Vector3(F(3), F(4), F(5))),
                    "colour" or "color" when a.Length >= 6 => o => Pack.SetColour(o, new Vector3(F(3), F(4), F(5))),
                    "direction" when a.Length >= 6 => o => Pack.SetDirection(o, new Vector3(F(3), F(4), F(5))),
                    "intensity" => o => Pack.SetNumber(o, "intensity", F(3)),
                    "radius" => o => Pack.SetNumber(o, "radius", Math.Max(F(3), 1f)),
                    "type" when a[3] is "point" or "spot" => o => o["type"] = a[3],
                    "cone" when a.Length >= 5 => o => Pack.SetCone(o, F(3), F(4)),
                    "flicker" when a.Length >= 5 => o => Pack.SetFlicker(o, F(3), F(4)),
                    "enabled" => o => { if (a[3] is "on" or "1") o.Remove("enabled"); else o["enabled"] = false; },
                    _ => null,
                };
                if (change == null) return Err("light", $"cannot set '{field}' from that");
                Pack.SetLight(area, name, $"{field} = {string.Join(' ', a[3..])}", change);
                return Ok("light", LightList(area));
            }
        }
        return Err("light", "light list|add|remove|select|set");
    }

    static JsonObject LightList(int area)
    {
        var list = new JsonArray();
        var m = Runtime.Mem;
        var view = m != null ? Lights.ReadView(m) : default;
        foreach (var l in Pack.Lights(area))
        {
            var o = new JsonObject
            {
                ["name"] = l.Name, ["type"] = l.Spot ? "spot" : "point",
                ["position"] = new JsonArray(l.Position.X, l.Position.Y, l.Position.Z),
                ["colour"] = new JsonArray(l.Colour.X, l.Colour.Y, l.Colour.Z),
                ["intensity"] = l.Intensity, ["radius"] = l.Radius,
            };
            if (l.Spot)
            {
                o["direction"] = new JsonArray(l.Direction.X, l.Direction.Y, l.Direction.Z);
                o["cone"] = new JsonArray(l.ConeInner, l.ConeOuter);
            }
            if (l.FlickerAmount > 0f) o["flicker"] = new JsonArray(l.FlickerAmount, l.FlickerHz);
            if (l.Off) o["enabled"] = false;
            if (m != null && view.Project(l.Position, out var s, out float z))
                o["screen"] = new JsonArray(MathF.Round(s.X, 1), MathF.Round(s.Y, 1), MathF.Round(z));
            list.Add(o);
        }
        var glow = new JsonArray();
        foreach (var l in Lights.DerivedLights)
        {
            var o = new JsonObject
            {
                ["name"] = l.Name,
                ["position"] = new JsonArray(MathF.Round(l.Position.X), MathF.Round(l.Position.Y), MathF.Round(l.Position.Z)),
                ["colour"] = new JsonArray(l.Colour.X, l.Colour.Y, l.Colour.Z),
                ["intensity"] = l.Intensity, ["radius"] = l.Radius,
                ["normal"] = new JsonArray(MathF.Round(l.Direction.X, 3), MathF.Round(l.Direction.Y, 3), MathF.Round(l.Direction.Z, 3)),
            };
            if (m != null && view.Project(l.Position, out var s, out float z))
                o["screen"] = new JsonArray(MathF.Round(s.X, 1), MathF.Round(s.Y, 1), MathF.Round(z));
            glow.Add(o);
        }
        return new JsonObject
        {
            ["area"] = area, ["lights"] = list, ["glow"] = glow, ["modelGlow"] = Lights.ModelLights,
            ["selected"] = Editor.SelectedLight,
            ["refused"] = Lights.Refused, ["authored"] = Lights.Authored, ["sent"] = Lights.Sent,
            ["culled"] = Lights.Culled, ["uploads"] = RemasterUniforms.Uploads,
            ["litBatches"] = RemasterUniforms.LitBatches, ["supported"] = RemasterUniforms.Supported,
            ["perPixel"] = PerPixelLighting.Enabled, ["ticks"] = Lights.Ticks,
        };
    }

    static string Status()
    {
        // Only the ids that were drawn, by id.
        var byMat = new JsonObject();
        for (int i = 0; i < SurfaceMaterial.Count; i++)
            if (SurfaceMaterial.ByMaterial[i] != 0) byMat[i.ToString()] = SurfaceMaterial.ByMaterial[i];
        return Ok("remaster", new JsonObject
        {
            ["on"] = Host.Enabled,
            ["area"] = Identity.Area,
            ["settled"] = Identity.Settled,
            ["fingerprint"] = Identity.Settled ? Identity.FingerprintText : null,
            ["fromLoad"] = Identity.Settled ? Identity.FromLoad : null,
            ["gap"] = Identity.LastGap,
            ["refused"] = Surfaces.Refused,
            ["tilesApplied"] = Surfaces.TilesApplied,
            ["meshRefused"] = Surfaces.MeshRefused,
            ["subdividedRefused"] = Faces.MapRefused,
            ["packets"] = Surfaces.Packets,
            ["reflections"] = Reflections.Enabled,
            ["byMaterial"] = byMat,
            ["fromPacket"] = SurfaceMaterial.FromPacket,
            ["selected"] = Editor.SelectedModel?.ToString() ?? Editor.Selected?.ToString(),
            ["editor"] = Editor.Open,
            ["tableUploads"] = SurfaceMaterial.Uploads,
            ["emissive"] = SurfaceMaterial.AnyEmissive,
        });
    }

    static JsonObject Describe(TileKey? key)
    {
        if (key is not { } k) return new JsonObject { ["selected"] = null };
        var o = new JsonObject { ["selected"] = k.ToString(), ["material"] = Pack.TileMaterial(k) };
        if (Editor.SelectedFaces.Count > 0)
        {
            var faces = new JsonArray();
            foreach (var f in Editor.SelectedFaces)
                faces.Add(new JsonObject
                {
                    ["face"] = f.ToString(), ["mesh"] = f.Mesh,
                    ["material"] = Pack.FaceMaterial(f, false), ["meshMaterial"] = Pack.FaceMaterial(f, true),
                });
            o["faces"] = faces;
        }
        var m = Runtime.Mem;
        if (m != null && k.Area == Identity.Area)
        {
            uint rec = Identity.HalfRecord(k.X, k.Z, k.Half);
            o["model"] = m.ReadU8(rec);
            o["height"] = m.ReadU8(rec + 1);
            o["flags"] = m.ReadU8(rec + 4);
            o["drawn"] = m.ReadU8(rec) < 240;
            int mesh = m.ReadU8(rec);
            o["meshFaces"] = Faces.Mesh(m, mesh)?.Length;
            o["meshHash"] = Faces.MeshHash(m, mesh);
            // What the last frame sealed this half's triangles with, by face.
            var sealedIds = new SortedDictionary<int, SortedSet<int>>();
            foreach (var t in Faces.Last)
                if (t.Rec == rec)
                {
                    if (!sealedIds.TryGetValue(t.Face, out var set)) sealedIds[t.Face] = set = new SortedSet<int>();
                    set.Add(t.Label);
                }
            var ids = new JsonObject();
            foreach (var (f, set) in sealedIds) ids[f.ToString()] = string.Join("/", set);
            o["sealedIds"] = ids;
        }
        return o;
    }

    static JsonObject DescribeModel(ModelKey k)
    {
        // What the last frame sealed this model's triangles with.
        var ids = new SortedSet<int>();
        int tris = 0;
        foreach (var t in Faces.Last)
            if (t.Rec == 0 && t.Model == k.Model && t.Kind == k.Kind) { ids.Add(t.Label); tris++; }
        return new JsonObject
        {
            ["selected"] = k.ToString(), ["material"] = Pack.ModelMaterial(k),
            ["triangles"] = tris, ["sealedIds"] = string.Join("/", ids),
        };
    }

    static string Ok(string cmd, JsonObject body)
    {
        var o = new JsonObject { ["ok"] = true, ["cmd"] = cmd };
        foreach (var (k, v) in body) o[k] = v?.DeepClone();
        return o.ToJsonString();
    }

    static string Err(string cmd, string message)
        => new JsonObject { ["ok"] = false, ["cmd"] = cmd, ["error"] = message }.ToJsonString();
}
