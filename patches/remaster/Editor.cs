using System.Numerics;
using System.Text.Json.Nodes;
using ImGuiNET;
using RecompOne.Runtime;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Host.Window;
using Silk.NET.Input;
using HostWindow = RecompOne.Runtime.Host.HostWindow;

namespace Kf2.Remaster;

/// <summary>
/// The remaster editor, Shift+E. It writes documents only -- never a side table or a
/// uniform -- so undo, save and live reload are one operation. Open, it pauses the
/// world through <see cref="FramePacing.PauseWhen"/> as the full-screen map does;
/// the renderer keeps drawing, so an edit shows on the frozen scene.
///
/// A click on the picture picks the faces under it from the frame's own triangles
/// (<see cref="Faces.PickAt"/>), which is what the depth buffer drew there, or the model
/// drawn there, which takes a material wherever the area draws it; the docked
/// map (right-click, Shift+M) and the player's tile select a whole half. Lights are
/// placed at the eye or on the surface under a click, drawn over the picture, and
/// dragged across the screen at their depth. See "The editor", "Faces, picked from the
/// frame" and "Phase 2, the first slice" in docs/REMASTER.md.
/// </summary>
public static class Editor
{
    public static bool Open => Panel.Instance.IsOpen;

    /// <summary>The selected half, or the half of the first selected face.</summary>
    public static TileKey? Selected { get; private set; }

    /// <summary>The selected faces; empty selects the whole of <see cref="Selected"/>.</summary>
    public static readonly List<FaceRef> SelectedFaces = new();

    /// <summary>The selected model; a model and a tile are never selected together.</summary>
    public static ModelKey? SelectedModel { get; private set; }

    public static void SelectModel(ModelKey? k)
    {
        SelectedModel = k;
        Selected = null;
        SelectedFaces.Clear();
        SelectedTexture = null;
    }

    /// <summary>The art the last pick drew there, or the first selected face's: what
    /// a texture assignment names, in every area.</summary>
    public static TexKey? SelectedTexture { get; private set; }

    public static void SelectTexture(TexKey? k) => SelectedTexture = k;

    /// <summary>A texture assignment names the art under any palette.</summary>
    public static bool AnyPalette = true;

    /// <summary>The art a tile face draws, read from its mesh.</summary>
    public static TexKey? TextureOf(RecompOne.Runtime.Memory.IMemory m, FaceRef f)
    {
        var faces = Faces.Mesh(m, f.Mesh);
        if (faces == null || (uint)f.Face >= (uint)faces.Length || faces[f.Face].Verts.Length == 0) return null;
        var mf = faces[f.Face];
        return TextureKeys.Of(mf.Tpage, mf.Clut, (int)(mf.Rect & 0xFF), (int)((mf.Rect >> 8) & 0xFF),
                              (int)((mf.Rect >> 16) & 0xFF), (int)(mf.Rect >> 24), out var k) ? k : null;
    }

    /// <summary>Give the selected art a material in every area, or clear it.</summary>
    public static string? AssignTexture(string? material)
    {
        if (SelectedTexture is not { } k) return "no texture selected (pick a face or a model)";
        Pack.SetTextureMaterial(AnyPalette ? k.AnyClut : k, material);
        return null;
    }

    /// <summary>Assignments go to the mesh wherever the area uses it, not to the half.</summary>
    public static bool MeshScope;

    /// <summary>Clicks on the picture pick faces.</summary>
    public static bool Picking;

    public static void SetOpen(bool open) => Panel.Instance.IsOpen = open;

    /// <summary>The selected light, by name, in the loaded area.</summary>
    public static string? SelectedLight { get; private set; }

    public static void SelectLight(string? name) => SelectedLight = name;

    /// <summary>How far short of a picked surface a light is placed, towards the eye.</summary>
    const float PlaceBack = 192f;

    /// <summary>Where a light goes for a click at a game pixel: the nearest surface the
    /// last frame drew there, pulled back towards the eye so it is not inside it.</summary>
    public static Vector3? PlaceAt(RecompOne.Runtime.Memory.IMemory m, Vector2 px)
    {
        if (Faces.Nearest(px, out float z) < 0 || !float.IsFinite(z)) return null;
        var v = Lights.ReadView(m);
        var p = new Vector3((px.X - v.Cx) * z / v.H, (px.Y - v.Cy) * z / v.H, z);
        float len = p.Length();
        p *= MathF.Max(len - PlaceBack, len * 0.5f) / len;
        return v.ToWorld(p);
    }

    /// <summary>The eye, which is where "Add at eye" puts a light.</summary>
    public static Vector3 PlayerLightPosition(RecompOne.Runtime.Memory.IMemory m) => Lights.ReadView(m).ToWorld(Vector3.Zero);

    /// <summary>A whole half.</summary>
    public static void Select(TileKey? key)
    {
        Selected = key;
        SelectedModel = null;
        SelectedFaces.Clear();
        SelectedTexture = null;
    }

    /// <summary>Faces; with <paramref name="toggle"/>, each is added or, if already
    /// selected, removed.</summary>
    public static void SelectFaces(IReadOnlyList<FaceRef> faces, bool toggle)
    {
        if (!toggle) SelectedFaces.Clear();
        SelectedModel = null;
        foreach (var f in faces)
            if (!toggle || !SelectedFaces.Remove(f)) SelectedFaces.Add(f);
        Selected = SelectedFaces.Count > 0 ? SelectedFaces[0].Tile : toggle ? Selected : null;
        SelectedTexture = SelectedFaces.Count > 0 && RecompOne.Runtime.Runtime.Mem is { } m ? TextureOf(m, SelectedFaces[0]) : null;
    }

    /// <summary>The mesh a half draws now, or -1 when it draws none.</summary>
    public static int MeshOf(RecompOne.Runtime.Memory.IMemory m, TileKey k)
    {
        int model = m.ReadU8(Identity.HalfRecord(k.X, k.Z, k.Half));
        return model < 240 ? model : -1;
    }

    public enum Grow { Connected, Texture, Mesh }

    /// <summary>Widen the face selection within each selected half's mesh.</summary>
    public static void GrowSelection(RecompOne.Runtime.Memory.IMemory m, Grow how)
    {
        var seeds = SelectedFaces.ToList();
        foreach (var f in seeds)
        {
            IEnumerable<int> more = how switch
            {
                Grow.Connected => Faces.Connected(m, f.Mesh, f.Face, sameTexture: true),
                Grow.Texture => Faces.SameTexture(m, f.Mesh, f.Face),
                _ => Enumerable.Range(0, Faces.Mesh(m, f.Mesh)?.Length ?? 0),
            };
            foreach (int i in more)
            {
                var g = f with { Face = i };
                if (!SelectedFaces.Contains(g)) SelectedFaces.Add(g);
            }
        }
    }

