using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

/// <summary>
/// 0077. Shadows for the authored lights: a depth cubemap per shadowed light, drawn
/// from the retained map (<see cref="RetainedScene"/>) with the world program and the
/// light at its centre, so a texel the game draws as a hole casts none. Each face
/// holds the nearest opaque surface's distance along its axis, over 65536, which is
/// what the world program already writes as its depth. A cubemap is drawn again only
/// when its light moves or the map is rebuilt, from the top of a flush -- before the
/// batch that samples it, so a light is never drawn a frame with no shadow. See
/// "Shadows, the first slice" in docs/REMASTER.md.
///
/// <para>The frame's models cast too: a light with any in its reach samples a second
/// cubemap, the map's copied and the models drawn over it, drawn again only when the
/// models in its reach move. See "Shadows, the second slice".</para>
/// </summary>
public sealed partial class GlCore
{
    const int ShadowUnit = 12;

    int _uLightShadow = -1, _uShadowToWorld, _uShadowSize, _uShadowOffset, _uShadowBias, _uShadowSoft;
    uint _shadowFbo;
    readonly uint[] _shadowTex = new uint[RemasterUniforms.MaxShadows];
    readonly int[] _shadowTexSize = new int[RemasterUniforms.MaxShadows];
    readonly bool[] _shadowReady = new bool[RemasterUniforms.MaxShadows];
    readonly float[] _shadowKey = new float[RemasterUniforms.MaxShadows * 6];
    readonly int[] _shadowSend = new int[RemasterUniforms.MaxLights];
    int _shadowSentGen = -1, _shadowReadyMask = -1, _shadowReadySent = -1;

    // The models: a second cubemap per slot, the map's copied and the casters drawn over it.
    uint _shadowReadFbo, _casterVbo, _casterVao;
    int _casterCap, _casterN, _casterOpaqueN, _casterSerial = -1, _casterUploaded = -1, _uwOpaqueDepth = -1;
    RetainedScene.Vertex[] _casters = new RetainedScene.Vertex[3072];
    readonly uint[] _shadowComb = new uint[RemasterUniforms.MaxShadows];
    readonly int[] _shadowCombSize = new int[RemasterUniforms.MaxShadows];
    readonly uint[] _shadowBind = new uint[RemasterUniforms.MaxShadows];
    readonly int[] _staticVer = new int[RemasterUniforms.MaxShadows];
    readonly int[] _hashFor = new int[RemasterUniforms.MaxShadows];
    readonly ulong[] _casterHash = new ulong[RemasterUniforms.MaxShadows];
    readonly int[] _casterReach = new int[RemasterUniforms.MaxShadows];
    readonly ulong[] _combKey = new ulong[RemasterUniforms.MaxShadows];

    static readonly int[] NoShadows = [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1];

    /// <summary>The bound program's shadow samplers on their own units (a sampler of
    /// another type on unit 0 would fail every draw), and every light unshadowed.</summary>
    void InitShadowUniforms(uint prog, bool prim)
    {
        for (int i = 0; i < RemasterUniforms.MaxShadows; i++)
        {
            int l = _gl.GetUniformLocation(prog, $"uShadow{i}");
            if (l >= 0) _gl.Uniform1(l, ShadowUnit + i);
        }
        int ls = _gl.GetUniformLocation(prog, "uLightShadow");
        if (ls >= 0) _gl.Uniform1(ls, (uint)NoShadows.Length, NoShadows);
        if (!prim) return;
        _uLightShadow = ls;
        _uShadowToWorld = _gl.GetUniformLocation(prog, "uShadowToWorld");
        _uShadowSize = _gl.GetUniformLocation(prog, "uShadowSize");
        _uShadowOffset = _gl.GetUniformLocation(prog, "uShadowOffset");
        _uShadowBias = _gl.GetUniformLocation(prog, "uShadowBias");
        _uShadowSoft = _gl.GetUniformLocation(prog, "uShadowSoft");
        _shadowSentGen = _shadowReadySent = -1;
    }

