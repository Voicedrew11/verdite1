using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// The planes a planar reflection mirrors in, found in the retained map rather than
/// in the picture: every level face that is water (translucent, textured from a
/// fluid slot's VRAM rect) or carries an authored material that reflects. Grouped by
/// height when the map is built; each frame the heights are ranked by how much of
/// the picture their faces cover from the frame's own camera, and the best
/// <see cref="RetainedScene.MaxPlanes"/> the camera stands above are drawn.
///
/// The old finder binned the water the backend had drawn, so its plane was always
/// the previous frame's. This one is the frame's own.
/// </summary>
static class RetainedPlanes
{
    struct Face
    {
        public float X0, Z0, X1, Z1, X2, Z2, X3, Z3, Y;
        public int Corners;
        public bool Semi;
        public byte Mat;
        public uint TPage, Rect;
        public int Key;
    }

    static readonly List<Face> _faces = new();

    /// <summary>The faces grouped by height, chunk and what makes them reflect
    /// (water, or an authored id), each group one box: what a frame ranks.</summary>
    struct Group
    {
        public float X0, Z0, X1, Z1, Y, SumY;
        public int Key, Faces;
        public bool Water;
        public byte Mat;
    }

    static readonly List<Group> _groups = new();
    static readonly Dictionary<(int, int, int), int> _groupOf = new();
    static readonly Dictionary<int, double> _area = new();
    static readonly Dictionary<int, (double Sum, double W)> _height = new();
    static readonly float[] _chosen = new float[RetainedScene.MaxPlanes];
    static readonly double[] _chosenArea = new double[RetainedScene.MaxPlanes];
    static int _chosenN, _waterFaces, _authoredFaces;

    /// <summary>A face level to within this many units is a candidate.</summary>
    const float Level = 2f;

    /// <summary>The least picture a plane must cover to be drawn, in game pixels.</summary>
    const double MinArea = 64.0;

    public static void Clear() { _faces.Clear(); _groups.Clear(); _groupOf.Clear(); }

    public static void Note(Span<float> px, Span<float> py, Span<float> pz, int corners, bool semi, byte mat,
                            uint tpage, uint rect)
    {
        if (!semi && mat == 0) return;
        float lo = py[0], hi = py[0];
        for (int k = 1; k < corners; k++) { lo = Math.Min(lo, py[k]); hi = Math.Max(hi, py[k]); }
        if (hi - lo > Level) return;
        float y = (lo + hi) * 0.5f;
        _faces.Add(new Face
        {
            X0 = px[0], Z0 = pz[0], X1 = px[1], Z1 = pz[1], X2 = px[2], Z2 = pz[2],
            X3 = corners == 4 ? px[3] : px[2], Z3 = corners == 4 ? pz[3] : pz[2],
            Y = y, Corners = corners, Semi = semi, Mat = mat, TPage = tpage, Rect = rect,
            Key = (int)MathF.Round(y / 16f),
        });
    }

    public static void Finish()
    {
        foreach (var f in _faces)
        {
            bool water = Water(f);
            if (!water && f.Mat == 0) continue;
            int chunk = (int)(Math.Min(f.X0, f.X2) / 16384f) * 16 + (int)(Math.Min(f.Z0, f.Z2) / 16384f);
            var id = (f.Key, chunk, water ? -1 : f.Mat);
            float x0 = Math.Min(Math.Min(f.X0, f.X1), Math.Min(f.X2, f.X3)), x1 = Math.Max(Math.Max(f.X0, f.X1), Math.Max(f.X2, f.X3));
            float z0 = Math.Min(Math.Min(f.Z0, f.Z1), Math.Min(f.Z2, f.Z3)), z1 = Math.Max(Math.Max(f.Z0, f.Z1), Math.Max(f.Z2, f.Z3));
            if (!_groupOf.TryGetValue(id, out int g))
            {
                g = _groups.Count;
                _groupOf[id] = g;
                _groups.Add(new Group { X0 = x0, Z0 = z0, X1 = x1, Z1 = z1, Key = f.Key, Water = water, Mat = f.Mat });
            }
            var gr = _groups[g];
            gr.X0 = Math.Min(gr.X0, x0); gr.Z0 = Math.Min(gr.Z0, z0);
            gr.X1 = Math.Max(gr.X1, x1); gr.Z1 = Math.Max(gr.Z1, z1);
            gr.SumY += f.Y;
            gr.Faces++;
            gr.Y = gr.SumY / gr.Faces;
            _groups[g] = gr;
        }
        _faces.Clear();
    }

