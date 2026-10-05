namespace RecompOne.Runtime;

/// <summary>
/// 0088. A static map pixel lit and fogged from the light record of the half it lies
/// on blended with its neighbours', so the light runs smoothly across a tile edge
/// instead of stepping there. Per pixel, in PrimFs: the weights are bilinear between
/// the centres of the four tiles around the pixel's world position (its own at its
/// centre, half and half on an edge, a quarter each at a corner), on the half's own
/// level, and a missing half weighs nothing, so every tile meeting at a point blends
/// the same set.
///
/// <para>The fog blends the records' **results**: each record's cue weight at the
/// pixel's own depth, clamped where the colour it leaves goes black (4096), averaged.
/// That is the average of the pictures the tiles would draw, and a record with no fog
/// (curve 0) weighs in as no fog. Only curve-5 (<see cref="LinearDepthCue"/>) and
/// curve-0 records blend; a pixel whose records use another curve is drawn as before.
/// The light blends the records' **inputs**, as 0085's <c>recordLit</c> does: the
/// colour matrix and back colour, rounded to integers, under the own record's light
/// matrix (the corner's dots, <c>vNbDots</c>), then <c>NormalColorCol</c>'s integers.
/// Where nothing around the pixel differs it is drawn exactly as before.</para>
///
/// <para>The port sets <see cref="Mode"/> and fills <see cref="Halves"/>; 0 is off,
/// and off the programs draw what they drew before. A corner opts in by 0085's
/// <see cref="RetainedScene.Vertex.Light"/> and its half flag.</para>
/// </summary>
public static class NeighbourBlend
{
    public const int Fog = 1, Light = 2;

    /// <summary><see cref="Fog"/> | <see cref="Light"/>; 0 off.</summary>
    public static int Mode;

    /// <summary>A tile's side in world units; the map's tile (x, z) is centred at
    /// ((x + 0.5) * Tile, (z + 0.5) * Tile), as <see cref="RetainedScene.HalfFlag"/>
    /// numbers it.</summary>
    public static float Tile = 2048f;

    /// <summary>Each half's record plus one, 0 for no half: index (z * 80 + x) * 2 +
    /// upper, laid out as <see cref="RetainedScene.HalvesW"/> x
    /// <see cref="RetainedScene.HalvesH"/>.</summary>
    public static readonly byte[] Halves = new byte[RetainedScene.HalvesW * RetainedScene.HalvesH];

    /// <summary>Bumped by <see cref="SetHalves"/> when a half changed; the backend
    /// re-uploads.</summary>
    public static int Generation { get; private set; }

    /// <summary>The probe's: the half table's uploads.</summary>
    public static long Uploads;

    public static void SetHalves(ReadOnlySpan<byte> halves)
    {
        if (halves[..Halves.Length].SequenceEqual(Halves)) return;
        halves[..Halves.Length].CopyTo(Halves);
        Generation++;
    }

    // ---- the CPU reference, the same arithmetic as Glsl ---------------------------

    static int Rec(ReadOnlySpan<int> records, int r, int i) => records[r * RetainedScene.RecordInts + i];

    static int Half(ReadOnlySpan<byte> halves, int tx, int tz, int upper)
    {
        int side = RetainedScene.HalvesW / 2;
        if (tx < 0 || tz < 0 || tx >= side || tz >= RetainedScene.HalvesH) return -1;
        return halves[tz * RetainedScene.HalvesW + tx * 2 + upper] - 1;
    }

    /// <summary>The records weighing in at world (x, z) on half <paramref name="hid"/>
    /// (the half flag's value, one-based) and their weights: own, along X, along Z,
    /// the diagonal; -1 and 0 where there is no half.</summary>
    public static void Around(ReadOnlySpan<byte> halves, int own, int hid, float x, float z,
                              Span<int> rec, Span<float> k)
    {
        int i = hid - 1, tile = i >> 1, upper = i & 1, side = RetainedScene.HalvesW / 2;
        int tx = tile % side, tz = tile / side;
        float ox = x - (tx + 0.5f) * Tile, oz = z - (tz + 0.5f) * Tile;
        float ax = Math.Min(Math.Abs(ox), 0.5f * Tile) / Tile, az = Math.Min(Math.Abs(oz), 0.5f * Tile) / Tile;
        int sx = ox < 0f ? -1 : 1, sz = oz < 0f ? -1 : 1;
        rec[0] = own;
        rec[1] = Half(halves, tx + sx, tz, upper);
        rec[2] = Half(halves, tx, tz + sz, upper);
        rec[3] = Half(halves, tx + sx, tz + sz, upper);
        k[0] = (1f - ax) * (1f - az);
        k[1] = rec[1] >= 0 ? ax * (1f - az) : 0f;
        k[2] = rec[2] >= 0 ? (1f - ax) * az : 0f;
        k[3] = rec[3] >= 0 ? ax * az : 0f;
    }

