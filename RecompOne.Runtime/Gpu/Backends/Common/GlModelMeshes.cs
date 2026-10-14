using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

/// <summary>
/// 0085. The models drawn from meshes kept on the GPU (Step 3's second slice): each
/// mesh's opaque faces uploaded once (<see cref="RetainedScene.MeshCorners"/>), and
/// each frame only the posed vertices, into a buffer texture per frame of the ring, and
/// a record per model. The world programs place, light and cull every corner from
/// those (<c>ModelGlsl</c>). See "Step 3, the second slice" in docs/GPU_RENDERER.md.
/// The third slice keeps the vertices too (<see cref="RetainedScene.PoseStore"/>): a
/// rigid model's as they are, an MO pose's keyframe and deltas, blended in the shader,
/// so an instance drawn from the store uploads nothing a frame.
/// </summary>
public sealed partial class GlCore
{
    const int ModelVertsUnit = 19, PoseUnit = 20;

    uint _poseBuf, _poseTex;
    int _poseUploaded, _poseCap;

    uint _meshVbo, _meshMipVbo, _meshVao;
    int _meshUploaded, _meshCap, _meshGen = -1;
    uint[] _meshMip = [];

    // The meshes' textures: a key each, and a table of their atlas entries the corners
    // index (bound on the static map's unit while models draw).
    readonly Dictionary<(int, int, uint), int> _mdlKeyAt = new();
    readonly List<(int TPage, int Clut, uint Rect)> _mdlKeys = new();
    readonly Dictionary<int, int[]> _meshKeys = new();
    uint[] _mdlKeyEntry = [];
    int[] _mdlKeyLooked = [];
    int _mdlLookSerial;
    bool _mdlTableDirty;
    uint _mdlTableBuf, _mdlTableTex;

    // The posed vertices, per frame of the ring, and the mesh store they were drawn with.
    readonly uint[] _mvBuf = new uint[ModelRing], _mvTex = new uint[ModelRing];
    readonly int[] _mvSerial = new int[ModelRing], _mvGen = new int[ModelRing], _mvCap = new int[ModelRing];

    int _uwModel = -1, _uwModelBase, _uwModelR, _uwModelT, _uwModelFar, _uwModelNear, _uwModelLlm, _uwModelCue, _uwModelRgbc, _uwModelMat, _uwModelGteC = -1;
    int _uwnModel = -1, _uwnModelBase, _uwnModelR, _uwnModelT, _uwnModelFar, _uwnModelNear, _uwnModelMat, _uwnModelGteC = -1;
    int _uwModelPose = -1, _uwModelPoseW = -1, _uwnModelPose = -1, _uwnModelPoseW = -1, _uwModelSky = -1;
    int _uwModelTile = -1, _uwnModelTile = -1;
    // 0098. ModelInstance.FadeOut, as the weight drawn, per program.
    int _uwModelKeep = -1, _uwnModelKeep = -1;
    // 0103. RetainedScene.ModelNearClip, per program.
    int _uwModelClip = -1, _uwnModelClip = -1;
    // The view-space placement (the arm), per program: uModelView, then the three rows and T.
    readonly int[] _uwView = [-1, -1, -1, -1, -1], _uwnView = [-1, -1, -1, -1, -1];
    static readonly string[] ViewNames = ["uModelView", "uModelVR0", "uModelVR1", "uModelVR2", "uModelVT"];