    /// <summary>Whether a translucent face samples one of the fluid slots' rects,
    /// which is how the reflection pass knows water from any other blended face.</summary>
    static bool Water(in Face f)
    {
        if (!f.Semi) return false;
        int div = ((f.TPage >> 7) & 3) switch { 0 => 4, 1 => 2, _ => 1 };
        int bx = (int)(f.TPage & 0xF) * 64, by = (int)((f.TPage >> 4) & 1) * 256;
        int x0 = bx + (int)(f.Rect & 0xFF) / div, x1 = bx + (int)((f.Rect >> 16) & 0xFF) / div;
        int y0 = by + (int)((f.Rect >> 8) & 0xFF), y1 = by + (int)(f.Rect >> 24);
        for (int i = 0; i < SurfaceMaterial.RectN; i++)
        {
            ref var r = ref SurfaceMaterial.Rects[i];
            if (x1 >= r.X && x0 < r.X + r.W && y1 >= r.Y && y0 < r.Y + r.H) return true;
        }
        return false;
    }

    /// <summary>The frame's planes, from its camera.</summary>
    public static void Choose(IMemory mem)
    {
        _chosenN = 0;
        _area.Clear();
        _height.Clear();
        _waterFaces = _authoredFaces = 0;
        var f = RetainedScene.Find(RetainedScene.Serial);
        if (f == null || !RetainedScene.Planar) return;
        var v = f.View;

        foreach (var g in _groups)
        {
            bool authored = !g.Water && SurfaceMaterial.Reflectivity[g.Mat] > 0f;
            if (!g.Water && !authored) continue;
            if (g.Water) _waterFaces += g.Faces; else _authoredFaces += g.Faces;
            // Seen from above only: Y is down, and the mirror is of what is above it.
            if (v.CamY >= g.Y - 16f) continue;
            double a = Area(v, g);
            if (a <= 0.0) continue;
            _area[g.Key] = _area.GetValueOrDefault(g.Key) + a;
            var h = _height.GetValueOrDefault(g.Key);
            _height[g.Key] = (h.Sum + a * g.Y, h.W + a);
        }

        foreach (var (key, a) in _area.OrderByDescending(kv => kv.Value))
        {
            if (a < MinArea || _chosenN == RetainedScene.MaxPlanes) break;
            var h = _height[key];
            _chosen[_chosenN] = (float)(h.Sum / h.W);
            _chosenArea[_chosenN] = a;
            _chosenN++;
        }
        RetainedScene.SetPlanes(_chosen.AsSpan(0, _chosenN));
    }

    /// <summary>A face's area on screen, near enough to rank by: its corners
    /// projected with the depth held off the eye, and pulled into the picture.</summary>
    static double Area(in RetainedScene.View v, in Group g)
    {
        Span<double> sx = stackalloc double[4], sy = stackalloc double[4];
        int behind = 0;
        for (int k = 0; k < 4; k++)
        {
            double dx = ((k & 1) != 0 ? g.X1 : g.X0) - v.CamX, dy = g.Y - v.CamY, dz = ((k & 2) != 0 ? g.Z1 : g.Z0) - v.CamZ;
            double vx = v.R00 * dx + v.R01 * dy + v.R02 * dz + v.Tx;
            double vy = v.R10 * dx + v.R11 * dy + v.R12 * dz + v.Ty;
            double vz = v.R20 * dx + v.R21 * dy + v.R22 * dz + v.Tz;
            if (vz < 64.0) { behind++; vz = 64.0; }
            sx[k] = Math.Clamp(v.Cx + v.H * vx / vz, -120.0, 440.0);
            sy[k] = Math.Clamp(v.Cy + v.H * vy / vz, 0.0, 240.0);
        }
        if (behind == 4) return 0.0;
        // Corners 0,1,3,2 go round the box.
        double a = (sx[0] * sy[1] - sx[1] * sy[0]) + (sx[1] * sy[3] - sx[3] * sy[1])
                 + (sx[3] * sy[2] - sx[2] * sy[3]) + (sx[2] * sy[0] - sx[0] * sy[2]);
        return Math.Abs(a) * 0.5;
    }

    public static string Describe()
    {
        var s = new System.Text.StringBuilder();
        s.Append($"{_groups.Count} level group(s), {_waterFaces} water and {_authoredFaces} authored face(s) this frame; planes");
        if (_chosenN == 0) s.Append(" none");
        for (int i = 0; i < _chosenN; i++) s.Append($" Y {_chosen[i]:F0} over {_chosenArea[i]:F0} px;");
        return s.ToString();
    }
}
