using RecompOne.Runtime.Hle;

namespace RecompOne.Runtime.Interp;

internal sealed class TransformInterp
{
    private const float Scale = 1f / 4096f;
    private const float MagnitudeThreshold = 10f;
    private const float ScreenTolerance = 8f;
    private const float Reach = 1024f;
    private const int Doubts = 2;
    private const float MaxTurn = 0.5f;
    private const float OrdPenalty = 64f;
    private const float KeyPenalty = 2048f;
    private const float UvPenalty = 48f;
    
    private struct Pose
    {
        public bool Valid;
        public float M0, M1, M2, M3, M4, M5, M6, M7, M8;
        public float TX, TY, TZ;
        public float CX, CY, CZ;
        public float H, OFX, OFY;
    }
    
    private readonly record struct Candidate(int Current, int Previous, float Score);
    
    private readonly List<Candidate> _candidates = [];
    private Pose[] _poses = [];
    private bool[] _taken = [];
    private int[] _bestCur = [];
    private int[] _bestPrev = [];
    private float[] _scoreCur = [];
    private float[] _scorePrev = [];
    
    public int Matched { get; private set; }
    
    public int Groups { get; private set; }
    
    
    
    
    
    
    private readonly Dictionary<uint, int> _order = new();
    
    private readonly Dictionary<long, float> _depths = new();
    
    
    private void Fill(FrameGraph current)
    {
        _depths.Clear();
        
        foreach (var tri in current.Tris)
        {
            if (tri.Transform <= 0) continue;
            
            Learn(tri.Transform, in tri.A);
            Learn(tri.Transform, in tri.B);
            Learn(tri.Transform, in tri.C);
        }
        
        for (var i = 0; i < current.Tris.Count; i++)
        {
            var tri = current.Tris[i];
            if (tri.Transform <= 0) continue;
            if (tri.A.Depth > 0f && tri.B.Depth > 0f && tri.C.Depth > 0f) continue;
            
            Borrow(tri.Transform, ref tri.A);
            Borrow(tri.Transform, ref tri.B);
            Borrow(tri.Transform, ref tri.C);
            
            current.Tris[i] = tri;
        }
    }
    
    private void Learn(int group, in HleVertex vertex)
    {
        if (vertex.Depth <= 0f) return;
        
        _depths.TryAdd(Corner(group, in vertex), vertex.Depth);
    }
    
    private void Borrow(int group, ref HleVertex vertex)
    {
        if (vertex.Depth > 0f) return;
        if (!_depths.TryGetValue(Corner(group, in vertex), out var depth)) return;
        
        vertex.Depth = depth;
    }
    
    private static long Corner(int group, in HleVertex vertex)
    {
        var x = (long)BitConverter.SingleToInt32Bits(vertex.X);
        var y = (uint)BitConverter.SingleToInt32Bits(vertex.Y);
        
        return ((x << 32) | y) * 31L + group;
    }
    
    private void Survey(FrameGraph current)
    {
        Fill(current);
        
        for (var i = 0; i < current.Transforms.Count; i++)
        {
            var group = current.Transforms[i];
            group.Warpable = true;
            current.Transforms[i] = group;
        }
        
        foreach (var tri in current.Tris)
        {
            var index = tri.Transform - 1;
            if (index < 0 || index >= current.Transforms.Count) continue;
            
            if (tri.A.Depth > 0f && tri.B.Depth > 0f && tri.C.Depth > 0f) continue;
            
            var group = current.Transforms[index];
            if (!group.Warpable) continue;
            
            group.Warpable = false;
            current.Transforms[index] = group;
        }
    }
    
    public void Match(FrameGraph current, FrameGraph previous)
    {
        Survey(current);
        
        Matched = 0;
        Groups = current.Transforms.Count;
        
        Order(current);
        Order(previous);
        
        if (_taken.Length < previous.Transforms.Count) _taken = new bool[previous.Transforms.Count];
        Array.Clear(_taken, 0, previous.Transforms.Count);
        
        Assign(current, previous);
    }
    