    /// <summary>A record's fog weight at view depth z, 0..4096; -1 for a curve this
    /// does not blend.</summary>
    public static float RecordFog(ReadOnlySpan<int> records, int r, float z)
    {
        int curve = Rec(records, r, RetainedScene.RecCurve);
        if (curve == 0) return 0f;
        if (curve != LinearDepthCue.Curve) return -1f;
        return Math.Min(LinearDepthCue.Weight(z, Rec(records, r, RetainedScene.RecDqa), Rec(records, r, RetainedScene.RecDqb)), 4096f);
    }

    /// <summary>The blended fog weight, or false where no record weighing in has a
    /// fog word of its own's or one uses a curve this does not blend.</summary>
    public static bool BlendFog(ReadOnlySpan<int> records, ReadOnlySpan<int> rec, ReadOnlySpan<float> k, float z, out float w)
    {
        w = 0f;
        int word = Rec(records, rec[0], RetainedScene.RecWord);
        bool mixed = false;
        for (int i = 1; i < 4; i++) mixed |= k[i] > 0f && Rec(records, rec[i], RetainedScene.RecWord) != word;
        if (!mixed) return false;
        float t = 0f, s = 0f;
        for (int i = 0; i < 4; i++)
        {
            if (k[i] <= 0f) continue;
            float f = RecordFog(records, rec[i], z);
            if (f < 0f) return false;
            t += k[i]; s += k[i] * f;
        }
        w = s / t;
        return true;
    }

    static bool SameLight(ReadOnlySpan<int> records, int a, int b)
    {
        for (int i = RetainedScene.RecLcm; i < RetainedScene.RecWord; i++)
            if (Rec(records, a, i) != Rec(records, b, i)) return false;
        return true;
    }

    /// <summary>The weighted mean of four values, half away from zero.</summary>
    static int Mix(ReadOnlySpan<float> k, float t, int v0, int v1, int v2, int v3)
    {
        float m = (k[0] * v0 + k[1] * v1 + k[2] * v2 + k[3] * v3) / t;
        return (int)(MathF.Sign(m) * MathF.Floor(MathF.Abs(m) + 0.5f));
    }

    /// <summary>The lit colour from the corner's dots under the blended colour matrix
    /// and back colour, or false where every record weighing in lights alike.</summary>
    public static bool BlendLight(ReadOnlySpan<int> records, ReadOnlySpan<int> rec, ReadOnlySpan<float> k,
                                  int ax, int ay, int az, uint rgbc, out int r, out int g, out int b)
    {
        r = g = b = 0;
        bool mixed = false;
        for (int i = 1; i < 4; i++) mixed |= k[i] > 0f && !SameLight(records, rec[i], rec[0]);
        if (!mixed) return false;
        float t = k[0] + k[1] + k[2] + k[3];
        Span<int> c3 = stackalloc int[3];
        Span<int> a = [ax, ay, az];
        int r0 = Math.Max(rec[0], 0), r1 = Math.Max(rec[1], 0), r2 = Math.Max(rec[2], 0), r3 = Math.Max(rec[3], 0);
        for (int c = 0; c < 3; c++)
        {
            int at = RetainedScene.RecBk + c;
            int v = Mix(k, t, Rec(records, r0, at), Rec(records, r1, at), Rec(records, r2, at), Rec(records, r3, at)) << 12;
            for (int j = 0; j < 3; j++)
            {
                int i = RetainedScene.RecLcm + 3 * c + j;
                v += Mix(k, t, Rec(records, r0, i), Rec(records, r1, i), Rec(records, r2, i), Rec(records, r3, i)) * a[j];
            }
            int ir = Math.Clamp(v >> 12, 0, 0x7FFF);
            int mac = (((int)((rgbc >> (8 * c)) & 255u) * ir) << 4) >> 12;
            c3[c] = Math.Clamp(mac >> 4, 0, 255);
        }
        (r, g, b) = (c3[0], c3[1], c3[2]);
        return true;
    }