    /// <summary>From the top of a flush: draw any cubemap whose light or map changed.</summary>
    void UpdateShadows()
    {
        if (!RemasterUniforms.Active || _progWorld == 0 || _uLightShadow < 0) return;
        int size = Math.Clamp(RemasterUniforms.ShadowSize, 64, 4096);
        int gen = RetainedScene.StaticGeneration;
        bool map = RetainedScene.Static.Length > 0;
        int mask = 0;
        bool casters = TakeCasters();
        for (int s = 0; s < RemasterUniforms.MaxShadows; s++)
        {
            int o = s * 4, k = s * 6;
            float r = RemasterUniforms.ShadowLight[o + 3];
            if (r <= 0f || !map) { _shadowReady[s] = false; _combKey[s] = 0; continue; }
            if (!_shadowReady[s] || _shadowKey[k] != RemasterUniforms.ShadowLight[o] || _shadowKey[k + 1] != RemasterUniforms.ShadowLight[o + 1]
                || _shadowKey[k + 2] != RemasterUniforms.ShadowLight[o + 2] || _shadowKey[k + 3] != r
                || _shadowKey[k + 4] != gen || _shadowKey[k + 5] != size)
            {
                RenderShadow(s, size);
                _shadowKey[k] = RemasterUniforms.ShadowLight[o];
                _shadowKey[k + 1] = RemasterUniforms.ShadowLight[o + 1];
                _shadowKey[k + 2] = RemasterUniforms.ShadowLight[o + 2];
                _shadowKey[k + 3] = r;
                _shadowKey[k + 4] = gen;
                _shadowKey[k + 5] = size;
                _shadowReady[s] = true;
                _staticVer[s]++;
                _hashFor[s] = -1;
            }
            _shadowBind[s] = _shadowTex[s];
            if (casters)
            {
                if (_hashFor[s] != _casterSerial)
                {
                    _casterHash[s] = CasterHash(s);
                    _hashFor[s] = _casterSerial;
                }
                if (_casterHash[s] != 0)
                {
                    ulong key = _casterHash[s] * 0x100000001B3ul ^ (ulong)_staticVer[s];
                    if (key != _combKey[s] || _shadowCombSize[s] != size)
                    {
                        RenderShadowModels(s, size);
                        _combKey[s] = key;
                    }
                    _shadowBind[s] = _shadowComb[s];
                }
                else _combKey[s] = 0;
            }
            mask |= 1 << s;
        }
        _shadowReadyMask = mask;
        RemasterUniforms.ShadowsReady = System.Numerics.BitOperations.PopCount((uint)mask);
        int reach = 0;
        for (int s = 0; s < RemasterUniforms.MaxShadows; s++)
            if ((mask & (1 << s)) != 0 && casters) reach += _casterReach[s];
        RemasterUniforms.ShadowCasters = reach;
    }

    /// <summary>The published frame's casters into <see cref="_casters"/>, once a
    /// frame: first what casts with every texel (opaque faces, and a solid blended
    /// model's), then the other blended faces, which cast with their opaque texels.
    /// Effects cast nothing. False when models do not cast; a frame gone from the ring
    /// keeps the last.</summary>
    bool TakeCasters()
    {
        if (!RetainedScene.ShadowModels) return false;
        int serial = RemasterUniforms.ShadowFrame;
        if (serial == _casterSerial) return true;
        var f = RetainedScene.Find(serial);
        if (f == null) return _casterSerial > 0;
        int n = f.DynamicCount / 3 * 3, m = 0;
        if (_casters.Length < n) _casters = new RetainedScene.Vertex[Math.Max(n, _casters.Length * 2)];
        for (int pass = 0; pass < 2; pass++)
        {
            for (int i = 0; i < n; i += 3)
            {
                uint fl = f.Dynamic[i].Flags;
                if ((fl & RetainedScene.FlagNoShadow) != 0) continue;
                bool whole = (fl & RetainedScene.FlagSemi) == 0 || (fl & RetainedScene.FlagSolid) != 0;
                if (whole != (pass == 0)) continue;
                f.Dynamic.AsSpan(i, 3).CopyTo(_casters.AsSpan(m));
                m += 3;
            }
            if (pass == 0) _casterOpaqueN = m;
        }
        _casterN = m;
        _casterSerial = serial;
        return true;
    }