    private void Order(FrameGraph graph)
    {
        _order.Clear();
        
        for (var i = 0; i < graph.Transforms.Count; i++)
        {
            var group = graph.Transforms[i];
            _order.TryGetValue(group.Key, out var seen);
            group.Ord = seen;
            _order[group.Key] = seen + 1;
            graph.Transforms[i] = group;
        }
    }
    
    private void Assign(FrameGraph current, FrameGraph previous)
    {
        _candidates.Clear();
        
        var here = current.Transforms.Count;
        var there = previous.Transforms.Count;
        
        for (var i = 0; i < here; i++)
        for (var j = 0; j < there; j++)
        {
            if (!Score(current.Transforms[i], previous.Transforms[j], out var score)) continue;
            _candidates.Add(new Candidate(i, j, score));
        }
        
        if (_bestCur.Length < here) { _bestCur = new int[here]; _scoreCur = new float[here]; }
        if (_bestPrev.Length < there) { _bestPrev = new int[there]; _scorePrev = new float[there]; }
        
        for (var i = 0; i < here; i++) { _bestCur[i] = -1; _scoreCur[i] = float.MaxValue; }
        for (var j = 0; j < there; j++) { _bestPrev[j] = -1; _scorePrev[j] = float.MaxValue; }
        
        foreach (var candidate in _candidates)
        {
            if (candidate.Score < _scoreCur[candidate.Current])
            {
                _scoreCur[candidate.Current] = candidate.Score;
                _bestCur[candidate.Current] = candidate.Previous;
            }
            
            if (candidate.Score < _scorePrev[candidate.Previous])
            {
                _scorePrev[candidate.Previous] = candidate.Score;
                _bestPrev[candidate.Previous] = candidate.Current;
            }
        }
        
        for (var i = 0; i < here; i++)
        {
            var j = _bestCur[i];
            if (j < 0 || _bestPrev[j] != i) continue;
            
            Accept(current, previous, i, j);
        }

    }
    
    private void Accept(FrameGraph current, FrameGraph previous, int index, int partner)
    {
        var group = current.Transforms[index];
        var from = previous.Transforms[partner];
        
        group.Match = partner;
        group.Vx = group.TX - from.TX;
        group.Vy = group.TY - from.TY;
        group.Vz = group.TZ - from.TZ;
        
        Project(ref group);
        
        if (group.Screened && from.Screened)
        {
            group.Vsx = group.Sx - from.Sx;
            group.Vsy = group.Sy - from.Sy;
        }
        
        var steady = Continues(in group, in from);
        
        group.Held = steady ? 0 : from.Held + 1;
        group.Lerp = steady || group.Held < Doubts;
        
        current.Transforms[index] = group;
        _taken[partner] = true;
        Matched++;
    }
    
    private static bool Score(in TransformRecord current, in TransformRecord previous, out float score)
    {
        score = float.MaxValue;
        
        if ((current.Pages & previous.Pages) == 0u) return false;
        
        var few = Math.Min(current.Tris, previous.Tris);
        var many = Math.Max(current.Tris, previous.Tris);
        if (many > few * 2) return false;
        
        if (Determinant(in current) * Determinant(in previous) < 0f) return false;
        if (current.H != previous.H) return false;
        
        var dx = current.TX - (previous.TX + previous.Vx);
        var dy = current.TY - (previous.TY + previous.Vy);
        var dz = current.TZ - (previous.TZ + previous.Vz);
        
        var distance = MathF.Sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > Reach) return false;
        
        var orientation =
            Row(current.R0, current.R1, current.R2, previous.R0, previous.R1, previous.R2) +
            Row(current.R3, current.R4, current.R5, previous.R3, previous.R4, previous.R5) +
            Row(current.R6, current.R7, current.R8, previous.R6, previous.R7, previous.R8);
        
        if (orientation > MaxTurn) return false;
        