    /// <summary>Whether the selection's area can be edited now, and why not.</summary>
    public static string? Blocked(TileKey k) => Blocked(k.Area);

    public static string? Blocked(int area)
        => !Identity.Settled ? "the area is settling"
         : area != Identity.Area ? "the selection is in another area"
         : Surfaces.Refused;

    /// <summary>What the selection is given at the current scope, or "(mixed)".</summary>
    public static string? SelectionMaterial(RecompOne.Runtime.Memory.IMemory m)
    {
        if (SelectedModel is { } mk) return Pack.ModelMaterial(mk);
        if (Selected is not { } k) return null;
        if (SelectedFaces.Count == 0)
            return MeshScope ? MeshOf(m, k) is int mesh and >= 0 ? Pack.MeshMaterial(k.Area, mesh) : null
                             : Pack.TileMaterial(k);
        var first = Pack.FaceMaterial(SelectedFaces[0], MeshScope);
        foreach (var f in SelectedFaces)
            if (Pack.FaceMaterial(f, MeshScope) != first) return "(mixed)";
        return first;
    }

    /// <summary>Give the selection a material at the current scope, or clear it.</summary>
    public static string? Assign(RecompOne.Runtime.Memory.IMemory m, string? material)
    {
        if (SelectedModel is { } mk)
        {
            if (Blocked(mk.Area) is { } w) return w;
            Pack.SetModelMaterial(mk, material, Identity.FingerprintText);
            return null;
        }
        if (Selected is not { } k) return "nothing selected";
        if (Blocked(k) is { } why) return why;
        string fp = Identity.FingerprintText;
        if (SelectedFaces.Count > 0)
        {
            foreach (var f in SelectedFaces)
                if (Faces.MeshHash(m, f.Mesh) == null) return $"mesh {f.Mesh} cannot be read";
            Pack.SetFaces(SelectedFaces, material, MeshScope, mesh => Faces.MeshHash(m, mesh), fp);
            return null;
        }
        if (!MeshScope) { Pack.SetTile(k, material, fp); return null; }
        int model = MeshOf(m, k);
        if (model < 0 || Faces.MeshHash(m, model) is not { } hash) return "the half draws no mesh";
        Pack.SetMeshMaterial(k.Area, model, hash, material, fp);
        return null;
    }

    public static void Install()
    {
        FramePacing.PauseWhen(() => Open && Identity.Area >= 0);

        Event.AddListener<KeyboardEvent>(e =>
        {
            if (!e.Pressed || e.Repeat || e.Key != (int)Key.E || PopupManager.AnyOpen || HotkeyGate.Typing) return;
            if (!HostWindow.IsKeyDown(Key.ShiftLeft) && !HostWindow.IsKeyDown(Key.ShiftRight)) return;
            Panel.Instance.IsOpen = !Panel.Instance.IsOpen;
        });
    }

    public static void Register()
    {
        Localization.Merge("""
        {
          "strings": {
            "kf2.remaster.editor": { "en": "Remaster editor", "pt-BR": "Editor de remasterização",
                                     "es-419": "Editor de remasterización" }
          }
        }
        """);
        PanelManager.Register(Panel.Instance);
        // Not restored from the saved view: open, it pauses the world, and a boot
        // that reopened it froze the area's fade-in on a dark frame.
        Panel.Instance.IsOpen = false;
    }

    sealed class Panel : IPanel
    {
        public static readonly Panel Instance = new();
        Panel() { }

        public string Name => "kf2remaster";
        public string TitleKey => "kf2.remaster.editor";
        public bool IsOpen { get; set; }

        string _newName = "";
        string? _pickWhy;
        bool _highlight = true;
        string? _held;
        float _heldFrom;

        public void Draw()
        {
            bool open = IsOpen;
            ImGui.SetNextWindowSize(new Vector2(440, 560), ImGuiCond.FirstUseEver);
            if (!ImGui.Begin(this.Title(), ref open))
            {
                IsOpen = open;
                ImGui.End();
                return;
            }
            IsOpen = open;

            DrawStatus();
            ImGui.Separator();
            DrawSelection();
            ImGui.Separator();
            DrawMaterials();
            ImGui.Separator();
            DrawLights();
            ImGui.Separator();
            DrawAtmosphere();
            bool hovered = ImGui.IsWindowHovered(ImGuiHoveredFlags.RootAndChildWindows);
            ImGui.End();

            var m = Runtime.Mem;
            if (m == null || Identity.Area < 0) return;
            var mouse = ImGui.GetIO().MousePos;
            if (!hovered && OutputView.Hovered && ImGui.IsMouseClicked(ImGuiMouseButton.Left)
                && GamePixel(mouse, out var px))
            {
                if (_placing)
                {
                    _placing = false;
                    if (PlaceAt(m, px) is { } at) AddLight(at);
                    else _pickWhy = "no surface under that pixel for a light";
                }
                else if (_lightGizmos && LightUnder(m, mouse) is { } name)
                {
                    SelectLight(name);
                    BeginDrag(m, name);
                }
                else if (Picking)
                {
                    bool toggle = ImGui.GetIO().KeyShift;
                    var hit = Faces.PickAt(px, out var model, out _pickWhy, out var tex);
                    if (hit != null) SelectFaces(hit, toggle);
                    else if (model != null) SelectModel(model);
                    if (hit != null || model != null) SelectTexture(tex);
                }
            }
            Drag(m, mouse);
            if (_highlight && Selected is { } k && k.Area == Identity.Area) Highlight(k);
            if (_highlight && SelectedModel is { } mk && mk.Area == Identity.Area) HighlightModel(mk);
            if (_lightGizmos) DrawGizmos(m);
        }

        bool _placing, _lightGizmos = true;
        JsonObject? _lightBefore;
        string? _dragging;
        float _dragZ;
        Vector2 _dragGrab;

        static bool LightsBlocked(out string? why)
        {
            why = !Identity.Settled ? "the area is settling" : Lights.Refused;
            return why != null;
        }

        void AddLight(Vector3 at)
        {
            if (LightsBlocked(out _)) return;
            string name = Pack.FreeLightName(Identity.Area);
            if (Pack.AddLight(Identity.Area, Identity.FingerprintText, name, at)) SelectLight(name);
        }