    /// <summary>A hash of the casters in slot <paramref name="s"/>'s reach, by their
    /// corners' bits; 0 when none is.</summary>
    ulong CasterHash(int s)
    {
        int o = s * 4;
        float lx = RemasterUniforms.ShadowLight[o], ly = RemasterUniforms.ShadowLight[o + 1];
        float lz = RemasterUniforms.ShadowLight[o + 2], r = RemasterUniforms.ShadowLight[o + 3];
        ulong h = 0xCBF29CE484222325ul;
        int n = 0;
        for (int i = 0; i < _casterN; i += 3)
        {
            ref readonly var a = ref _casters[i];
            ref readonly var b = ref _casters[i + 1];
            ref readonly var c = ref _casters[i + 2];
            float cx = (a.X + b.X + c.X) / 3f, cy = (a.Y + b.Y + c.Y) / 3f, cz = (a.Z + b.Z + c.Z) / 3f;
            float e = MathF.Max(Dist2(a, cx, cy, cz), MathF.Max(Dist2(b, cx, cy, cz), Dist2(c, cx, cy, cz)));
            float dx = cx - lx, dy = cy - ly, dz = cz - lz;
            float reach = r + MathF.Sqrt(e);
            if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
            n++;
            h = Mix(Mix(Mix(h, a), b), c);
        }
        _casterReach[s] = n;
        return n == 0 ? 0 : h | 1;

        static float Dist2(in RetainedScene.Vertex v, float x, float y, float z)
            => (v.X - x) * (v.X - x) + (v.Y - y) * (v.Y - y) + (v.Z - z) * (v.Z - z);
        static ulong Mix(ulong h, in RetainedScene.Vertex v)
        {
            h = (h ^ BitConverter.SingleToUInt32Bits(v.X)) * 0x100000001B3ul;
            h = (h ^ BitConverter.SingleToUInt32Bits(v.Y)) * 0x100000001B3ul;
            return (h ^ BitConverter.SingleToUInt32Bits(v.Z)) * 0x100000001B3ul;
        }
    }