        score = distance + orientation * 4096f + MathF.Abs(current.Ord - previous.Ord) * OrdPenalty;
        if (current.Key != previous.Key) score += KeyPenalty;
        score += Uv(in current, in previous) * UvPenalty;
        
        return true;
    }
    
    private static float Uv(in TransformRecord current, in TransformRecord previous)
    {
        if (current.U0 > current.U1 || previous.U0 > previous.U1) return 0f;
        
        return MathF.Abs(current.U0 - previous.U0) + MathF.Abs(current.U1 - previous.U1) +
               MathF.Abs(current.V0 - previous.V0) + MathF.Abs(current.V1 - previous.V1);
    }
    
    private static void Project(ref TransformRecord group)
    {
        group.Screened = false;
        if (group.H <= 0 || group.TZ < group.H / 2) return;
        
        group.Sx = group.OFX / 65536f + (float)group.TX * group.H / group.TZ;
        group.Sy = group.OFY / 65536f + (float)group.TY * group.H / group.TZ;
        group.Screened = true;
    }
    
    private static float Screen(in TransformRecord group)
    {
        if (group.Screened) return MathF.Sqrt(group.Vsx * group.Vsx + group.Vsy * group.Vsy);
        
        var speed = MathF.Sqrt(group.Vx * group.Vx + group.Vy * group.Vy + group.Vz * group.Vz);
        var depth = MathF.Max(MathF.Abs(group.TZ), group.H);
        
        return depth <= 0f ? 0f : speed * group.H / depth;
    }
    
    private static bool Continues(in TransformRecord current, in TransformRecord previous)
    {
        const float epsilon = 1e-3f;
        
        if (!current.Screened || !previous.Screened) return true;
        
        var speed = MathF.Sqrt(current.Vsx * current.Vsx + current.Vsy * current.Vsy);
        if (speed < ScreenTolerance) return true;
        
        var before = MathF.Sqrt(previous.Vsx * previous.Vsx + previous.Vsy * previous.Vsy);
        if (before < epsilon) return true;
        
        var dot = (current.Vsx * previous.Vsx + current.Vsy * previous.Vsy) / (speed * before);
        
        return speed / MathF.Max(dot, epsilon) / before < MagnitudeThreshold;
    }
    
    public void Build(FrameGraph current, FrameGraph previous, float weight)
    {
        if (_poses.Length < current.Transforms.Count) _poses = new Pose[current.Transforms.Count];
        
        Span<float> blended = stackalloc float[9];
        Span<float> inverse = stackalloc float[9];
        
        for (var i = 0; i < current.Transforms.Count; i++)
        {
            _poses[i].Valid = false;
            
            var group = current.Transforms[i];
            if (group.Match < 0 || !group.Warpable || !group.Lerp) continue;
            
            var from = previous.Transforms[group.Match];
            
            if (!Blend(in from, in group, weight, blended)) continue;
            if (!Invert(in group, inverse)) continue;
            
            ref var pose = ref _poses[i];
            
            pose.M0 = blended[0] * inverse[0] + blended[1] * inverse[3] + blended[2] * inverse[6];
            pose.M1 = blended[0] * inverse[1] + blended[1] * inverse[4] + blended[2] * inverse[7];
            pose.M2 = blended[0] * inverse[2] + blended[1] * inverse[5] + blended[2] * inverse[8];
            pose.M3 = blended[3] * inverse[0] + blended[4] * inverse[3] + blended[5] * inverse[6];
            pose.M4 = blended[3] * inverse[1] + blended[4] * inverse[4] + blended[5] * inverse[7];
            pose.M5 = blended[3] * inverse[2] + blended[4] * inverse[5] + blended[5] * inverse[8];
            pose.M6 = blended[6] * inverse[0] + blended[7] * inverse[3] + blended[8] * inverse[6];
            pose.M7 = blended[6] * inverse[1] + blended[7] * inverse[4] + blended[8] * inverse[7];
            pose.M8 = blended[6] * inverse[2] + blended[7] * inverse[5] + blended[8] * inverse[8];
            
            pose.CX = group.TX;
            pose.CY = group.TY;
            pose.CZ = group.TZ;
            
            pose.TX = from.TX + (group.TX - from.TX) * weight;
            pose.TY = from.TY + (group.TY - from.TY) * weight;
            pose.TZ = from.TZ + (group.TZ - from.TZ) * weight;
            
            pose.H = group.H;
            pose.OFX = group.OFX / 65536f;
            pose.OFY = group.OFY / 65536f;
            pose.Valid = pose.H > 0f;
        }
    }
    
    public bool Ready(int group)
    {
        var index = group - 1;
        return index >= 0 && index < _poses.Length && _poses[index].Valid;
    }
    
    public bool Warp(int group, in HleVertex vertex, out HleVertex result)
    {
        result = vertex;
        
        var index = group - 1;
        if (index < 0 || index >= _poses.Length || !_poses[index].Valid) return false;
        
        return Place(ref _poses[index], in vertex, ref result);
    }
    
    public bool Warp(int group, in HleVertex a, in HleVertex b, in HleVertex c,
        out HleVertex wa, out HleVertex wb, out HleVertex wc)
    {
        wa = a;
        wb = b;
        wc = c;
        
        var index = group - 1;
        if (index < 0 || index >= _poses.Length || !_poses[index].Valid) return false;
        
        ref var shared = ref _poses[index];
        
        return Place(ref shared, in a, ref wa) && Place(ref shared, in b, ref wb) &&
               Place(ref shared, in c, ref wc);
    }
    
    private static bool Place(ref Pose pose, in HleVertex vertex, ref HleVertex result)
    {
        if (vertex.Depth <= 0f) return false;
        
        var depth = vertex.Depth;
        var vx = (vertex.X - pose.OFX) * depth / pose.H;
        var vy = (vertex.Y - pose.OFY) * depth / pose.H;
        
        vx -= pose.CX;
        vy -= pose.CY;
        var vz = depth - pose.CZ;
        
        var nx = pose.M0 * vx + pose.M1 * vy + pose.M2 * vz + pose.TX;
        var ny = pose.M3 * vx + pose.M4 * vy + pose.M5 * vz + pose.TY;
        var nz = pose.M6 * vx + pose.M7 * vy + pose.M8 * vz + pose.TZ;
        
        var near = pose.H / 2f;
        if (nz < near) nz = near;
        
        var x = pose.OFX + nx * pose.H / nz;
        var y = pose.OFY + ny * pose.H / nz;
        
        if (x < -Reach || x > Reach - 1f || y < -Reach || y > Reach - 1f)
        {
            x = Math.Clamp(x, -Reach, Reach - 1f);
            y = Math.Clamp(y, -Reach, Reach - 1f);
        }
        
        result.X = x;
        result.Y = y;
        result.Z = nz;
        
        return true;
    }
    
    private static bool Blend(in TransformRecord from, in TransformRecord to, float weight, Span<float> result)
    {
        Span<float> qa = stackalloc float[9];
        Span<float> ka = stackalloc float[6];
        Span<float> qb = stackalloc float[9];
        Span<float> kb = stackalloc float[6];
        
        if (!Decompose(in from, qa, ka) || !Decompose(in to, qb, kb)) return false;
        
        Span<float> rotation = stackalloc float[9];
        if (!Slerp(qa, qb, weight, rotation)) return false;

        Span<float> k = stackalloc float[6];
        for (var i = 0; i < 6; i++) k[i] = ka[i] + (kb[i] - ka[i]) * weight;
        
        result[0] = rotation[0] * k[0];
        result[1] = rotation[0] * k[1] + rotation[1] * k[3];
        result[2] = rotation[0] * k[2] + rotation[1] * k[4] + rotation[2] * k[5];
        result[3] = rotation[3] * k[0];
        result[4] = rotation[3] * k[1] + rotation[4] * k[3];
        result[5] = rotation[3] * k[2] + rotation[4] * k[4] + rotation[5] * k[5];
        result[6] = rotation[6] * k[0];
        result[7] = rotation[6] * k[1] + rotation[7] * k[3];
        result[8] = rotation[6] * k[2] + rotation[7] * k[4] + rotation[8] * k[5];
        
        return true;
    }
    
    private static bool Decompose(in TransformRecord m, Span<float> q, Span<float> k)
    {
        var c0x = m.R0 * Scale;
        var c0y = m.R3 * Scale;
        var c0z = m.R6 * Scale;
        var c1x = m.R1 * Scale;
        var c1y = m.R4 * Scale;
        var c1z = m.R7 * Scale;
        var c2x = m.R2 * Scale;
        var c2y = m.R5 * Scale;
        var c2z = m.R8 * Scale;
        
        var k00 = MathF.Sqrt(c0x * c0x + c0y * c0y + c0z * c0z);
        if (k00 < 1e-4f) return false;
        
        var q0x = c0x / k00;
        var q0y = c0y / k00;
        var q0z = c0z / k00;
        
        var k01 = q0x * c1x + q0y * c1y + q0z * c1z;
        var e1x = c1x - k01 * q0x;
        var e1y = c1y - k01 * q0y;
        var e1z = c1z - k01 * q0z;
        
        var k11 = MathF.Sqrt(e1x * e1x + e1y * e1y + e1z * e1z);
        if (k11 < 1e-4f) return false;
        
        var q1x = e1x / k11;
        var q1y = e1y / k11;
        var q1z = e1z / k11;
        
        var k02 = q0x * c2x + q0y * c2y + q0z * c2z;
        var k12 = q1x * c2x + q1y * c2y + q1z * c2z;
        var e2x = c2x - k02 * q0x - k12 * q1x;
        var e2y = c2y - k02 * q0y - k12 * q1y;
        var e2z = c2z - k02 * q0z - k12 * q1z;
        
        var k22 = MathF.Sqrt(e2x * e2x + e2y * e2y + e2z * e2z);
        if (k22 < 1e-4f) return false;
        
        var q2x = e2x / k22;
        var q2y = e2y / k22;
        var q2z = e2z / k22;
        
        var determinant = q0x * (q1y * q2z - q1z * q2y) - q1x * (q0y * q2z - q0z * q2y) +q2x * (q0y * q1z - q0z * q1y);
        
        if (determinant < 0f)
        {
            q2x = -q2x;
            q2y = -q2y;
            q2z = -q2z;
            k22 = -k22;
        }
        
        q[0] = q0x; q[1] = q1x; q[2] = q2x;
        q[3] = q0y; q[4] = q1y; q[5] = q2y;
        q[6] = q0z; q[7] = q1z; q[8] = q2z;
        
        k[0] = k00;
        k[1] = k01;
        k[2] = k02;
        k[3] = k11;
        k[4] = k12;
        k[5] = k22;
        
        return true;
    }
    
    private static bool Invert(in TransformRecord m, Span<float> result)
    {
        var a = m.R0 * Scale;
        var b = m.R1 * Scale;
        var c = m.R2 * Scale;
        var d = m.R3 * Scale;
        var e = m.R4 * Scale;
        var f = m.R5 * Scale;
        var g = m.R6 * Scale;
        var h = m.R7 * Scale;
        var i = m.R8 * Scale;
        
        var determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (MathF.Abs(determinant) < 1e-6f) return false;
        
        var inverse = 1f / determinant;
        
        result[0] = (e * i - f * h) * inverse;
        result[1] = (c * h - b * i) * inverse;
        result[2] = (b * f - c * e) * inverse;
        result[3] = (f * g - d * i) * inverse;
        result[4] = (a * i - c * g) * inverse;
        result[5] = (c * d - a * f) * inverse;
        result[6] = (d * h - e * g) * inverse;
        result[7] = (b * g - a * h) * inverse;
        result[8] = (a * e - b * d) * inverse;
        
        return true;
    }
    
    private static bool Slerp(Span<float> from, Span<float> to, float weight, Span<float> result)
    {
        Span<float> a = stackalloc float[4];
        Span<float> b = stackalloc float[4];
        
        if (!Quaternion(from, a) || !Quaternion(to, b)) return false;
        
        var dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (dot < 0f)
        {
            dot = -dot;
            b[0] = -b[0];
            b[1] = -b[1];
            b[2] = -b[2];
            b[3] = -b[3];
        }
        
        float wa, wb;
        
        if (dot > 0.9995f)
        {
            wa = 1f - weight;
            wb = weight;
        }
        else
        {
            var angle = MathF.Acos(dot);
            var sin = MathF.Sin(angle);
            wa = MathF.Sin((1f - weight) * angle) / sin;
            wb = MathF.Sin(weight * angle) / sin;
        }
        
        var x = a[0] * wa + b[0] * wb;
        var y = a[1] * wa + b[1] * wb;
        var z = a[2] * wa + b[2] * wb;
        var w = a[3] * wa + b[3] * wb;
        
        var length = MathF.Sqrt(x * x + y * y + z * z + w * w);
        if (length < 1e-6f) return false;
        
        x /= length;
        y /= length;
        z /= length;
        w /= length;
        
        result[0] = 1f - 2f * (y * y + z * z);
        result[1] = 2f * (x * y - z * w);
        result[2] = 2f * (x * z + y * w);
        result[3] = 2f * (x * y + z * w);
        result[4] = 1f - 2f * (x * x + z * z);
        result[5] = 2f * (y * z - x * w);
        result[6] = 2f * (x * z - y * w);
        result[7] = 2f * (y * z + x * w);
        result[8] = 1f - 2f * (x * x + y * y);
        
        return true;
    }
    
    private static bool Quaternion(Span<float> m, Span<float> q)
    {
        var m00 = m[0];
        var m01 = m[1];
        var m02 = m[2];
        var m10 = m[3];
        var m11 = m[4];
        var m12 = m[5];
        var m20 = m[6];
        var m21 = m[7];
        var m22 = m[8];
        
        var trace = m00 + m11 + m22;
        
        if (trace > 0f)
        {
            var s = MathF.Sqrt(trace + 1f) * 2f;
            q[3] = 0.25f * s;
            q[0] = (m21 - m12) / s;
            q[1] = (m02 - m20) / s;
            q[2] = (m10 - m01) / s;
            return true;
        }
        
        if (m00 > m11 && m00 > m22)
        {
            var s = MathF.Sqrt(1f + m00 - m11 - m22) * 2f;
            q[3] = (m21 - m12) / s;
            q[0] = 0.25f * s;
            q[1] = (m01 + m10) / s;
            q[2] = (m02 + m20) / s;
            return true;
        }
        
        if (m11 > m22)
        {
            var s = MathF.Sqrt(1f + m11 - m00 - m22) * 2f;
            q[3] = (m02 - m20) / s;
            q[0] = (m01 + m10) / s;
            q[1] = 0.25f * s;
            q[2] = (m12 + m21) / s;
            return true;
        }
        
        var t = MathF.Sqrt(1f + m22 - m00 - m11) * 2f;
        q[3] = (m10 - m01) / t;
        q[0] = (m02 + m20) / t;
        q[1] = (m12 + m21) / t;
        q[2] = 0.25f * t;
        return true;
    }
    
    private static float Row(short ax, short ay, short az, short bx, short by, short bz)
    {
        var a = MathF.Sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
        var b = MathF.Sqrt((float)bx * bx + (float)by * by + (float)bz * bz);
        if (a < 1e-6f || b < 1e-6f) return 1f;
        
        return 1f - ((float)ax * bx + (float)ay * by + (float)az * bz) / (a * b);
    }
    
    private static float Determinant(in TransformRecord m)
    {
        return m.R0 * ((float)m.R4 * m.R8 - (float)m.R5 * m.R7) -
               m.R1 * ((float)m.R3 * m.R8 - (float)m.R5 * m.R6) +
               m.R2 * ((float)m.R3 * m.R7 - (float)m.R4 * m.R6);
    }
}