    void InitModelMeshes()
    {
        if (_progWorld == 0) return;
        int L(string n) => _gl.GetUniformLocation(_progWorld, n);
        _uwModel = L("uModel"); _uwModelBase = L("uModelBase"); _uwModelR = L("uModelR"); _uwModelT = L("uModelT");
        _uwModelFar = L("uModelFar"); _uwModelNear = L("uModelNear"); _uwModelLlm = L("uModelLlm"); _uwModelCue = L("uModelCue");
        _uwModelRgbc = L("uModelRgbc"); _uwModelMat = L("uModelMat"); _uwModelGteC = L("uModelGteC");
        _uwModelPose = L("uModelPose"); _uwModelPoseW = L("uModelPoseW"); _uwModelSky = L("uModelSky");
        _uwModelTile = L("uModelTile"); _uwModelKeep = L("uModelKeep"); _uwModelClip = L("uModelClip");
        for (int i = 0; i < ViewNames.Length; i++) _uwView[i] = L(ViewNames[i]);
        _gl.UseProgram(_progWorld);
        if (_uwModelKeep >= 0) _gl.Uniform1(_uwModelKeep, 1f);
        if (_uwModelSky >= 0) _gl.Uniform1(_uwModelSky, 0);
        if (_uwModelTile >= 0) _gl.Uniform1(_uwModelTile, 0);
        if (_uwView[0] >= 0) _gl.Uniform1(_uwView[0], 0);
        if (_uwModel >= 0) _gl.Uniform1(_uwModel, 0);
        int u = L("uModelVerts");
        if (u >= 0) _gl.Uniform1(u, ModelVertsUnit);
        u = L("uModelPoses");
        if (u >= 0) _gl.Uniform1(u, PoseUnit);
        if (_progWorldNrm != 0)
        {
            int N(string n) => _gl.GetUniformLocation(_progWorldNrm, n);
            _uwnModel = N("uModel"); _uwnModelBase = N("uModelBase"); _uwnModelR = N("uModelR"); _uwnModelT = N("uModelT");
            _uwnModelFar = N("uModelFar"); _uwnModelNear = N("uModelNear"); _uwnModelMat = N("uModelMat"); _uwnModelGteC = N("uModelGteC");
            _uwnModelPose = N("uModelPose"); _uwnModelPoseW = N("uModelPoseW"); _uwnModelTile = N("uModelTile");
            _uwnModelKeep = N("uModelKeep"); _uwnModelClip = N("uModelClip");
            for (int i = 0; i < ViewNames.Length; i++) _uwnView[i] = N(ViewNames[i]);
            _gl.UseProgram(_progWorldNrm);
            if (_uwnModelKeep >= 0) _gl.Uniform1(_uwnModelKeep, 1f);
            if (_uwnView[0] >= 0) _gl.Uniform1(_uwnView[0], 0);
            if (_uwnModelTile >= 0) _gl.Uniform1(_uwnModelTile, 0);
            if (_uwnModel >= 0) _gl.Uniform1(_uwnModel, 0);
            int v = N("uModelVerts");
            if (v >= 0) _gl.Uniform1(v, ModelVertsUnit);
            v = N("uModelPoses");
            if (v >= 0) _gl.Uniform1(v, PoseUnit);
        }
        _gl.UseProgram(0);
        _meshVbo = _gl.GenBuffer();
        _meshMipVbo = _gl.GenBuffer();
        _meshVao = MakeWorldVao(_meshVbo, _meshMipVbo);
        // The table holds a word before any mesh has a texture.
        _mdlTableBuf = _gl.GenBuffer();
        _mdlTableTex = _gl.GenTexture();
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, _mdlTableBuf);
        _gl.BufferData<uint>(BufferTargetARB.TextureBuffer, [0u], BufferUsageARB.DynamicDraw);
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
        _gl.BindTexture(TextureTarget.TextureBuffer, _mdlTableTex);
        _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.R32ui, _mdlTableBuf);
        _gl.BindTexture(TextureTarget.TextureBuffer, 0);
        // The pose store holds a texel before any pose is kept.
        _poseBuf = _gl.GenBuffer();
        _poseTex = _gl.GenTexture();
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, _poseBuf);
        _gl.BufferData<short>(BufferTargetARB.TextureBuffer, [0, 0, 0, 0], BufferUsageARB.DynamicDraw);
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
        _gl.BindTexture(TextureTarget.TextureBuffer, _poseTex);
        _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.Rgba16i, _poseBuf);
        _gl.BindTexture(TextureTarget.TextureBuffer, 0);
    }

    /// <summary>Whether both programs can draw an instance.</summary>
    bool InstancesReady => _uwModel >= 0 && _meshVao != 0;

    /// <summary>The store's corners not yet on the GPU, and their textures' keys; the
    /// whole store again after it was emptied.</summary>
    unsafe void UploadMeshes()
    {
        if (_meshGen != RetainedScene.MeshGeneration)
        {
            _meshGen = RetainedScene.MeshGeneration;
            _meshUploaded = 0;
            _poseUploaded = 0;
            _mdlKeyAt.Clear();
            _mdlKeys.Clear();
            _meshKeys.Clear();
        }
        UploadPoses();
        int n = RetainedScene.MeshCornerCount;
        if (n <= _meshUploaded) return;
        var all = RetainedScene.MeshCorners;
        if (_meshMip.Length < n) Array.Resize(ref _meshMip, Math.Max(n, _meshMip.Length * 2));
        for (int i = _meshUploaded; i + 2 < n; i += 3)
            _meshMip[i] = _meshMip[i + 1] = _meshMip[i + 2] = (uint)(ModelKey(all[i]) + 1);

        int from = _meshUploaded;
        if (n > _meshCap)
        {
            // Grown: everything again, into a buffer with room.
            _meshCap = Math.Max(n, _meshCap * 2);
            from = 0;
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _meshVbo);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_meshCap * sizeof(RetainedScene.Vertex)), null, BufferUsageARB.DynamicDraw);
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _meshMipVbo);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_meshCap * 4), null, BufferUsageARB.DynamicDraw);
        }
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _meshVbo);
        _gl.BufferSubData<RetainedScene.Vertex>(BufferTargetARB.ArrayBuffer, from * sizeof(RetainedScene.Vertex),
            new ReadOnlySpan<RetainedScene.Vertex>(all, from, n - from));
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _meshMipVbo);
        _gl.BufferSubData<uint>(BufferTargetARB.ArrayBuffer, from * 4, new ReadOnlySpan<uint>(_meshMip, from, n - from));
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        _meshUploaded = n;
        if (_mdlKeyEntry.Length < _mdlKeys.Count)
        {
            Array.Resize(ref _mdlKeyEntry, Math.Max(_mdlKeys.Count, _mdlKeyEntry.Length * 2));
            Array.Resize(ref _mdlKeyLooked, _mdlKeyEntry.Length);
            _mdlTableDirty = true;
        }
    }

    /// <summary>The pose store's texels not yet on the GPU; all of them into a larger
    /// buffer when it has outgrown its own.</summary>
    unsafe void UploadPoses()
    {
        int n = RetainedScene.PoseTexels;
        if (n <= _poseUploaded) return;
        int from = _poseUploaded;
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, _poseBuf);
        if (n > _poseCap)
        {
            _poseCap = Math.Max(n, _poseCap * 2);
            from = 0;
            _gl.BufferData(BufferTargetARB.TextureBuffer, (nuint)(_poseCap * 8), null, BufferUsageARB.DynamicDraw);
            // The texture holds the buffer's storage, which BufferData replaced.
            _gl.BindTexture(TextureTarget.TextureBuffer, _poseTex);
            _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.Rgba16i, _poseBuf);
            _gl.BindTexture(TextureTarget.TextureBuffer, 0);
        }
        _gl.BufferSubData<short>(BufferTargetARB.TextureBuffer, from * 8,
            new ReadOnlySpan<short>(RetainedScene.PoseStore, from * 4, (n - from) * 4));
        _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
        RetainedScene.PoseTexelsUploaded += n - from;
        _poseUploaded = n;
    }

    int ModelKey(in RetainedScene.Vertex v)
    {
        int tp = (int)(v.Texpage + 0.5f), cl = (int)(v.Clut + 0.5f);
        if ((tp & 0x8000) != 0 || (v.Flags & RetainedScene.FlagRect) == 0) return -1;
        var key = (tp & 0x1FF, cl, v.Rect);
        if (_mdlKeyAt.TryGetValue(key, out int k)) return k;
        _mdlKeyAt[key] = k = _mdlKeys.Count;
        _mdlKeys.Add(key);
        return k;
    }

    /// <summary>A mesh's distinct textures, by its first corner.</summary>
    int[] MeshKeys(int start, int count)
    {
        if (_meshKeys.TryGetValue(start, out var keys)) return keys;
        var set = new HashSet<int>();
        for (int i = start; i < start + count && i < _meshUploaded; i += 3)
            if (_meshMip[i] != 0) set.Add((int)_meshMip[i] - 1);
        return _meshKeys[start] = set.ToArray();
    }

    /// <summary>
    /// A view's instances ready to draw, before the target is bound (the atlas decode
    /// draws): the store uploaded, the frame's posed vertices in its slot of the ring,
    /// and the atlas entries of the textures these instances' meshes use. The slot, or
    /// -1 with nothing to draw.
    /// </summary>
    unsafe int PrepareInstances(RetainedScene.Frame f, List<RetainedScene.ModelInstance> list, bool mips)
    {
        if (list.Count == 0 || !InstancesReady) return -1;
        UploadMeshes();
        int slot = f.Serial & (ModelRing - 1);
        if (_mvSerial[slot] != f.Serial || _mvGen[slot] != _meshGen)
        {
            if (_mvBuf[slot] == 0) { _mvBuf[slot] = _gl.GenBuffer(); _mvTex[slot] = _gl.GenTexture(); }
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, _mvBuf[slot]);
            // Every instance may come from the pose store; the buffer still needs storage.
            int bytes = Math.Max(f.VertCount, 1) * 8;
            if (bytes > _mvCap[slot])
            {
                _mvCap[slot] = Math.Max(bytes, _mvCap[slot] * 2);
                _gl.BufferData(BufferTargetARB.TextureBuffer, (nuint)_mvCap[slot], null, BufferUsageARB.StreamDraw);
            }
            _gl.BufferSubData<short>(BufferTargetARB.TextureBuffer, 0, new ReadOnlySpan<short>(f.Verts, 0, f.VertCount * 4));
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
            _gl.BindTexture(TextureTarget.TextureBuffer, _mvTex[slot]);
            _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.Rgba16i, _mvBuf[slot]);
            _gl.BindTexture(TextureTarget.TextureBuffer, 0);
            _mvSerial[slot] = f.Serial;
            _mvGen[slot] = _meshGen;
            RetainedScene.InstanceVertices += f.VertCount;
        }

        bool on = mips && _mip != null;
        _mdlLookSerial++;
        foreach (var m in list)
            if (m.MeshGen == _meshGen)
            foreach (int k in MeshKeys(m.MeshStart, Math.Max(m.MeshCount, m.MeshAll)))
            {
                if (_mdlKeyLooked[k] == _mdlLookSerial) continue;
                _mdlKeyLooked[k] = _mdlLookSerial;
                var (tp, cl, rect) = _mdlKeys[k];
                uint e = on ? MipOf(tp, cl, rect) : 0u;
                if (e != _mdlKeyEntry[k]) { _mdlKeyEntry[k] = e; _mdlTableDirty = true; }
            }
        if (_mdlTableDirty)
        {
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, _mdlTableBuf);
            _gl.BufferData<uint>(BufferTargetARB.TextureBuffer, _mdlKeyEntry, BufferUsageARB.DynamicDraw);
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
            _gl.BindTexture(TextureTarget.TextureBuffer, _mdlTableTex);
            _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.R32ui, _mdlTableBuf);
            _gl.BindTexture(TextureTarget.TextureBuffer, 0);
            _mdlTableDirty = false;
        }
        if (on && _mip!.HasPending) _mip.Process(_vram.SampleTexture);
        return slot;
    }

    /// <summary>The instances' textures in place of the static map's, and the frame's
    /// posed vertices; <see cref="EndInstances"/> puts the map's back.</summary>
    void BeginInstances(int slot, bool colour)
    {
        _gl.BindVertexArray(_meshVao);
        _gl.ActiveTexture(TextureUnit.Texture0 + ModelVertsUnit);
        _gl.BindTexture(TextureTarget.TextureBuffer, _mvTex[slot]);
        _gl.ActiveTexture(TextureUnit.Texture0 + PoseUnit);
        _gl.BindTexture(TextureTarget.TextureBuffer, _poseTex);
        _gl.ActiveTexture(TextureUnit.Texture0 + MipTableUnit);
        _gl.BindTexture(TextureTarget.TextureBuffer, _mdlTableTex);
        _gl.ActiveTexture(TextureUnit.Texture0);
        if (colour)
        {
            if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 1);
            _gl.Uniform1(_uwModel, 1);
        }
        else if (_uwnModel >= 0) _gl.Uniform1(_uwnModel, 1);
    }

    void EndInstances(bool colour)
    {
        var view = colour ? _uwView : _uwnView;
        if (view[0] >= 0) _gl.Uniform1(view[0], 0);
        if (colour)
        {
            if (_uwModelSky >= 0) _gl.Uniform1(_uwModelSky, 0);
            if (_uwModelTile >= 0) _gl.Uniform1(_uwModelTile, 0);
            if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 0);
            _gl.Uniform1(_uwModel, 0);
        }
        else
        {
            if (_uwnModelTile >= 0) _gl.Uniform1(_uwnModelTile, 0);
            if (_uwnModel >= 0) _gl.Uniform1(_uwnModel, 0);
        }
        _gl.ActiveTexture(TextureUnit.Texture0 + MipTableUnit);
        _gl.BindTexture(TextureTarget.TextureBuffer, _mipTableTex);
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    readonly float[] _m9 = new float[9];

    /// <summary>One instance's record into the bound program's uniforms: the colour
    /// program's, or the normal program's (placement, the cull and the material).</summary>
    void SendInstance(in RetainedScene.ModelInstance m, bool colour)
    {
        _m9[0] = m.R00; _m9[1] = m.R01; _m9[2] = m.R02; _m9[3] = m.R10; _m9[4] = m.R11; _m9[5] = m.R12;
        _m9[6] = m.R20; _m9[7] = m.R21; _m9[8] = m.R22;
        // The store's first texel, or -1 for the frame's vertices; -1 a rigid weight.
        int pose = m.Pose - 1, weight = m.PoseMorph ? m.PoseWeight : -1;
        var view = colour ? _uwView : _uwnView;
        if (view[0] >= 0)
        {
            _gl.Uniform1(view[0], m.ViewSpace ? 1 : 0);
            if (m.ViewSpace)
            {
                _gl.Uniform3(view[1], m.V00, m.V01, m.V02);
                _gl.Uniform3(view[2], m.V10, m.V11, m.V12);
                _gl.Uniform3(view[3], m.V20, m.V21, m.V22);
                _gl.Uniform3(view[4], m.Vtx, m.Vty, m.Vtz);
            }
        }
        if (!colour)
        {
            _gl.Uniform1(_uwnModelBase, m.VertBase);
            if (_uwnModelTile >= 0) _gl.Uniform1(_uwnModelTile, m.Tile ? 1 : 0);
            if (_uwnModelClip >= 0) _gl.Uniform1(_uwnModelClip, RetainedScene.ModelNearClip ? 1 : 0);
            if (_uwnModelPose >= 0) _gl.Uniform1(_uwnModelPose, pose);
            if (_uwnModelPoseW >= 0) _gl.Uniform1(_uwnModelPoseW, weight);
            _gl.UniformMatrix3(_uwnModelR, 1, true, _m9);
            _gl.Uniform3(_uwnModelT, m.Tx, m.Ty, m.Tz);
            _gl.Uniform1(_uwnModelFar, m.Far);
            _gl.Uniform1(_uwnModelNear, m.Near);
            if (_uwnModelMat >= 0) _gl.Uniform1(_uwnModelMat, m.Material);
            if (_uwnModelKeep >= 0) _gl.Uniform1(_uwnModelKeep, 1f - Math.Clamp(m.FadeOut, 0f, 1f));
            return;
        }
        _gl.Uniform1(_uwModelBase, m.VertBase);
        if (_uwModelSky >= 0) _gl.Uniform1(_uwModelSky, m.Sky ? 1 : 0);
        if (_uwModelTile >= 0) _gl.Uniform1(_uwModelTile, m.Tile ? 1 : 0);
        if (_uwModelClip >= 0) _gl.Uniform1(_uwModelClip, RetainedScene.ModelNearClip ? 1 : 0);
        if (_uwModelPose >= 0) _gl.Uniform1(_uwModelPose, pose);
        if (_uwModelPoseW >= 0) _gl.Uniform1(_uwModelPoseW, weight);
        _gl.UniformMatrix3(_uwModelR, 1, true, _m9);
        _gl.Uniform3(_uwModelT, m.Tx, m.Ty, m.Tz);
        _gl.Uniform1(_uwModelFar, m.Far);
        _gl.Uniform1(_uwModelNear, m.Near);
        _m9[0] = m.Llm0; _m9[1] = m.Llm1; _m9[2] = m.Llm2; _m9[3] = m.Llm3; _m9[4] = m.Llm4; _m9[5] = m.Llm5;
        _m9[6] = m.Llm6; _m9[7] = m.Llm7; _m9[8] = m.Llm8;
        if (_uwModelLlm >= 0) _gl.UniformMatrix3(_uwModelLlm, 1, true, _m9);
        if (_uwModelCue >= 0) _gl.Uniform3(_uwModelCue, m.Dqa, m.Dqb, m.Curve);
        if (_uwModelRgbc >= 0) _gl.Uniform1(_uwModelRgbc, m.Rgbc);
        if (_uwModelMat >= 0) _gl.Uniform1(_uwModelMat, m.Material);
        if (_uwModelKeep >= 0) _gl.Uniform1(_uwModelKeep, 1f - Math.Clamp(m.FadeOut, 0f, 1f));
        if (_uwBk >= 0) _gl.Uniform3(_uwBk, m.Bk0, m.Bk1, m.Bk2);
        if (_uwLcmR >= 0) _gl.Uniform3(_uwLcmR, m.L0, m.L1, m.L2);
        if (_uwLcmG >= 0) _gl.Uniform3(_uwLcmG, m.L3, m.L4, m.L5);
        if (_uwLcmB >= 0) _gl.Uniform3(_uwLcmB, m.L6, m.L7, m.L8);
    }

    /// <summary>
    /// A view's instances after the map, as <see cref="DrawWorldModels"/> draws the
    /// captured models: 0051's true depth first with colour off, then colour against
    /// it pulled towards the camera. Not culled by GL: the shader keeps the faces the
    /// lit assembler keeps.
    /// </summary>
    void DrawInstances(List<RetainedScene.ModelInstance> list, int slot)
    {
        _gl.Disable(EnableCap.CullFace);
        BeginInstances(slot, true);
        bool bias = GteDepth.ZBuffer && (GteDepth.DepthBias > 0f || GteDepth.DepthSlope > 0f);
        ProbeModelsUnderMap(() =>
        {
            foreach (var m in list)
            {
                if (m.MeshGen != _meshGen) continue;
                SendInstance(m, true);
                _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
            }
        });
        if (bias)
        {
            DepthOnly(true);
            foreach (var m in list)
            {
                if (m.MeshGen != _meshGen) continue;
                SendInstance(m, true);
                _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
            }
            DepthOnly(false);
            _gl.DepthMask(false);
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, GteDepth.DepthBias / 65536f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, GteDepth.DepthSlope);
        }
        if (bias)
            ProbeTolerance(2, () =>
            {
                foreach (var m in list)
                {
                    if (m.MeshGen != _meshGen) continue;
                    SendInstance(m, true);
                    _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
                }
            });
        MarkModels(true);
        foreach (var m in list)
        {
            // A store emptied since the instance was made holds other meshes there now.
            if (m.MeshGen != _meshGen) continue;
            SendInstance(m, true);
            _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
            RetainedScene.InstanceCorners += m.MeshCount;
        }
        MarkModels(false);
        if (bias)
        {
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
            _gl.DepthMask(true);
        }
        EndInstances(true);
    }

    int _uwFarPlane = -1;

    // The frame's sky faces in the order the walk sends them, as corners in the store.
    RetainedScene.SkyFace[] _skyEnt = [];
    int[] _skyIdx = [];
    uint _skyEbo;

    /// <summary>
    /// The frame's sky (<see cref="RetainedScene.AddSky"/>), into the bound target ahead
    /// of the map, as the table walks its packets: far slot first, the last linked first
    /// within a slot, with no depth test. An opaque face writes the far plane, as an
    /// unrecorded packet does under the occlusion pass (zMode 3); a blended one writes no
    /// depth (zMode 0). The world program is bound for the frame.
    /// </summary>
    unsafe void DrawSky(RetainedScene.Frame f, int slot)
    {
        var src = f.SkyFaces;
        int n = src.Count;
        if (n == 0 || slot < 0 || !RetainedScene.MainModelsShown || !RetainedScene.SkyShown) return;
        if (_uwFarPlane < 0) _uwFarPlane = _gl.GetUniformLocation(_progWorld, "uFarPlane");
        if (_skyEnt.Length < n) _skyEnt = new RetainedScene.SkyFace[n * 2];
        int m = 0, corners = 0;
        for (int i = 0; i < n; i++)
        {
            var e = src[i];
            if (f.Sky[e.Inst].MeshGen != _meshGen) continue;
            _skyEnt[m++] = e;
            corners += e.Corners;
        }
        if (m == 0) { RetainedScene.SkyMissed++; return; }
        Array.Sort(_skyEnt, 0, m, Comparer<RetainedScene.SkyFace>.Create((a, b) =>
            a.Key != b.Key ? b.Key.CompareTo(a.Key) : b.Seq.CompareTo(a.Seq)));
        if (_skyIdx.Length < corners) _skyIdx = new int[corners * 2];
        int at = 0;
        for (int i = 0; i < m; i++)
            for (int j = 0; j < _skyEnt[i].Corners; j++) _skyIdx[at++] = _skyEnt[i].Corner + j;

        _gl.Disable(EnableCap.CullFace);
        BeginInstances(slot, true);
        if (_skyEbo == 0) _skyEbo = _gl.GenBuffer();
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, _skyEbo);
        _gl.BufferData<int>(BufferTargetARB.ElementArrayBuffer, new ReadOnlySpan<int>(_skyIdx, 0, at), BufferUsageARB.StreamDraw);
        _gl.DepthFunc(DepthFunction.Always);
        bool far = GteDepth.SurfacesWanted;
        int from = 0;
        for (int i = 0; i < m;)
        {
            int j = i + 1, inst = _skyEnt[i].Inst, mode = _skyEnt[i].Mode;
            int count = _skyEnt[i].Corners;
            while (j < m && _skyEnt[j].Inst == inst && _skyEnt[j].Mode == mode) count += _skyEnt[j++].Corners;
            SendInstance(f.Sky[inst], true);
            if (mode < 0)
            {
                _gl.Disable(EnableCap.Blend);
                _gl.DepthMask(far);
                if (_uwFarPlane >= 0) _gl.Uniform1(_uwFarPlane, far ? 1 : 0);
                _gl.DrawElements(PrimitiveType.Triangles, (uint)count, DrawElementsType.UnsignedInt, (void*)(from * 4L));
            }
            else
            {
                _gl.DepthMask(false);
                if (_uwFarPlane >= 0) _gl.Uniform1(_uwFarPlane, 0);
                DrawBlended(mode, count, from * 4L);
            }
            from += count;
            i = j;
        }
        if (_uwFarPlane >= 0) _gl.Uniform1(_uwFarPlane, 0);
        _gl.Disable(EnableCap.Blend);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.DepthMask(true);
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, 0);
        EndInstances(true);
        RetainedScene.SkyDrawn += f.Sky.Count;
        RetainedScene.SkyFacesDrawn += m;
    }

    /// <summary>
    /// Elements of the bound VAO and element buffer, blended as GlCore blends a packet at
    /// the console's rate <paramref name="mode"/>: dual-source, the texels without the
    /// semi-transparency bit opaque. Mode 2 subtracts, which no one blend function does
    /// for a face with opaque texels too: those first with the rest left as they are, then
    /// the rest subtracted with the opaque ones left. The blend is left on.
    /// </summary>
    unsafe void DrawBlended(int mode, int count, long offset)
    {
        _gl.Enable(EnableCap.Blend);
        _gl.BlendFuncSeparate(BlendingFactor.Src1Color, BlendingFactor.Src1Alpha, BlendingFactor.One, BlendingFactor.Zero);
        if (_uwAtmosSkip >= 0) _gl.Uniform1(_uwAtmosSkip, mode == 0 ? 0 : 1);
        if (mode == 2)
        {
            _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
            if (_uwBlend >= 0) _gl.Uniform4(_uwBlend, 0f, 0f, 0f, 1f);
            _gl.DrawElements(PrimitiveType.Triangles, (uint)count, DrawElementsType.UnsignedInt, (void*)offset);
            _gl.BlendEquationSeparate(BlendEquationModeEXT.FuncReverseSubtract, BlendEquationModeEXT.FuncAdd);
            if (_uwBlend >= 0) _gl.Uniform4(_uwBlend, 1f, 1f, 1f, 1f);
            if (_uwBlendOpaque >= 0) _gl.Uniform4(_uwBlendOpaque, 0f, 0f, 0f, 1f);
            _gl.DrawElements(PrimitiveType.Triangles, (uint)count, DrawElementsType.UnsignedInt, (void*)offset);
            if (_uwBlendOpaque >= 0) _gl.Uniform4(_uwBlendOpaque, 1f, 1f, 1f, 0f);
            _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
        }
        else
        {
            _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
            float a = mode switch { 0 => 0.5f, 3 => 0.25f, _ => 1f }, d = mode == 0 ? 0.5f : 1f;
            if (_uwBlend >= 0) _gl.Uniform4(_uwBlend, a, a, a, d);
            _gl.DrawElements(PrimitiveType.Triangles, (uint)count, DrawElementsType.UnsignedInt, (void*)offset);
        }
        if (_uwAtmosSkip >= 0) _gl.Uniform1(_uwAtmosSkip, 0);
    }
    readonly List<RetainedScene.ModelInstance> _armList = new(1);

    uint _armEbo;

    /// <summary>
    /// The first-person arm, as the table's walk reaches its faces: every run of one key
    /// the walk has passed (<see cref="RetainedScene.ArmCut"/>), in the order the walk
    /// would have sent their packets, with no depth test, and the far plane written
    /// where it drew, as its unrecorded packets leave the depth under them (GlCore's
    /// zMode 3). What the walk sends between runs is drawn over the ones before it
    /// where it passes its own test, as it was over the packets. True while runs are
    /// left, with <see cref="RetainedScene.ArmSlot"/> the next one's slot.
    /// </summary>
    unsafe bool DrawWorldArm(int offX, int offY)
    {
        var f = RetainedScene.Find(RetainedScene.ArmSerial);
        if (f == null || !f.HasArm || _progWorld == 0 || !InstancesReady) { RetainedScene.ArmMissed++; return false; }
        int first = f.ArmNext, last = first;
        while (last < f.ArmRuns && 0x1FFF - f.ArmRunKey[last] <= RetainedScene.ArmCut) last++;
        f.ArmNext = last;
        if (last < f.ArmRuns) RetainedScene.ArmSlot = 0x1FFF - f.ArmRunKey[last];
        if (last == first) return last < f.ArmRuns;
        // The probe's hide leaves it out, as it leaves out the other models.
        if (!RetainedScene.MainModelsShown) return last < f.ArmRuns;
        if (_uwFarPlane < 0) _uwFarPlane = _gl.GetUniformLocation(_progWorld, "uFarPlane");
        Flush(FlushReason.Target);
        var rt = ClassifyDisplay();
        if (rt == null || _uwFarPlane < 0) { RetainedScene.ArmMissed++; return false; }
        _armList.Clear();
        _armList.Add(f.Arm);
        int inst = PrepareInstances(f, _armList, _mainMips);
        // The store is uploaded now; one emptied since holds other meshes there.
        if (inst < 0 || f.Arm.MeshGen != _meshGen) { RetainedScene.ArmMissed++; return false; }

        uint query = BeginGpuTimer();
        if (_wOpen) BindWorldMain(rt, _mainMips);
        else
        {
            BeginWorldMain(f, rt, offX, offY, _mainMips);
            _wOpen = true;
        }
        _gl.Disable(EnableCap.CullFace);
        _gl.Disable(EnableCap.Blend);
        BeginInstances(inst, true);
        SendInstance(f.Arm, true);
        if (_armEbo == 0) _armEbo = _gl.GenBuffer();
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, _armEbo);
        if (first == 0)
            _gl.BufferData<int>(BufferTargetARB.ElementArrayBuffer, new ReadOnlySpan<int>(f.ArmOrder, 0, f.ArmOrderCount),
                                BufferUsageARB.StreamDraw);
        int from = f.ArmRunAt[first], to = last < f.ArmRuns ? f.ArmRunAt[last] : f.ArmOrderCount;
        _gl.DepthMask(true);
        _gl.DepthFunc(DepthFunction.Always);
        _gl.Uniform1(_uwFarPlane, 1);
        _gl.DrawElements(PrimitiveType.Triangles, (uint)(to - from), DrawElementsType.UnsignedInt, (void*)(from * 4L));
        _gl.Uniform1(_uwFarPlane, 0);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, 0);
        EndInstances(true);
        EndWorldState();
        EndGpuTimer(query, GpuWork.Batch, 0, Diagnostics.GpuTimes.Pass.World);
        RetainedScene.InstanceCorners += to - from;
        RetainedScene.ArmCalls++;
        MarkDrawn(rt);
        // The normal pass puts it in as an overlay where the list had its first run
        // (DrawArmNormals).
        if (AoGeometry.Active && first == 0)
        {
            rt.Geo.Frame(_frame, GteDepth.Generation);
            rt.Geo.ArmAt = rt.Geo.Count;
            rt.Geo.ArmSerial = f.Serial;
        }
        if (first == 0) RetainedScene.ArmDraws++;
        return last < f.ArmRuns;
    }

    /// <summary>The arm into the normal and surface buffers as an overlay, at its place
    /// in the list: the surface under it is then no surface for the passes, as its
    /// packets made it, and nothing is murked or reflected over it.</summary>
    void DrawArmNormals(GlDisplayRt src)
    {
        var f = RetainedScene.Find(src.Geo.ArmSerial);
        int slot = src.Geo.ArmSerial & (ModelRing - 1);
        if (f == null || !f.HasArm || !_wnReady || _uwnModel < 0 || f.Arm.MeshGen != _meshGen
            || _mvSerial[slot] != f.Serial || _mvGen[slot] != _meshGen) return;
        _gl.UseProgram(_progWorldNrm);
        if (_uwnDepthCull >= 0) _gl.Uniform1(_uwnDepthCull, 0);
        BeginInstances(slot, false);
        var m = f.Arm;
        m.Material = SurfaceMaterial.Overlay;
        SendInstance(m, false);
        _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
        EndInstances(false);
        if (_uwnDepthCull >= 0) _gl.Uniform1(_uwnDepthCull, 1);
    }

    /// <summary>The frame's instances into the normal and surface buffers, through the
    /// world normal program; nothing if the frame's vertices have left their slot.</summary>
    void DrawInstanceNormals(RetainedScene.Frame f)
    {
        int slot = f.Serial & (ModelRing - 1);
        if (f.Instances.Count == 0 || _uwnModel < 0 || _mvSerial[slot] != f.Serial || _mvGen[slot] != _meshGen) return;
        BeginInstances(slot, false);
        foreach (var m in f.Instances)
        {
            if (m.MeshGen != _meshGen) continue;
            SendInstance(m, false);
            _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart, (uint)m.MeshCount);
        }
        EndInstances(false);
    }
}