    /// <summary>In the light upload: which light samples which slot, only for slots
    /// drawn; the rotation; the cubemaps on their units.</summary>
    void SendShadows(int lightN)
    {
        if (_uLightShadow < 0) return;
        int mask = Math.Max(_shadowReadyMask, 0);
        if (_shadowSentGen != RemasterUniforms.Generation || _shadowReadySent != mask)
        {
            for (int i = 0; i < RemasterUniforms.MaxLights; i++)
            {
                int s = i < lightN ? RemasterUniforms.LightShadow[i] : -1;
                _shadowSend[i] = s >= 0 && s < RemasterUniforms.MaxShadows && (mask & (1 << s)) != 0 ? s : -1;
            }
            _gl.Uniform1(_uLightShadow, (uint)_shadowSend.Length, _shadowSend);
            if (mask != 0)
            {
                // The transpose of the row-major world-to-view R, by GLSL's column-major read.
                _gl.UniformMatrix3(_uShadowToWorld, 1, false, RemasterUniforms.ToWorld);
                _gl.Uniform1(_uShadowSize, (float)Math.Clamp(RemasterUniforms.ShadowSize, 64, 4096));
                _gl.Uniform1(_uShadowOffset, RemasterUniforms.ShadowOffset);
                _gl.Uniform1(_uShadowBias, RemasterUniforms.ShadowBias);
                _gl.Uniform1(_uShadowSoft, RemasterUniforms.ShadowSoft);
            }
            _shadowSentGen = RemasterUniforms.Generation;
            _shadowReadySent = mask;
        }
        if (mask == 0) return;
        for (int s = 0; s < RemasterUniforms.MaxShadows; s++)
            if ((mask & (1 << s)) != 0)
            {
                _gl.ActiveTexture(TextureUnit.Texture0 + ShadowUnit + s);
                _gl.BindTexture(TextureTarget.TextureCubeMap, _shadowBind[s]);
            }
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    unsafe void EnsureShadowTex(uint[] tex, int[] sizes, int s, int n)
    {
        if (_shadowFbo == 0)
        {
            _shadowFbo = _gl.GenFramebuffer();
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowFbo);
            _gl.DrawBuffer(DrawBufferMode.None);
            _gl.ReadBuffer(ReadBufferMode.None);
            _gl.Enable(EnableCap.TextureCubeMapSeamless);
        }
        if (tex[s] != 0 && sizes[s] == n) return;
        if (tex[s] == 0) tex[s] = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.TextureCubeMap, tex[s]);
        for (int i = 0; i < 6; i++)
            _gl.TexImage2D(TextureTarget.TextureCubeMapPositiveX + i, 0, InternalFormat.DepthComponent24, (uint)n, (uint)n, 0,
                PixelFormat.DepthComponent, PixelType.Float, null);
        // Linear with a compare is the hardware's own 2x2 filter of four compares.
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureCompareMode, (int)GLEnum.CompareRefToTexture);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureCompareFunc, (int)GLEnum.Lequal);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapR, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMaxLevel, 0);
        _gl.BindTexture(TextureTarget.TextureCubeMap, 0);
        sizes[s] = n;
    }

    /// <summary>Slot <paramref name="s"/>'s six faces from the light, the map's opaque
    /// triangles in the light's reach. Leaves the program, the framebuffer and the
    /// depth state for the flush to set, as it does for every batch.</summary>
    void RenderShadow(int s, int n)
    {
        EnsureShadowTex(_shadowTex, _shadowTexSize, s, n);
        UploadStatic();
        int o = s * 4;
        float lx = RemasterUniforms.ShadowLight[o], ly = RemasterUniforms.ShadowLight[o + 1];
        float lz = RemasterUniforms.ShadowLight[o + 2], r = RemasterUniforms.ShadowLight[o + 3];

        BeginShadowDraw();
        CullChunksSphere(lx, ly, lz, r);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowFbo);
        _gl.Viewport(0, 0, (uint)n, (uint)n);
        _gl.ClearDepth(1.0);
        for (int i = 0; i < 6; i++)
        {
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.DepthAttachment,
                TextureTarget.TextureCubeMapPositiveX + i, _shadowTex[s], 0);
            _gl.Clear(ClearBufferMask.DepthBufferBit);
            SetWorldView(CubeFaces[i], lx, ly, lz, 0f, 0f, 0f, n * 0.5f, n * 0.5f, n * 0.5f, n, n, 1f, false, 1);
            RemasterUniforms.ShadowTriangles += DrawRange(0, null) / 3;
        }
        _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.DepthAttachment,
            TextureTarget.TextureCubeMapPositiveX, 0, 0);
        _gl.BindVertexArray(0);
        RemasterUniforms.ShadowRenders++;
    }

    /// <summary>Slot <paramref name="s"/>'s second cubemap: the map's faces copied,
    /// then the frame's casters drawn over them. The same state left as
    /// <see cref="RenderShadow"/> leaves.</summary>
    unsafe void RenderShadowModels(int s, int n)
    {
        EnsureShadowTex(_shadowComb, _shadowCombSize, s, n);
        if (_casterVao == 0)
        {
            _uwOpaqueDepth = _gl.GetUniformLocation(_progWorld, "uOpaqueDepth");
            _casterVbo = _gl.GenBuffer();
            _casterVao = MakeWorldVao(_casterVbo);
            _shadowReadFbo = _gl.GenFramebuffer();
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowReadFbo);
            _gl.DrawBuffer(DrawBufferMode.None);
            _gl.ReadBuffer(ReadBufferMode.None);
        }
        if (_casterUploaded != _casterSerial)
        {
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _casterVbo);
            if (_casterN > _casterCap)
            {
                _casterCap = Math.Max(_casterN, _casterCap * 2);
                _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_casterCap * sizeof(RetainedScene.Vertex)), null,
                               BufferUsageARB.StreamDraw);
            }
            if (_casterN > 0)
                _gl.BufferSubData<RetainedScene.Vertex>(BufferTargetARB.ArrayBuffer, 0, _casters.AsSpan(0, _casterN));
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
            _casterUploaded = _casterSerial;
        }
        int o = s * 4;
        float lx = RemasterUniforms.ShadowLight[o], ly = RemasterUniforms.ShadowLight[o + 1];
        float lz = RemasterUniforms.ShadowLight[o + 2];

        BeginShadowDraw();
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _shadowReadFbo);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, _shadowFbo);
        _gl.Viewport(0, 0, (uint)n, (uint)n);
        _gl.BindVertexArray(_casterVao);
        for (int i = 0; i < 6; i++)
        {
            _gl.FramebufferTexture2D(FramebufferTarget.ReadFramebuffer, FramebufferAttachment.DepthAttachment,
                TextureTarget.TextureCubeMapPositiveX + i, _shadowTex[s], 0);
            _gl.FramebufferTexture2D(FramebufferTarget.DrawFramebuffer, FramebufferAttachment.DepthAttachment,
                TextureTarget.TextureCubeMapPositiveX + i, _shadowComb[s], 0);
            _gl.BlitFramebuffer(0, 0, n, n, 0, 0, n, n, ClearBufferMask.DepthBufferBit, BlitFramebufferFilter.Nearest);
            SetWorldView(CubeFaces[i], lx, ly, lz, 0f, 0f, 0f, n * 0.5f, n * 0.5f, n * 0.5f, n, n, 1f, false, 1);
            if (_casterOpaqueN > 0) _gl.DrawArrays(PrimitiveType.Triangles, 0, (uint)_casterOpaqueN);
            if (_casterN > _casterOpaqueN)
            {
                // The GPU blends only a texel with the semi-transparency bit; the rest is opaque.
                if (_uwOpaqueDepth >= 0) _gl.Uniform1(_uwOpaqueDepth, 1);
                _gl.DrawArrays(PrimitiveType.Triangles, _casterOpaqueN, (uint)(_casterN - _casterOpaqueN));
                if (_uwOpaqueDepth >= 0) _gl.Uniform1(_uwOpaqueDepth, 0);
            }
        }
        RemasterUniforms.ShadowModelTriangles += _casterN / 3 * 6;
        _gl.FramebufferTexture2D(FramebufferTarget.ReadFramebuffer, FramebufferAttachment.DepthAttachment,
            TextureTarget.TextureCubeMapPositiveX, 0, 0);
        _gl.FramebufferTexture2D(FramebufferTarget.DrawFramebuffer, FramebufferAttachment.DepthAttachment,
            TextureTarget.TextureCubeMapPositiveX, 0, 0);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowFbo);
        _gl.BindVertexArray(0);
        RemasterUniforms.ShadowModelRenders++;
    }

    /// <summary>The world program set for depth from a light: no blend, no clip, no
    /// mask, one tap, and the depth test and write on.</summary>
    void BeginShadowDraw()
    {
        _gl.UseProgram(_progWorld);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.ClipDistance0);
        _gl.BlendFunc(BlendingFactor.One, BlendingFactor.Zero);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);
        _gl.ActiveTexture(TextureUnit.Texture0);
        if (_uwMirror >= 0) _gl.Uniform1(_uwMirror, 0);
        if (_uwMaskOn >= 0) _gl.Uniform1(_uwMaskOn, 0);
        if (_uwFluidN >= 0) _gl.Uniform1(_uwFluidN, 0f);
        if (_uwAniso >= 0) _gl.Uniform1(_uwAniso, 1f);
        _gl.Enable(EnableCap.DepthTest);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.DepthMask(true);
    }

    /// <summary>The static chunks whose box the light's sphere reaches.</summary>
    void CullChunksSphere(float x, float y, float z, float r)
    {
        for (int c = 0; c < RetainedScene.Chunks; c++)
        {
            _chunkVis[c] = false;
            if (!RetainedScene.ChunkUsed[c]) continue;
            float dx = Math.Clamp(x, RetainedScene.ChunkMin[c * 3], RetainedScene.ChunkMax[c * 3]) - x;
            float dy = Math.Clamp(y, RetainedScene.ChunkMin[c * 3 + 1], RetainedScene.ChunkMax[c * 3 + 1]) - y;
            float dz = Math.Clamp(z, RetainedScene.ChunkMin[c * 3 + 2], RetainedScene.ChunkMax[c * 3 + 2]) - z;
            _chunkVis[c] = dx * dx + dy * dy + dz * dz <= r * r;
        }
    }
}