        void DrawLights()
        {
            ImGui.Text("Lights");
            var m = Runtime.Mem;
            if (m == null || Identity.Area < 0) { ImGui.TextDisabled("No area loaded."); return; }
            int area = Identity.Area;
            if (!PerPixelLighting.Enabled)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), "Per-pixel lighting is off; a light is drawn only with it.");
            if (!RemasterUniforms.Supported)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), "This renderer has no light term (GL core only).");
            bool blocked = LightsBlocked(out string? why);
            if (why != null && Lights.Refused != null) ImGui.TextColored(new Vector4(1f, 0.4f, 0.4f, 1f), why);

            ImGui.BeginDisabled(blocked);
            if (ImGui.Button("Add at eye")) AddLight(PlayerLightPosition(m));
            ImGui.SameLine();
            ImGui.Checkbox("Place on the picture", ref _placing);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("The next click on the game picture adds a light just short of the surface under it.");
            ImGui.SameLine();
            ImGui.Checkbox("Show", ref _lightGizmos);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Draw the area's lights over the picture. Click one to select it; drag it to move it across the screen at its depth.");
            ImGui.EndDisabled();
            if (Host.Enabled)
                ImGui.TextDisabled($"{Lights.Authored} in the area, {Lights.Sent} drawn, {Lights.Culled} culled");

            var lights = Pack.Lights(area).ToList();
            foreach (var l in lights)
                if (ImGui.Selectable($"{l.Name}  ({(l.Spot ? "spot" : "point")}{(l.Off ? ", off" : "")})", l.Name == SelectedLight))
                    SelectLight(l.Name);
            if (SelectedLight is not { } name || Pack.GetLight(area, name) is not { } sel) return;

            ImGui.PushID("light:" + name);
            ImGui.BeginDisabled(blocked);
            int type = sel.Spot ? 1 : 0;
            ImGui.SetNextItemWidth(120);
            if (ImGui.Combo("Type", ref type, "point\0spot\0"))
                Pack.SetLight(area, name, $"type = {(type == 1 ? "spot" : "point")}", o => o["type"] = type == 1 ? "spot" : "point");

            var pos = sel.Position;
            ImGui.SetNextItemWidth(260);
            Edited(area, name, "position", ImGui.DragFloat3("Position", ref pos, 8f, 0f, 0f, "%.0f"), o => Pack.SetPosition(o, pos));
            if (ImGui.IsItemHovered()) ImGui.SetTooltip("World units: a tile is 2048, a height step 128, and up is -Y.");
            var col = sel.Colour;
            ImGui.SetNextItemWidth(260);
            Edited(area, name, "colour", ImGui.ColorEdit3("Colour", ref col, ImGuiColorEditFlags.Float), o => Pack.SetColour(o, col));
            float intensity = sel.Intensity;
            ImGui.SetNextItemWidth(200);
            Edited(area, name, "intensity", ImGui.SliderFloat("Intensity", ref intensity, 0f, 4f, "%.2f"),
                   o => Pack.SetNumber(o, "intensity", intensity));
            float radius = sel.Radius;
            ImGui.SetNextItemWidth(200);
            Edited(area, name, "radius", ImGui.SliderFloat("Radius", ref radius, 256f, 16384f, "%.0f", ImGuiSliderFlags.Logarithmic),
                   o => Pack.SetNumber(o, "radius", radius));
            if (sel.Spot)
            {
                var dir = sel.Direction;
                ImGui.SetNextItemWidth(260);
                Edited(area, name, "direction", ImGui.DragFloat3("Direction", ref dir, 0.01f, -1f, 1f, "%.2f"), o => Pack.SetDirection(o, dir));
                if (ImGui.Button("Aim along the view"))
                {
                    var fwd = Lights.ReadView(m).ToWorld(new Vector3(0, 0, 1)) - Lights.ReadView(m).ToWorld(Vector3.Zero);
                    Pack.SetLight(area, name, "aim", o => Pack.SetDirection(o, fwd));
                }
                float inner = sel.ConeInner, outer = sel.ConeOuter;
                ImGui.SetNextItemWidth(200);
                Edited(area, name, "cone", ImGui.SliderFloat("Inner cone", ref inner, 0f, 89f, "%.0f deg"), o => Pack.SetCone(o, inner, outer));
                ImGui.SetNextItemWidth(200);
                Edited(area, name, "cone", ImGui.SliderFloat("Outer cone", ref outer, 1f, 89f, "%.0f deg"), o => Pack.SetCone(o, inner, outer));
            }
            float amount = sel.FlickerAmount, hz = sel.FlickerHz > 0f ? sel.FlickerHz : 7f;
            ImGui.SetNextItemWidth(200);
            Edited(area, name, "flicker", ImGui.SliderFloat("Flicker", ref amount, 0f, 1f, "%.2f"), o => Pack.SetFlicker(o, amount, hz));
            if (ImGui.IsItemHovered()) ImGui.SetTooltip("How far the intensity dips. It steps with the world, so it holds while the editor pauses it.");
            if (amount > 0f)
            {
                ImGui.SetNextItemWidth(200);
                Edited(area, name, "flicker", ImGui.SliderFloat("Flicker rate", ref hz, 0.5f, 10f, "%.1f Hz"), o => Pack.SetFlicker(o, amount, hz));
            }
            bool on = !sel.Off;
            if (ImGui.Checkbox("On", ref on))
                Pack.SetLight(area, name, on ? "on" : "off", o => { if (on) o.Remove("enabled"); else o["enabled"] = false; });
            ImGui.SameLine();
            bool shadows = sel.Shadows;
            ImGui.BeginDisabled(!Lights.ShadowsOn);
            if (ImGui.Checkbox("Shadows", ref shadows))
                Pack.SetLight(area, name, shadows ? "shadows" : "no shadows", o => { if (shadows) o.Remove("shadows"); else o["shadows"] = false; });
            ImGui.EndDisabled();
            if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
                ImGui.SetTooltip(Lights.ShadowsOn
                    ? $"The area's walls and floors{(RetainedScene.ShadowModels ? ", its creatures and its objects" : "")} cast shadows from this light. The nearest {RemasterUniforms.MaxShadows} shadowed lights in view get one."
                    : "Shadows are off (KF2_REMASTER_SHADOWS=0).");
            ImGui.SameLine();
            if (ImGui.Button("Move to eye")) Pack.SetLight(area, name, "move to eye", o => Pack.SetPosition(o, PlayerLightPosition(m)));
            ImGui.SameLine();
            if (ImGui.Button("Delete")) { Pack.RemoveLight(area, name); SelectLight(null); }
            ImGui.EndDisabled();
            ImGui.PopID();
        }

        /// <summary>Live while a control is held, one undo entry on release; a change made
        /// in one step (typed in, or a colour picked) is its own entry.</summary>
        void Edited(int area, string name, string label, bool changed, Action<JsonObject> apply)
        {
            if (ImGui.IsItemActivated()) _lightBefore = Pack.LightSnapshot(area, name);
            if (changed)
            {
                if (ImGui.IsItemActive() && _lightBefore != null) Pack.PreviewLight(area, name, apply);
                else Pack.SetLight(area, name, label, apply);
            }
            if (ImGui.IsItemDeactivatedAfterEdit() && _lightBefore != null) Pack.CommitLight(area, name, label, _lightBefore);
            if (ImGui.IsItemDeactivated()) _lightBefore = null;
        }

        // ---- atmosphere: the area's own light records -------------------------

        int _record = -1;
        bool _followRecord = true;
        JsonObject? _recBefore;
        bool _recHeld;
        int[] _usage = [];
        int _usageSettle = -1;

        static string fp0() => Identity.FingerprintText;

        static int[] Rgb255(Vector3 c) => [(int)MathF.Round(c.X * 255f), (int)MathF.Round(c.Y * 255f), (int)MathF.Round(c.Z * 255f)];

        void DrawAtmosphere()
        {
            ImGui.Text("Atmosphere");
            var m = Runtime.Mem;
            if (m == null || Identity.Area < 0 || !Identity.Settled) { ImGui.TextDisabled("No settled area."); return; }
            int area = Identity.Area;
            if (Atmosphere.Refused is { } why) ImGui.TextColored(new Vector4(1f, 0.4f, 0.4f, 1f), why);
            if (_usageSettle != Identity.Settles) { _usage = Atmosphere.Usage(m); _usageSettle = Identity.Settles; }

            // The area's darkness: a scale on the game's own light, not an edit of it.
            float pct = (Pack.GetRecord(area, Pack.AllRecords)?.Darkness ?? 0f) * 100f;
            ImGui.SetNextItemWidth(220);
            RecEdited(area, Pack.AllRecords, "", "darkness", ImGui.SliderFloat("Darkness", ref pct, 0f, 100f, "%.0f%%"),
                x => Pack.SetDarkness(x, pct / 100f));
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("How much of the game's own light the area loses: 0% is the game's, 100% leaves only authored lights and glows. " +
                                 "Scales every tile record's back colour and light colours after the record's own edits; the HUD keeps its light. Ctrl+click to type.");

            // The area's fog: the colour the depth cue fades into, its curve, and the sky.
            var all = Pack.GetRecord(area, Pack.AllRecords);
            bool fogColOn = all?.FogColour != null;
            if (ImGui.Checkbox("##fogcol", ref fogColOn))
                Pack.SetRecord(area, Pack.AllRecords, "", fp0(), fogColOn ? "fog colour" : "fog colour = game",
                    x => Pack.SetFogColour(x, fogColOn ? [0, 0, 0] : null));
            ImGui.SameLine();
            ImGui.BeginDisabled(!fogColOn);
            var fc = all?.FogColour is { } c0 ? new Vector3(c0[0], c0[1], c0[2]) / 255f : Vector3.Zero;
            ImGui.SetNextItemWidth(220);
            RecEdited(area, Pack.AllRecords, "", "fog colour", ImGui.ColorEdit3("Fog colour", ref fc),
                x => Pack.SetFogColour(x, Rgb255(fc)));
            ImGui.EndDisabled();
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("What the distance fades into; the game's is black. Needs per-pixel lighting and Fast geometry.");
            float power = all?.FogPower ?? 1f, most = (all?.FogMax ?? 1f) * 100f;
            ImGui.SetNextItemWidth(220);
            RecEdited(area, Pack.AllRecords, "", "fog curve", ImGui.SliderFloat("Fog curve", ref power, 0.25f, 4f, "%.2f", ImGuiSliderFlags.Logarithmic),
                x => Pack.SetFogCurve(x, power, all?.FogMax));
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Bends the game's own fog: below 1 thickens it close by, above 1 holds it off until further out. 1 is the game's.");
            ImGui.SetNextItemWidth(220);
            RecEdited(area, Pack.AllRecords, "", "fog at most", ImGui.SliderFloat("Fog at most", ref most, 0f, 100f, "%.0f%%"),
                x => Pack.SetFogCurve(x, all?.FogPower, most / 100f));
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("The most the fog ever takes: below 100% the far distance never quite disappears. 100% is the game's.");
            bool skyOn = all?.Sky != null;
            if (ImGui.Checkbox("##sky", ref skyOn))
                Pack.SetRecord(area, Pack.AllRecords, "", fp0(), skyOn ? "sky" : "sky = fog",
                    x => Pack.SetSky(x, skyOn ? (all?.FogColour ?? [0, 0, 0]) : null));
            ImGui.SameLine();
            ImGui.BeginDisabled(!skyOn);
            var sk = all?.Sky is { } s0 ? new Vector3(s0[0], s0[1], s0[2]) / 255f : fc;
            ImGui.SetNextItemWidth(220);
            RecEdited(area, Pack.AllRecords, "", "sky", ImGui.ColorEdit3("Sky", ref sk), x => Pack.SetSky(x, Rgb255(sk)));
            ImGui.EndDisabled();
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("The colour past the draw distance, where nothing is drawn. Unticked it is the fog colour, or the game's black without one.");

            int under = Atmosphere.UnderPlayer(m);
            ImGui.Checkbox("Follow the player", ref _followRecord);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Edit the light record of the tile you stand on. Every tile names one of 80 records: its light, back colour and fog.");
            if (_followRecord && under >= 0) _record = under;
            if (_record < 0) _record = under >= 0 ? under : 0;

            string Label(int r) => $"record {r}  ({(r < _usage.Length ? _usage[r] : 0)} halves)" +
                                   (r == under ? ", under you" : "") + (Pack.GetRecord(area, r) != null ? ", edited" : "");
            ImGui.SetNextItemWidth(260);
            if (ImGui.BeginCombo("##record", Label(_record)))
            {
                for (int r = 0; r < Atmosphere.Records; r++)
                {
                    bool show = (r < _usage.Length && _usage[r] > 0) || r == under || Pack.GetRecord(area, r) != null;
                    if (show && ImGui.Selectable(Label(r), r == _record)) { _record = r; _followRecord = false; }
                }
                ImGui.EndCombo();
            }

            int rec = _record;
            string hash = Atmosphere.SourceHash(m, rec), fp = Identity.FingerprintText;
            var o = Pack.GetRecord(area, rec);
            var g = Atmosphere.Game(m, rec);
            var e = Atmosphere.Effective(m, rec, o, dark: false);
            if (Atmosphere.Darkness > 0f && rec < Atmosphere.Darkened)
                ImGui.TextDisabled($"The area's darkness scales this record's light by {100f * (1f - Atmosphere.Darkness):0}% after these edits.");
            if (o is { Hash: { } h } && h != hash)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), "The game's record has changed since this was authored; it is not applied.");
            if (!Host.Enabled) ImGui.TextDisabled("The remaster is off; edits apply when it is on.");

            ImGui.PushID("record:" + rec);
            // Back colour, the light nothing faces away from.
            bool backOn = o?.Back != null;
            if (ImGui.Checkbox("##back", ref backOn))
                Pack.SetRecord(area, rec, hash, fp, backOn ? "back" : "back = game", x => Pack.SetBack(x, backOn ? g.Back : null));
            ImGui.SameLine();
            ImGui.BeginDisabled(!backOn);
            var back = new Vector3(e.Back[0], e.Back[1], e.Back[2]) / 255f;
            ImGui.SetNextItemWidth(220);
            RecEdited(area, rec, hash, "back", ImGui.ColorEdit3("Back colour", ref back),
                x => Pack.SetBack(x, [(int)MathF.Round(back.X * 255f), (int)MathF.Round(back.Y * 255f), (int)MathF.Round(back.Z * 255f)]));
            ImGui.EndDisabled();

            for (int j = 0; j < 3; j++)
            {
                int jj = j;
                ImGui.PushID(j);
                bool dirOn = o?.Direction[j] != null, colOn = o?.Colour[j] != null;
                if (ImGui.Checkbox("##dir", ref dirOn))
                    Pack.SetRecord(area, rec, hash, fp, $"light {j} direction{(dirOn ? "" : " = game")}",
                        x => Pack.SetRecordLight(x, jj, "direction", dirOn ? g.Direction[jj] : null));
                ImGui.SameLine();
                ImGui.BeginDisabled(!dirOn);
                var dir = e.Direction[j];
                ImGui.SetNextItemWidth(220);
                RecEdited(area, rec, hash, $"light {j} direction",
                    ImGui.DragFloat3($"Light {j + 1} faces", ref dir, 0.01f, -1f, 1f, "%.2f"),
                    x => Pack.SetRecordLight(x, jj, "direction", dir));
                ImGui.EndDisabled();
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("A face turned this way takes the light's whole colour; its length scales it.");
                if (ImGui.Checkbox("##col", ref colOn))
                    Pack.SetRecord(area, rec, hash, fp, $"light {j} colour{(colOn ? "" : " = game")}",
                        x => Pack.SetRecordLight(x, jj, "colour", colOn ? g.Colour[jj] : null));
                ImGui.SameLine();
                ImGui.BeginDisabled(!colOn);
                var col = e.Colour[j];
                ImGui.SetNextItemWidth(220);
                RecEdited(area, rec, hash, $"light {j} colour",
                    ImGui.ColorEdit3($"Light {j + 1} colour", ref col, ImGuiColorEditFlags.Float | ImGuiColorEditFlags.HDR),
                    x => Pack.SetRecordLight(x, jj, "colour", col));
                ImGui.EndDisabled();
                ImGui.PopID();
            }

            bool fogOn = o?.Fog != null;
            if (ImGui.Checkbox("##fog", ref fogOn))
                Pack.SetRecord(area, rec, hash, fp, fogOn ? "fog" : "fog = game", x => Pack.SetFog(x, fogOn ? g.Fog : null));
            ImGui.SameLine();
            ImGui.BeginDisabled(!fogOn);
            int word = e.Fog;
            bool linear = (word & 0x8000) != 0, none = !linear && (word & 0x7FFF) >= 32000;
            int start = none ? 16000 : (word & 0x7FFF) >> 1;
            ImGui.BeginDisabled(none);
            ImGui.SetNextItemWidth(220);
            RecEdited(area, rec, hash, "fog", ImGui.SliderInt("Fog starts at", ref start, 0, 15999),
                x => Pack.SetFog(x, (linear ? 0x8000 : 0) | (start << 1)));
            ImGui.EndDisabled();
            if (ImGui.IsItemHovered()) ImGui.SetTooltip("View units: a tile is 2048. The game's own is " + Atmosphere.DescribeFog(g.Fog) + ".");
            ImGui.Indent(ImGui.GetFrameHeight() + ImGui.GetStyle().ItemInnerSpacing.X);
            if (ImGui.Checkbox("Linear", ref linear))
                Pack.SetRecord(area, rec, hash, fp, linear ? "fog linear" : "fog knee",
                    x => Pack.SetFog(x, (linear ? 0x8000 : 0) | ((none ? 16000 : start) << 1)));
            if (ImGui.IsItemHovered()) ImGui.SetTooltip("The game's two fog curves: a knee that turns black quickly (most areas), or a straight ramp.");
            ImGui.SameLine();
            if (ImGui.Checkbox("No fog", ref none))
                Pack.SetRecord(area, rec, hash, fp, none ? "no fog" : "fog",
                    x => Pack.SetFog(x, none ? 32000 : start << 1));
            ImGui.Unindent(ImGui.GetFrameHeight() + ImGui.GetStyle().ItemInnerSpacing.X);
            ImGui.EndDisabled();

            ImGui.BeginDisabled(o == null);
            if (ImGui.Button("Reset record")) Pack.RemoveRecord(area, rec);
            ImGui.EndDisabled();
            ImGui.SameLine();
            ImGui.TextDisabled($"{Atmosphere.Applied} override(s) written" + (Atmosphere.Stale > 0 ? $", {Atmosphere.Stale} refused" : ""));
            ImGui.PopID();
        }

        /// <summary>As <see cref="Edited"/>, for a record: live while held, one undo entry on release.</summary>
        void RecEdited(int area, int rec, string hash, string label, bool changed, Action<JsonObject> apply)
        {
            string fp = Identity.FingerprintText;
            if (ImGui.IsItemActivated()) { _recBefore = Pack.RecordSnapshot(area, rec); _recHeld = true; }
            if (changed)
            {
                if (ImGui.IsItemActive() && _recHeld) Pack.PreviewRecord(area, rec, hash, fp, apply);
                else Pack.SetRecord(area, rec, hash, fp, label, apply);
            }
            if (ImGui.IsItemDeactivatedAfterEdit() && _recHeld) Pack.CommitRecord(area, rec, fp, label, _recBefore);
            if (ImGui.IsItemDeactivated()) { _recBefore = null; _recHeld = false; }
        }

        /// <summary>The light whose marker is under a window position.</summary>
        static string? LightUnder(RecompOne.Runtime.Memory.IMemory m, Vector2 mouse)
        {
            if (!OutputView.Valid || OutputView.GameW <= 0) return null;
            var v = Lights.ReadView(m);
            string? best = null;
            float bestD = 10f * 10f;
            foreach (var l in Pack.Lights(Identity.Area))
            {
                if (!v.Project(l.Position, out var s, out _)) continue;
                float d = Vector2.DistanceSquared(WindowPixel(s), mouse);
                if (d < bestD) { bestD = d; best = l.Name; }
            }
            return best;
        }

        void BeginDrag(RecompOne.Runtime.Memory.IMemory m, string name)
        {
            if (LightsBlocked(out _) || Pack.GetLight(Identity.Area, name) is not { } l) return;
            var v = Lights.ReadView(m);
            if (!v.Project(l.Position, out var s, out float z)) return;
            _dragging = name;
            _dragZ = z;
            _dragGrab = s;
            _lightBefore = Pack.LightSnapshot(Identity.Area, name);
        }

        /// <summary>Across the screen at the light's depth; with Shift, straight up and
        /// down in the world, a height step at a time.</summary>
        void Drag(RecompOne.Runtime.Memory.IMemory m, Vector2 mouse)
        {
            if (_dragging is not { } name) return;
            int area = Identity.Area;
            if (!ImGui.IsMouseDown(ImGuiMouseButton.Left))
            {
                if (_lightBefore != null && Pack.LightSnapshot(area, name)?.ToJsonString() != _lightBefore.ToJsonString())
                    Pack.CommitLight(area, name, "move", _lightBefore);
                _dragging = null;
                _lightBefore = null;
                return;
            }
            if (!GamePixel(mouse, out var px) || _lightBefore == null) return;
            var v = Lights.ReadView(m);
            var origin = Vec3(_lightBefore["position"]);
            Vector3 to;
            if (ImGui.GetIO().KeyShift)
            {
                float dy = (px.Y - _dragGrab.Y) * _dragZ / v.H;
                to = origin with { Y = origin.Y + MathF.Round(dy / 128f) * 128f };
            }
            else to = origin + (v.Unproject(px, _dragZ) - v.Unproject(_dragGrab, _dragZ));
            Pack.PreviewLight(area, name, o => Pack.SetPosition(o, to));
        }

        static Vector3 Vec3(JsonNode? n)
            => n is JsonArray a && a.Count >= 3
                ? new Vector3((float)a[0]!.GetValue<double>(), (float)a[1]!.GetValue<double>(), (float)a[2]!.GetValue<double>())
                : Vector3.Zero;

        /// <summary>Each light as a dot in its colour and its reach as a ring at its depth.</summary>
        void DrawGizmos(RecompOne.Runtime.Memory.IMemory m)
        {
            if (!OutputView.Valid || OutputView.GameW <= 0) return;
            var v = Lights.ReadView(m);
            int margin = Display.WideMargin(OutputView.GameW);
            float scale = OutputView.Size.X / (OutputView.GameW + 2 * margin);
            var dl = ImGui.GetForegroundDrawList();
            dl.PushClipRect(OutputView.Min, OutputView.Max, true);
            foreach (var l in Pack.Lights(Identity.Area))
            {
                if (!v.Project(l.Position, out var s, out float z)) continue;
                var w = WindowPixel(s);
                bool sel = l.Name == SelectedLight;
                uint ring = ImGui.GetColorU32(sel ? new Vector4(1f, 0.85f, 0.2f, 0.9f) : new Vector4(1f, 1f, 1f, 0.35f));
                uint fill = ImGui.GetColorU32(new Vector4(l.Colour.X, l.Colour.Y, l.Colour.Z, l.Off ? 0.3f : 1f));
                float r = l.Radius * v.H / z * scale;
                if (r < 4000f) dl.AddCircle(w, r, ring, 48, sel ? 1.5f : 1f);
                dl.AddCircleFilled(w, 6f, fill);
                dl.AddCircle(w, 7f, ring, 16, 2f);
                if (l.Spot && v.Project(l.Position + Vector3.Normalize(l.Direction) * MathF.Min(l.Radius, 1024f), out var tip, out _))
                    dl.AddLine(w, WindowPixel(tip), ring, 2f);
                if (sel) dl.AddText(w + new Vector2(10f, -8f), ring, l.Name);
            }
            dl.PopClipRect();
        }

        void DrawStatus()
        {
            bool on = Host.Enabled;
            if (ImGui.Checkbox("Remaster on", ref on)) Host.SetEnabled(on);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Apply the working pack. Off, nothing it holds reaches the picture.");

            if (!Reflections.AnySource)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f),
                    "No reflections are on, so only a material's glow is drawn.");

            if (Identity.Area < 0) ImGui.TextDisabled("No area loaded.");
            else
                ImGui.TextDisabled($"Area {Identity.Area}, " +
                                   (Identity.Settled ? $"fingerprint {Identity.FingerprintText}" : "settling..."));
            if (Surfaces.Refused != null) ImGui.TextColored(new Vector4(1f, 0.4f, 0.4f, 1f), Surfaces.Refused);
            if (Surfaces.MeshRefused > 0)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f),
                    $"{Surfaces.MeshRefused} face list(s) not applied: their mesh no longer hashes as it did.");

            ImGui.TextDisabled(Pack.Root);
            if (ImGui.Button("Save")) Pack.Save();
            ImGui.SameLine();
            if (ImGui.Button("Reload")) Pack.Load();
            ImGui.SameLine();
            ImGui.BeginDisabled(Pack.UndoLabel == null);
            if (ImGui.Button("Undo")) Pack.Undo();
            if (Pack.UndoLabel != null && ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
                ImGui.SetTooltip(Pack.UndoLabel);
            ImGui.EndDisabled();
            ImGui.SameLine();
            ImGui.BeginDisabled(Pack.RedoLabel == null);
            if (ImGui.Button("Redo")) Pack.Redo();
            ImGui.EndDisabled();
            ImGui.SameLine();
            ImGui.TextDisabled(Pack.Dirty ? "unsaved" : Pack.SavedAt is { } t ? $"saved {t:HH:mm:ss}" : "");
            if (Pack.LastError != null) ImGui.TextColored(new Vector4(1f, 0.4f, 0.4f, 1f), Pack.LastError);
        }

        void DrawSelection()
        {
            var m = Runtime.Mem;
            ImGui.BeginDisabled(m == null || Identity.Area < 0);
            if (ImGui.Button("Player's tile") && m != null) Select(Identity.PlayerTile(m));
            ImGui.SameLine();
            ImGui.Checkbox("Pick on the picture", ref Picking);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("A click on the game picture selects the faces under it, or the model; Shift+click adds or removes faces. " +
                                 "Faces lying on top of each other are picked together. " +
                                 "Right-click a tile in the docked map (Shift+M) selects its whole half.");
            ImGui.SameLine();
            ImGui.Checkbox("Highlight", ref _highlight);
            ImGui.EndDisabled();
            if (Picking && _pickWhy != null) ImGui.TextDisabled($"Nothing picked: {_pickWhy}.");

            if (SelectedModel is { } mk && m != null)
            {
                ImGui.Text($"{mk} (every draw of it in the area)");
                MaterialCombo(m, Blocked(mk.Area));
                TextureCombo();
                return;
            }
            if (Selected is not { } k || m == null) { ImGui.TextDisabled("Nothing selected."); return; }
            if (SelectedFaces.Count == 0)
            {
                ImGui.Text($"{k} (whole half)");
                if (k.Area == Identity.Area)
                {
                    uint rec = Identity.HalfRecord(k.X, k.Z, k.Half);
                    ImGui.TextDisabled($"mesh {m.ReadU8(rec)} height {m.ReadU8(rec + 1):X2} light {m.ReadU8(rec + 4) & 0x3F:X2}");
                }
            }
            else
            {
                ImGui.Text(SelectedFaces.Count == 1 ? "1 face" : $"{SelectedFaces.Count} faces");
                foreach (var g in SelectedFaces.GroupBy(f => (f.Tile, f.Mesh)))
                    ImGui.TextDisabled($"{g.Key.Tile} mesh {g.Key.Mesh}: faces {string.Join(", ", g.Select(f => f.Face).Order())}");
                if (ImGui.Button("Connected")) GrowSelection(m, Grow.Connected);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Add the faces joined to these by an edge, with the same texture.");
                ImGui.SameLine();
                if (ImGui.Button("Same texture")) GrowSelection(m, Grow.Texture);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Add every face of the same mesh with the same texture.");
                ImGui.SameLine();
                if (ImGui.Button("Whole mesh")) GrowSelection(m, Grow.Mesh);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Add every face of the mesh, one by one.");
                ImGui.SameLine();
                if (ImGui.Button("Whole half")) Select(k);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Select the half itself: everything its mesh draws, whatever the mesh.");
            }

            ImGui.Checkbox("Every tile with this mesh", ref MeshScope);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Assign to the mesh wherever this area uses it. A half's own assignment still wins on that half.");

            MaterialCombo(m, Blocked(k));
            TextureCombo();
        }

        /// <summary>The picked art's material, in every area.</summary>
        static void TextureCombo()
        {
            if (SelectedTexture is not { } tk) return;
            ImGui.Separator();
            var key = AnyPalette ? tk.AnyClut : tk;
            ImGui.Text("Texture");
            ImGui.SameLine();
            ImGui.TextDisabled(key.ToString());
            ImGui.Checkbox("Any palette", ref AnyPalette);
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("On: this art wherever it is drawn, whatever its colours. Off: only in this palette.");
            string current = Pack.TextureMaterial(key) ?? "(none)";
            ImGui.SetNextItemWidth(220);
            if (ImGui.BeginCombo("Everywhere", current))
            {
                if (ImGui.Selectable("(none)", current == "(none)")) AssignTexture(null);
                foreach (var mat in Pack.Materials())
                    if (ImGui.Selectable(mat.Name, mat.Name == current)) AssignTexture(mat.Name);
                ImGui.EndCombo();
            }
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Every face drawing this art, in every area. A tile, face, mesh or model assignment wins over it.");
        }

        static void MaterialCombo(RecompOne.Runtime.Memory.IMemory m, string? blocked)
        {
            string current = SelectionMaterial(m) ?? "(none)";
            ImGui.BeginDisabled(blocked != null);
            ImGui.SetNextItemWidth(220);
            if (ImGui.BeginCombo("Material", current))
            {
                if (ImGui.Selectable("(none)", current == "(none)")) Assign(m, null);
                foreach (var mat in Pack.Materials())
                    if (ImGui.Selectable(mat.Name, mat.Name == current)) Assign(m, mat.Name);
                ImGui.EndCombo();
            }
            ImGui.EndDisabled();
            if (blocked != null) ImGui.TextDisabled(blocked);
            if (current is not ("(none)" or "(mixed)") && !Pack.HasMaterial(current))
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f), $"'{current}' is not in the library.");
        }

        void DrawMaterials()
        {
            ImGui.Text("Materials");
            if (Surfaces.Unallocated > 0)
                ImGui.TextColored(new Vector4(1f, 0.75f, 0.3f, 1f),
                    $"Only {SurfaceMaterial.Count - SurfaceMaterial.FirstAuthored} materials have an id; " +
                    $"{Surfaces.Unallocated} past them apply nowhere.");

            string? remove = null;
            foreach (var mat in Pack.Materials().ToList())
            {
                ImGui.PushID(mat.Name);
                byte id = Surfaces.IdOf(mat.Name);
                ImGui.Text(mat.Name);
                ImGui.SameLine();
                ImGui.TextDisabled(id != 0 ? $"id {id}" : Host.Enabled ? "no id" : "");
                ImGui.SameLine(ImGui.GetContentRegionAvail().X - 20);
                if (ImGui.SmallButton("x")) remove = mat.Name;
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Remove from the library");
                Slider(mat.Name, "reflectivity", "Reflectivity", mat.Reflectivity);
                Slider(mat.Name, "f0", "F0", mat.F0);
                Slider(mat.Name, "roughness", "Roughness", mat.Roughness);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Blurs the reflection, more the further what it shows is, and widens the highlight.");
                Slider(mat.Name, "metalness", "Metalness", mat.Metalness);
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("0 stone, 1 metal: a mirror tinted by its own colour, reflecting at least this much, " +
                                     "as strongly looking straight at it, and its own colour darker. Needs reflections.");
                Slider(mat.Name, "specular", "Specular", mat.Specular);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("The highlight authored lights (and glows) leave on it. Its size is the roughness.");
                Slider(mat.Name, "occlusion", "Occlusion", mat.Occlusion);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("How much ambient occlusion darkens it. A glowing material defaults to 0.");
                Colour(mat.Name, "emissive", "Emissive", mat.Emissive);
                Slider(mat.Name, "emissiveStrength", "Glow", mat.EmissiveStrength, 4f);
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("Light the surface gives off, fogged like the game's own light. " +
                                     "Needs per-pixel lighting.");
                bool additive = mat.GlowAdditive;
                if (ImGui.Checkbox("Light source", ref additive))
                    Pack.SetText(mat.Name, "glowMode", additive ? null : "lit");
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("On: the glow is added over the texture, so its dark texels light too.\n" +
                                     "Off: the glow lights the texture, which shows it brighter (1 is the texture at full).");
                ImGui.SameLine();
                ImGui.BeginDisabled(!additive);
                bool fog = !mat.GlowUnfogged;
                if (ImGui.Checkbox("Fogged", ref fog)) Pack.SetFlag(mat.Name, "glowFog", fog ? null : false);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("Off: the glow stays bright in the dark distance, like a lamp.");
                ImGui.EndDisabled();
                Slider(mat.Name, "light", "Light", mat.Light, Pack.MaxLight);
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("The light it gives off, in the emissive colour, with or without a glow of its own. " +
                                     "It lights what is around it and not the material itself.");
                Slider(mat.Name, "glowRadius", "Light reach", mat.GlowRadius, Pack.MaxGlowRadius, "%.0f");
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("How far that light reaches, in world units (a tile is 2048). 0 gives no light.");
                Slider(mat.Name, "pulseAmount", "Pulse", mat.PulseAmount);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("How far the glow and its light dip, on the world clock.");
                Slider(mat.Name, "pulseHz", "Pulse rate", mat.PulseHz, 10f, "%.2f Hz");
                bool flicker = mat.PulseFlicker;
                if (ImGui.Checkbox("Flicker", ref flicker)) Pack.SetText(mat.Name, "pulseStyle", flicker ? "flicker" : null);
                if (ImGui.IsItemHovered()) ImGui.SetTooltip("On: an irregular flicker, like a flame. Off: a steady breathing.");
                ImGui.PopID();
            }
            if (remove != null) Pack.RemoveMaterial(remove);

            ImGui.SetNextItemWidth(200);
            bool enter = ImGui.InputTextWithHint("##new", "new material name", ref _newName, 48,
                                                 ImGuiInputTextFlags.EnterReturnsTrue);
            ImGui.SameLine();
            if ((ImGui.Button("Add") || enter) && _newName.Trim().Length > 0)
            {
                Pack.AddMaterial(_newName.Trim());
                _newName = "";
            }
        }

        /// <summary>Live while held, one undo entry on release.</summary>
        void Slider(string name, string field, string label, float value, float max = 1f, string format = "%.3f")
        {
            ImGui.SetNextItemWidth(200);
            float v = value;
            if (ImGui.SliderFloat(label, ref v, 0f, max, format)) Pack.Preview(name, field, v);
            if (ImGui.IsItemActivated()) { _held = name + "." + field; _heldFrom = value; }
            if (ImGui.IsItemDeactivatedAfterEdit() && _held == name + "." + field)
            {
                Pack.SetField(name, field, Pack.GetField(name, field), _heldFrom);
                _held = null;
            }
        }

        Vector3 _heldColour;

        /// <summary>A colour field, live while held, one undo entry on release.</summary>
        void Colour(string name, string field, string label, Vector3 value)
        {
            ImGui.SetNextItemWidth(200);
            var v = value;
            bool changed = ImGui.ColorEdit3(label, ref v, ImGuiColorEditFlags.Float);
            if (ImGui.IsItemActivated()) { _held = name + "." + field; _heldColour = value; }
            if (changed)
            {
                if (ImGui.IsItemActive() && _held == name + "." + field) Pack.PreviewColour(name, field, v);
                else Pack.SetColourField(name, field, v);
            }
            if (ImGui.IsItemDeactivatedAfterEdit() && _held == name + "." + field)
            {
                Pack.SetColourField(name, field, Pack.GetColour(name, field), _heldColour);
                _held = null;
            }
        }

        /// <summary>A window position to a game pixel, margin and all (the MenuMouse
        /// conversion).</summary>
        static bool GamePixel(Vector2 pos, out Vector2 game)
        {
            game = default;
            if (!OutputView.Valid || OutputView.GameW <= 0 || OutputView.GameH <= 0) return false;
            var min = OutputView.Min;
            var size = OutputView.Size;
            if (pos.X < min.X || pos.Y < min.Y || pos.X > OutputView.Max.X || pos.Y > OutputView.Max.Y) return false;
            int margin = Display.WideMargin(OutputView.GameW);
            float picW = OutputView.GameW + 2 * margin;
            game = new Vector2((pos.X - min.X) / size.X * picW - margin, (pos.Y - min.Y) / size.Y * OutputView.GameH);
            return true;
        }

        static Vector2 WindowPixel(Vector2 game)
        {
            int margin = Display.WideMargin(OutputView.GameW);
            float picW = OutputView.GameW + 2 * margin;
            var min = OutputView.Min;
            var size = OutputView.Size;
            return new Vector2(min.X + (game.X + margin) / picW * size.X, min.Y + game.Y / OutputView.GameH * size.Y);
        }

        /// <summary>The selected model's triangles from the last frame, tinted over the picture.</summary>
        static void HighlightModel(ModelKey k)
        {
            if (!OutputView.Valid || OutputView.GameW <= 0) return;
            var dl = ImGui.GetForegroundDrawList();
            dl.PushClipRect(OutputView.Min, OutputView.Max, true);
            uint col = ImGui.GetColorU32(new Vector4(0.3f, 0.85f, 1f, 0.35f));
            foreach (var t in Faces.Last)
            {
                if (t.Rec != 0 || t.Model != k.Model || t.Kind != k.Kind) continue;
                dl.AddTriangleFilled(WindowPixel(new(t.X0, t.Y0)), WindowPixel(new(t.X1, t.Y1)), WindowPixel(new(t.X2, t.Y2)), col);
            }
            dl.PopClipRect();
        }

        /// <summary>The selection's triangles from the last frame, tinted over the picture.</summary>
        static void Highlight(TileKey k)
        {
            if (!OutputView.Valid || OutputView.GameW <= 0) return;
            uint whole = SelectedFaces.Count == 0 ? Identity.HalfRecord(k.X, k.Z, k.Half) : 0u;
            var faces = new HashSet<(uint, int)>();
            foreach (var f in SelectedFaces) faces.Add((Identity.HalfRecord(f.Tile.X, f.Tile.Z, f.Tile.Half), f.Face));
            var dl = ImGui.GetForegroundDrawList();
            dl.PushClipRect(OutputView.Min, OutputView.Max, true);
            uint col = ImGui.GetColorU32(new Vector4(1f, 0.85f, 0.2f, 0.35f));
            foreach (var t in Faces.Last)
            {
                if (t.Rec == 0 || (whole != 0 ? t.Rec != whole : !faces.Contains((t.Rec, t.Face)))) continue;
                dl.AddTriangleFilled(WindowPixel(new(t.X0, t.Y0)), WindowPixel(new(t.X1, t.Y1)), WindowPixel(new(t.X2, t.Y2)), col);
            }
            dl.PopClipRect();
        }
    }
}
