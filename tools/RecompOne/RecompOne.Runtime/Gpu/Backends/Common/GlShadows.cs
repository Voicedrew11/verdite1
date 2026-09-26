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
        for (int s = 0; s < RemasterUniforms.MaxShadows; s++)
        {
            int o = s * 4, k = s * 6;
            float r = RemasterUniforms.ShadowLight[o + 3];
            if (r <= 0f || !map) { _shadowReady[s] = false; continue; }
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
            }
            mask |= 1 << s;
        }
        _shadowReadyMask = mask;
        RemasterUniforms.ShadowsReady = System.Numerics.BitOperations.PopCount((uint)mask);
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
                _gl.BindTexture(TextureTarget.TextureCubeMap, _shadowTex[s]);
            }
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    unsafe void EnsureShadowTex(int s, int n)
    {
        if (_shadowFbo == 0)
        {
            _shadowFbo = _gl.GenFramebuffer();
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowFbo);
            _gl.DrawBuffer(DrawBufferMode.None);
            _gl.ReadBuffer(ReadBufferMode.None);
            _gl.Enable(EnableCap.TextureCubeMapSeamless);
        }
        if (_shadowTex[s] != 0 && _shadowTexSize[s] == n) return;
        if (_shadowTex[s] == 0) _shadowTex[s] = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.TextureCubeMap, _shadowTex[s]);
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
        _shadowTexSize[s] = n;
    }

    /// <summary>Slot <paramref name="s"/>'s six faces from the light, the map's opaque
    /// triangles in the light's reach. Leaves the program, the framebuffer and the
    /// depth state for the flush to set, as it does for every batch.</summary>
    void RenderShadow(int s, int n)
    {
        EnsureShadowTex(s, n);
        UploadStatic();
        int o = s * 4;
        float lx = RemasterUniforms.ShadowLight[o], ly = RemasterUniforms.ShadowLight[o + 1];
        float lz = RemasterUniforms.ShadowLight[o + 2], r = RemasterUniforms.ShadowLight[o + 3];

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

        CullChunksSphere(lx, ly, lz, r);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _shadowFbo);
        _gl.Viewport(0, 0, (uint)n, (uint)n);
        _gl.Enable(EnableCap.DepthTest);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.DepthMask(true);
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