    /// <summary>PrimFs's half: the same functions, after <see cref="LinearDepthCue.Glsl"/>.
    /// uRecords is 0085's record texture, uNbHalves <see cref="Halves"/>, uNbTile
    /// <see cref="Tile"/>.</summary>
    public const string Glsl = """
        uniform isampler2D uRecords;
        uniform usampler2D uNbHalves;
        uniform float uNbTile;
        int nbRecInt(int r, int i) { return texelFetch(uRecords, ivec2(i >> 2, r), 0)[i & 3]; }
        int nbHalf(int tx, int tz, int upper) {
            ivec2 size = textureSize(uNbHalves, 0);
            if (tx < 0 || tz < 0 || tx >= size.x / 2 || tz >= size.y) return -1;
            return int(texelFetch(uNbHalves, ivec2(tx * 2 + upper, tz), 0).r) - 1;
        }
        void nbAround(int own, int hid, vec2 xz, out ivec4 rec, out vec4 k) {
            int i = hid - 1, tile = i >> 1, upper = i & 1, side = textureSize(uNbHalves, 0).x / 2;
            int tx = tile % side, tz = tile / side;
            vec2 o = xz - (vec2(tx, tz) + 0.5) * uNbTile;
            vec2 a = min(abs(o), vec2(0.5 * uNbTile)) / uNbTile;
            ivec2 s = ivec2(o.x < 0.0 ? -1 : 1, o.y < 0.0 ? -1 : 1);
            rec = ivec4(own, nbHalf(tx + s.x, tz, upper), nbHalf(tx, tz + s.y, upper), nbHalf(tx + s.x, tz + s.y, upper));
            k = vec4((1.0 - a.x) * (1.0 - a.y), rec.y >= 0 ? a.x * (1.0 - a.y) : 0.0,
                     rec.z >= 0 ? (1.0 - a.x) * a.y : 0.0, rec.w >= 0 ? a.x * a.y : 0.0);
        }
        float nbRecordFog(int r, float z) {
            int curve = nbRecInt(r, 51);
            if (curve == 0) return 0.0;
            if (curve != 5) return -1.0;
            return min(linearDepthCue(z, vec2(float(nbRecInt(r, 49)), float(nbRecInt(r, 50)))), 4096.0);
        }
        bool nbFog(ivec4 rec, vec4 k, float z, out float w) {
            w = 0.0;
            int word = nbRecInt(rec.x, 48);
            bool mixed = false;
            for (int i = 1; i < 4; i++) mixed = mixed || (k[i] > 0.0 && nbRecInt(rec[i], 48) != word);
            if (!mixed) return false;
            float t = 0.0, s = 0.0;
            for (int i = 0; i < 4; i++) {
                if (k[i] <= 0.0) continue;
                float f = nbRecordFog(rec[i], z);
                if (f < 0.0) return false;
                t += k[i]; s += k[i] * f;
            }
            w = s / t;
            return true;
        }
        bool nbSameLight(int a, int b) {
            for (int i = 9; i < 12; i++)
                if (texelFetch(uRecords, ivec2(i, a), 0) != texelFetch(uRecords, ivec2(i, b), 0)) return false;
            return true;
        }
        int nbMix(vec4 k, float t, ivec4 v) {
            float m = (k.x * float(v.x) + k.y * float(v.y) + k.z * float(v.z) + k.w * float(v.w)) / t;
            return int(sign(m) * floor(abs(m) + 0.5));
        }
        bool nbLight(ivec4 rec, vec4 k, ivec3 a, uint rgbc, out vec3 color) {
            color = vec3(0.0);
            bool mixed = false;
            for (int i = 1; i < 4; i++) mixed = mixed || (k[i] > 0.0 && !nbSameLight(rec[i], rec.x));
            if (!mixed) return false;
            float t = k.x + k.y + k.z + k.w;
            ivec4 r = max(rec, ivec4(0));
            for (int c = 0; c < 3; c++) {
                int v = nbMix(k, t, ivec4(nbRecInt(r.x, 45 + c), nbRecInt(r.y, 45 + c), nbRecInt(r.z, 45 + c), nbRecInt(r.w, 45 + c))) << 12;
                for (int j = 0; j < 3; j++) {
                    int i = 36 + 3 * c + j;
                    v += nbMix(k, t, ivec4(nbRecInt(r.x, i), nbRecInt(r.y, i), nbRecInt(r.z, i), nbRecInt(r.w, i))) * a[j];
                }
                int ir = clamp(v >> 12, 0, 0x7FFF);
                int mac = ((int((rgbc >> uint(8 * c)) & 255u) * ir) << 4) >> 12;
                color[c] = float(clamp(mac >> 4, 0, 255));
            }
            return true;
        }
        """;
}
