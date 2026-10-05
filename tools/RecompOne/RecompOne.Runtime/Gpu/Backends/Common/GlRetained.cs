using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

/// <summary>
/// 0072. The retained scene (<see cref="RetainedScene"/>), drawn at present for the
/// target being presented: its planes into the target's planar texture, each
/// mirrored straight into the picture's pixels and kept only where the frame's own
/// surface lies on that plane, and a cubemap from the frame's camera. The reflection
/// pass reads both. See "The retained scene" in docs/RENDERING.md.
/// </summary>
public sealed partial class GlCore
{
    uint _progWorld, _worldVao, _worldVbo, _worldDynVao, _worldDynVbo;
    int _worldGen = -1, _worldCap, _worldDynCap;
    int _uwR, _uwCam, _uwT, _uwH, _uwC, _uwFb, _uwNear, _uwCueH, _uwFogOn, _uwMirror, _uwPlaneY, _uwPlaneBias;
    int _uwScale, _uwAniso, _uwBlend, _uwBlendOpaque, _uwFluidN;
    int _uwAtmosOn, _uwAtmosColour, _uwAtmosShape, _uwAtmosSkip;
    int _uwMaskOn, _uwMaskPlane, _uwMaskTol, _uwMaskCentre, _uwMaskH, _uwMaskSize;
    readonly int[] _uwFluidRect = new int[8], _uwFluidOff = new int[8];

    uint _cubeTex, _cubeDepth, _cubeFbo;
    int _cubeMade;

    // What this present drew, for the reflection pass.
    RetainedScene.Frame? _retFrame;
    GlDisplayRt? _retPlanar;
    bool _retCube;
    int _retPlaneN;
    readonly float[] _retPlanes = new float[RetainedScene.MaxPlanes * 4];

    int _uSsrRetN, _uSsrRetPlane, _uSsrCubeOn, _uSsrToWorld, _uSsrViewT, _uSsrCubeSteps, _uSsrCubeSize;

    const int CubeUnit = 9, CubeDepthUnit = 10, MaskUnit = 11, HalvesUnit = 16;

    uint _halvesTex;
    int _uwHalfGate;

    // Authored lights, glows and mipmaps in the world program (0072 amended).
    int _uwWorldLit, _uwLightN, _uwLightPos, _uwLightCol, _uwLightDir, _uwLightCentre, _uwLightH, _uwEmitOn, _uwMipOn;
    int _uwLightShadow, _uwShadowToWorld, _uwShadowSize, _uwShadowOffset, _uwShadowBias, _uwShadowSoft;
    uint _worldMipVbo, _worldDynMipVbo;
    readonly float[] _wLightPos = new float[RemasterUniforms.MaxLights * 4], _wLightDir = new float[RemasterUniforms.MaxLights * 4];
    int _wLightN;

    /// <summary>How far above a plane geometry must stand to be mirrored in it:
    /// the water itself, and anything lying in it, is not its own reflection.</summary>
    public static float PlaneBias = 8f;

    unsafe void InitRetained(uint progSsr)
    {
        if (_legacy || progSsr == 0) return;
        _progWorld = GlShaders.Build(_gl, GlShaders.WorldVs, GlShaders.PrimFs, "world");
        if (_progWorld == 0) return;

        int L(string n) => _gl.GetUniformLocation(_progWorld, n);
        _uwR = L("uR"); _uwCam = L("uCam"); _uwT = L("uT"); _uwH = L("uH"); _uwC = L("uC"); _uwFb = L("uFb");
        _uwNear = L("uNear"); _uwCueH = L("uCueH"); _uwFogOn = L("uFogOn"); _uwMirror = L("uMirror");
        _uwPlaneY = L("uPlaneY"); _uwPlaneBias = L("uPlaneBias");
        _uwScale = L("uScale"); _uwAniso = L("uAniso"); _uwBlend = L("uBlend"); _uwBlendOpaque = L("uBlendOpaque");
        _uwFluidN = L("uFluidN");
        for (int i = 0; i < 8; i++) { _uwFluidRect[i] = L($"uFluidRect[{i}]"); _uwFluidOff[i] = L($"uFluidOff[{i}]"); }
        _uwMaskOn = L("uMaskOn"); _uwMaskPlane = L("uMaskPlane"); _uwMaskTol = L("uMaskTol");
        _uwMaskCentre = L("uMaskCentre"); _uwMaskH = L("uMaskH"); _uwMaskSize = L("uMaskSize");
        _uwHalfGate = L("uHalfGate");
        _uwAtmosOn = L("uAtmosOn"); _uwAtmosColour = L("uAtmosColour"); _uwAtmosShape = L("uAtmosShape");
        _uwAtmosSkip = L("uAtmosSkip");
        _uwWorldLit = L("uWorldLit"); _uwLightN = L("uLightN"); _uwLightPos = L("uLightPos"); _uwLightCol = L("uLightCol");
        _uwLightDir = L("uLightDir"); _uwLightCentre = L("uLightCentre"); _uwLightH = L("uLightH"); _uwEmitOn = L("uEmitOn");
        _uwMipOn = L("uMipOn"); _uwLightShadow = L("uLightShadow"); _uwShadowToWorld = L("uShadowToWorld");
        _uwShadowSize = L("uShadowSize"); _uwShadowOffset = L("uShadowOffset"); _uwShadowBias = L("uShadowBias");
        _uwShadowSoft = L("uShadowSoft");

        _gl.UseProgram(_progWorld);
        void Unit(string n, int u) { int l = L(n); if (l >= 0) _gl.Uniform1(l, u); }
        Unit("uVram", 0); Unit("uDest", 1); Unit("uExtTex", 2); Unit("uRepTex", 3); Unit("uRepClut", 4);
        Unit("uMip", 5); Unit("uMatTable", MatUnit); Unit("uMaskSurface", MaskUnit); Unit("uHalves", HalvesUnit);
        Unit("uRecords", RecordsUnit); Unit("uNbHalves", NbHalvesUnit);
        _uwNeighbour = L("uNeighbour"); _uwNbTile = L("uNbTile");
        _uwFade = L("uFade"); _uwFadeZ = L("uFadeZ");
        void I(string n, int v) { int l = L(n); if (l >= 0) _gl.Uniform1(l, v); }
        void F(string n, float v) { int l = L(n); if (l >= 0) _gl.Uniform1(l, v); }
        int tw = L("uTexWindow");
        if (tw >= 0) _gl.Uniform4(tw, 255, 255, 0, 0);
        F("uSetMask", 0f); I("uCheckMask", 0); I("uOpaqueDepth", 0); F("uDepthBias", 0f); F("uDepthSlope", 0f);
        I("uClipOn", 0); I("uLightN", 0); I("uEmitOn", 0); F("uMipOn", 0f); F("uTrueColor", 1f); F("uFluidN", 0f);
        I("uMaskOn", 0); I("uHalfGate", 0); I("uAtmosOn", 0); I("uAtmosSkip", 0); I("uWorldLit", 0);
        I("uWorldSnap", 0); I("uWorldPerPixel", 1); I("uWorldDither", 0); I("uNeighbour", 0);
        InitShadowUniforms(_progWorld, false);
        if (_uwBlendOpaque >= 0) _gl.Uniform4(_uwBlendOpaque, 1f, 1f, 1f, 0f);
        int pb = L("uPosBias");
        if (pb >= 0) _gl.Uniform2(pb, 0f, 0f);

        _halvesTex = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, _halvesTex);
        _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.R8ui, RetainedScene.HalvesW, RetainedScene.HalvesH, 0,
            PixelFormat.RedInteger, PixelType.UnsignedByte, null);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.BindTexture(TextureTarget.Texture2D, 0);

        _uwMipIndirect = L("uMipIndirect");
        Unit("uMipTable", MipTableUnit);
        _mipTableBuf = _gl.GenBuffer();
        _mipTableTex = _gl.GenTexture();

        _worldVbo = _gl.GenBuffer();
        _worldDynVbo = _gl.GenBuffer();
        _worldMipVbo = _gl.GenBuffer();
        _worldDynMipVbo = _gl.GenBuffer();
        _worldVao = MakeWorldVao(_worldVbo, _worldMipVbo);
        _worldDynVao = MakeWorldVao(_worldDynVbo, _worldDynMipVbo);

        _gl.UseProgram(progSsr);
        int SL(string n) => _gl.GetUniformLocation(progSsr, n);
        _uSsrRetN = SL("uRetPlanarN"); _uSsrRetPlane = SL("uRetPlane"); _uSsrCubeOn = SL("uCubeOn");
        _uSsrToWorld = SL("uToWorld"); _uSsrViewT = SL("uViewT"); _uSsrCubeSteps = SL("uCubeSteps");
        _uSsrCubeSize = SL("uCubeSize");
        int uc = SL("uCube"), ud = SL("uCubeDepth");
        if (uc >= 0) _gl.Uniform1(uc, CubeUnit);
        if (ud >= 0) _gl.Uniform1(ud, CubeDepthUnit);
        if (_uSsrRetN >= 0) _gl.Uniform1(_uSsrRetN, 0);
        if (_uSsrCubeOn >= 0) _gl.Uniform1(_uSsrCubeOn, 0);

        RetainedScene.Supported = _uSsrRetN >= 0 && _uSsrCubeOn >= 0 && _uwMaskOn >= 0;
    }

    unsafe uint MakeWorldVao(uint vbo, uint mipVbo)
    {
        uint vao = _gl.GenVertexArray();
        _gl.BindVertexArray(vao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, vbo);
        uint st = (uint)sizeof(RetainedScene.Vertex);
        _gl.EnableVertexAttribArray(0); _gl.VertexAttribPointer(0, 3, VertexAttribPointerType.Float, false, st, (void*)0);
        _gl.EnableVertexAttribArray(1); _gl.VertexAttribPointer(1, 3, VertexAttribPointerType.Float, false, st, (void*)12);
        _gl.EnableVertexAttribArray(2); _gl.VertexAttribPointer(2, 1, VertexAttribPointerType.Float, false, st, (void*)24);
        _gl.EnableVertexAttribArray(3); _gl.VertexAttribPointer(3, 1, VertexAttribPointerType.Float, false, st, (void*)28);
        _gl.EnableVertexAttribArray(4); _gl.VertexAttribPointer(4, 2, VertexAttribPointerType.Float, false, st, (void*)32);
        _gl.EnableVertexAttribArray(5); _gl.VertexAttribPointer(5, 3, VertexAttribPointerType.Float, false, st, (void*)40);
        _gl.EnableVertexAttribArray(6); _gl.VertexAttribIPointer(6, 1, VertexAttribIType.UnsignedInt, st, (void*)52);
        _gl.EnableVertexAttribArray(7); _gl.VertexAttribIPointer(7, 1, VertexAttribIType.UnsignedInt, st, (void*)56);
        _gl.EnableVertexAttribArray(8); _gl.VertexAttribIPointer(8, 1, VertexAttribIType.UnsignedInt, st, (void*)60);
        _gl.EnableVertexAttribArray(10); _gl.VertexAttribIPointer(10, 1, VertexAttribIType.UnsignedInt, st, (void*)64);
        // The mip entries are the backend's, not the port's: a buffer of their own. A
        // shadow's casters have none, and read the attribute's constant 0.
        if (mipVbo != 0)
        {
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, mipVbo);
            _gl.EnableVertexAttribArray(9); _gl.VertexAttribIPointer(9, 1, VertexAttribIType.UnsignedInt, 4, (void*)0);
        }
        else _gl.VertexAttribI4(9, 0u, 0u, 0u, 0u);
        _gl.BindVertexArray(0);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        return vao;
    }

    /// <summary>The planar texture and the cubemap for the target about to be
    /// presented, from the frame it holds. Nothing is drawn when the frame has left
    /// the ring (a menu's frozen picture, many presents on).</summary>
    unsafe void DrawRetained(GlDisplayRt src)
    {
        _retFrame = null;
        _retPlanar = null;
        _retCube = false;
        _retPlaneN = 0;
        if (!RetainedScene.Enabled || _progWorld == 0) return;
        var f = RetainedScene.Find(src.RetainedSerial);
        if (f == null) { RetainedScene.Missed++; return; }
        RetainedScene.Found++;
        _retFrame = f;
        RetainedScene.ChunksDrawn += _chunksDrawn; RetainedScene.ChunksTested += _chunksTested;
        _chunksDrawn = _chunksTested = 0;

        UploadWorld(f);
        bool mips = UpdateWorldMips(f, RetainedScene.HalfGate ? f.Halves : null);
        CloseWorldMain();
        _gl.UseProgram(_progWorld);
        if (_uwMipOn >= 0) _gl.Uniform1(_uwMipOn, mips ? 1f : 0f);
        if (mips)
        {
            _gl.ActiveTexture(TextureUnit.Texture5);
            _gl.BindTexture(TextureTarget.Texture2D, _mip!.Texture);
        }
        BeginWorldLights();
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);
        if (_uwAniso >= 0) _gl.Uniform1(_uwAniso, (float)GteDepth.Anisotropy);
        SendWorldFluid();
        // Only the map halves the frame's walk drew: its visibility flood from the
        // eye's level, which a mirror (pitch negated, same cone) shares.
        _gl.ActiveTexture(TextureUnit.Texture0 + HalvesUnit);
        _gl.BindTexture(TextureTarget.Texture2D, _halvesTex);
        _gl.TexSubImage2D<byte>(TextureTarget.Texture2D, 0, 0, 0, RetainedScene.HalvesW, RetainedScene.HalvesH,
            PixelFormat.RedInteger, PixelType.UnsignedByte, f.Halves);
        _gl.ActiveTexture(TextureUnit.Texture0);
        if (_uwHalfGate >= 0) _gl.Uniform1(_uwHalfGate, RetainedScene.HalfGate ? 1 : 0);
        SendWorldAtmos();

        if (RetainedScene.Planar && f.PlaneCount > 0 && src.Surface != 0)
        {
            uint q = BeginRetainedTimer();
            DrawRetainedPlanar(src, f);
            EndRetainedTimer(q, ref _planarQuery, true);
        }
        if (RetainedScene.Cube)
        {
            uint q = BeginRetainedTimer();
            DrawRetainedCube(f, src);
            EndRetainedTimer(q, ref _cubeQuery, false);
        }

        if (_uwHalfGate >= 0) _gl.Uniform1(_uwHalfGate, 0);
        EndWorldLights();
        // Dual-source factors left set are an error for any draw into more than one
        // buffer, blending on or not -- the reflection pass's probe draws into two.
        _gl.BlendFunc(BlendingFactor.One, BlendingFactor.Zero);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.DepthTest);
        _gl.DepthMask(false);
        _gl.BindVertexArray(0);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    /// <summary>0074. The area's fog colour and curve, as the frame's batches take them;
    /// only the fogged draws (the planes) reach it, the cubemap being unfogged.</summary>
    void SendWorldAtmos()
    {
        if (_uwAtmosOn < 0) return;
        bool on = RemasterUniforms.FogActive;
        _gl.Uniform1(_uwAtmosOn, on ? 1 : 0);
        if (!on) return;
        var fc = RemasterUniforms.FogColour;
        _gl.Uniform3(_uwAtmosColour, fc[0], fc[1], fc[2]);
        _gl.Uniform2(_uwAtmosShape, RemasterUniforms.FogPower, RemasterUniforms.FogMax);
    }

    // 0085. The light records the static map is lit from, 13 RGBA32I texels a record.
    const int RecordsUnit = 21;
    uint _recordsTex;
    int _recordsGen = -1;

    unsafe void UploadRecords()
    {
        if (_recordsGen == RetainedScene.RecordGeneration) return;
        _recordsGen = RetainedScene.RecordGeneration;
        _gl.ActiveTexture(TextureUnit.Texture0 + RecordsUnit);
        if (_recordsTex == 0)
        {
            _recordsTex = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, _recordsTex);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba32i, RetainedScene.RecordInts / 4,
                RetainedScene.RecordCount, 0, PixelFormat.RgbaInteger, PixelType.Int, null);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        }
        else _gl.BindTexture(TextureTarget.Texture2D, _recordsTex);
        fixed (int* p = RetainedScene.Records)
            _gl.TexSubImage2D(TextureTarget.Texture2D, 0, 0, 0, RetainedScene.RecordInts / 4, RetainedScene.RecordCount,
                PixelFormat.RgbaInteger, PixelType.Int, p);
        // Nothing else takes this unit, so the records stay bound for every world draw.
        _gl.ActiveTexture(TextureUnit.Texture0);
        RetainedScene.RecordUploads++;
    }

    // 0088. Each map half's record for NeighbourBlend, one byte a half.
    const int NbHalvesUnit = 22;
    uint _nbHalvesTex;
    int _nbHalvesGen = -1, _uwNeighbour = -1, _uwNbTile = -1;
    // 0089. DistanceFade's uniforms, in the world program and its normal program.
    int _uwFade = -1, _uwFadeZ = -1, _uwnFade = -1, _uwnFadeZ = -1;

    /// <summary>0089. The frame's DistanceFade, or none.</summary>
    void SendFade(int fade, int depth, RetainedScene.Frame? f)
    {
        if (fade >= 0)
        {
            if (f == null) _gl.Uniform4(fade, 0f, 0f, 0f, 0f);
            else _gl.Uniform4(fade, (float)f.View.CamX, (float)f.View.CamZ, f.FadeEdge, f.FadeBand);
        }
        if (depth >= 0) _gl.Uniform1(depth, f?.FadeDepth ?? 0f);
    }

    unsafe void UploadNbHalves()
    {
        if (_nbHalvesGen == NeighbourBlend.Generation) return;
        _nbHalvesGen = NeighbourBlend.Generation;
        _gl.ActiveTexture(TextureUnit.Texture0 + NbHalvesUnit);
        if (_nbHalvesTex == 0)
        {
            _nbHalvesTex = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, _nbHalvesTex);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.R8ui, RetainedScene.HalvesW, RetainedScene.HalvesH, 0,
                PixelFormat.RedInteger, PixelType.UnsignedByte, null);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        }
        else _gl.BindTexture(TextureTarget.Texture2D, _nbHalvesTex);
        fixed (byte* p = NeighbourBlend.Halves)
            _gl.TexSubImage2D(TextureTarget.Texture2D, 0, 0, 0, RetainedScene.HalvesW, RetainedScene.HalvesH,
                PixelFormat.RedInteger, PixelType.UnsignedByte, p);
        // Nothing else takes this unit either.
        _gl.ActiveTexture(TextureUnit.Texture0);
        NeighbourBlend.Uploads++;
    }

    /// <summary>The static map, when it was rebuilt since the last upload.</summary>
    void UploadStatic()
    {
        UploadRecords();
        UploadNbHalves();
        if (_worldGen == RetainedScene.StaticGeneration) return;
        var s = RetainedScene.Static;
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _worldVbo);
        _gl.BufferData<RetainedScene.Vertex>(BufferTargetARB.ArrayBuffer, s, BufferUsageARB.StaticDraw);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        _worldGen = RetainedScene.StaticGeneration;
    }

    unsafe void UploadWorld(RetainedScene.Frame f)
    {
        UploadStatic();
        var d = f.SortedDynamic();
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _worldDynVbo);
        if (d.Length > _worldDynCap)
        {
            _worldDynCap = Math.Max(d.Length, _worldDynCap * 2);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_worldDynCap * sizeof(RetainedScene.Vertex)), null,
                           BufferUsageARB.StreamDraw);
        }
        if (d.Length > 0) _gl.BufferSubData<RetainedScene.Vertex>(BufferTargetARB.ArrayBuffer, 0, d);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
    }

    void SendWorldFluid()
    {
        if (_uwFluidN >= 0) _gl.Uniform1(_uwFluidN, (float)GteDepth.FluidN);
        for (int i = 0; i < GteDepth.FluidN && i < 8; i++)
        {
            ref var slot = ref GteDepth.Fluid[i];
            if (_uwFluidRect[i] >= 0) _gl.Uniform4(_uwFluidRect[i], slot.X, slot.Y, slot.W, slot.H);
            if (_uwFluidOff[i] >= 0) _gl.Uniform1(_uwFluidOff[i], slot.Off);
        }
    }

    /// <summary>A camera for the world program: rotation, world position,
    /// translation, projection and target size.</summary>
    void SetWorldView(ReadOnlySpan<float> r, double camX, double camY, double camZ, float tx, float ty, float tz,
                      float h, float cx, float cy, float fbW, float fbH, float cueH, bool fog, int scale)
    {
        // GLSL reads a mat3 column-major; ask for the transpose of the row-major R.
        if (_uwR >= 0) _gl.UniformMatrix3(_uwR, 1, true, r);
        if (_uwCam >= 0) _gl.Uniform3(_uwCam, (float)camX, (float)camY, (float)camZ);
        if (_uwT >= 0) _gl.Uniform3(_uwT, tx, ty, tz);
        if (_uwH >= 0) _gl.Uniform1(_uwH, h);
        if (_uwC >= 0) _gl.Uniform2(_uwC, cx, cy);
        if (_uwFb >= 0) _gl.Uniform2(_uwFb, fbW, fbH);
        if (_uwNear >= 0) _gl.Uniform1(_uwNear, 16f);
        if (_uwCueH >= 0) _gl.Uniform1(_uwCueH, Math.Max(1f, cueH));
        if (_uwFogOn >= 0) _gl.Uniform1(_uwFogOn, fog ? 1 : 0);
        if (_uwScale >= 0) _gl.Uniform1(_uwScale, scale);
    }

    /// <summary>The static map and the frame's models: opaque with the depth test
    /// and write, then each blend mode tested without writing. Subtractive blending
    /// needs a second pass against a copy of the target and is left out; nothing
    /// this game reflects uses it.</summary>
    void DrawWorldRanges(RetainedScene.Frame f)
    {
        // A mirror and a cube face (whose rotations all have determinant -1) turn
        // the winding over, so the faces the game keeps are clockwise here.
        if (RetainedScene.CullBack)
        {
            _gl.Enable(EnableCap.CullFace);
            _gl.FrontFace(FrontFaceDirection.CW);
            _gl.CullFace(TriangleFace.Back);
        }
        _gl.Enable(EnableCap.DepthTest);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.DepthMask(true);
        _gl.Disable(EnableCap.Blend);
        RetainedScene.Triangles += DrawRange(0, f) / 3;

        _gl.DepthMask(false);
        _gl.Enable(EnableCap.Blend);
        _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
        _gl.BlendFuncSeparate(BlendingFactor.Src1Color, BlendingFactor.Src1Alpha, BlendingFactor.One, BlendingFactor.Zero);
        for (int mode = 0; mode < 4; mode++)
        {
            if (mode == 2) continue;
            float src = mode switch { 0 => 0.5f, 3 => 0.25f, _ => 1f }, dst = mode == 0 ? 0.5f : 1f;
            if (_uwBlend >= 0) _gl.Uniform4(_uwBlend, src, src, src, dst);
            if (_uwAtmosSkip >= 0) _gl.Uniform1(_uwAtmosSkip, mode == 0 ? 0 : 1);
            RetainedScene.Triangles += DrawRange(1 + mode, f) / 3;
        }
        if (_uwAtmosSkip >= 0) _gl.Uniform1(_uwAtmosSkip, 0);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.CullFace);
        _gl.FrontFace(FrontFaceDirection.Ccw);
    }

    /// <summary>The probe's count on the planar pass just drawn: pixels whose
    /// nearest opaque face is a front face, and pixels a back face would have taken
    /// with culling off (drawn against the culled depth, nearer than it or where
    /// nothing was). Waits on the queries; the probe only.</summary>
    void CountFacing(RetainedScene.Frame f)
    {
        if (!RetainedScene.CullBack) return;
        _gl.ColorMask(false, false, false, false);
        _gl.DepthMask(false);
        _gl.Enable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.Enable(EnableCap.CullFace);
        _gl.FrontFace(FrontFaceDirection.CW);
        long Count(TriangleFace cull, DepthFunction func)
        {
            _gl.CullFace(cull);
            _gl.DepthFunc(func);
            uint q = _gl.GenQuery();
            _gl.BeginQuery(QueryTarget.SamplesPassed, q);
            DrawRange(0, f);
            _gl.EndQuery(QueryTarget.SamplesPassed);
            _gl.GetQueryObject(q, QueryObjectParameterName.Result, out long n);
            _gl.DeleteQuery(q);
            return n;
        }
        RetainedScene.FrontPixels += Count(TriangleFace.Back, DepthFunction.Equal);
        RetainedScene.BackPixels += Count(TriangleFace.Front, DepthFunction.Less);
        if (RetainedScene.HalfGate && _uwHalfGate >= 0)
        {
            _gl.Uniform1(_uwHalfGate, 2);
            RetainedScene.UndrawnPixels += Count(TriangleFace.Back, DepthFunction.Less);
            _gl.Uniform1(_uwHalfGate, 1);
        }
        _gl.Disable(EnableCap.CullFace);
        _gl.FrontFace(FrontFaceDirection.Ccw);
        _gl.DepthFunc(DepthFunction.Lequal);
        _gl.ColorMask(true, true, true, true);
    }

    /// <summary>Range <paramref name="r"/> of the visible static chunks, and of the
    /// frame's models when there is a frame; the static vertices drawn.</summary>
    int DrawRange(int r, RetainedScene.Frame? f)
    {
        int drawn = 0;
        if (RetainedScene.StaticCount[r] > 0)
        {
            if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 1);
            drawn = DrawStaticChunks(r);
        }
        if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 0);
        int dn = f?.DynCount[r] ?? 0;
        if (dn > 0)
        {
            _gl.BindVertexArray(_worldDynVao);
            _gl.DrawArrays(PrimitiveType.Triangles, f!.DynStart[r], (uint)dn);
            RetainedScene.Triangles += dn / 3;
        }
        return drawn;
    }

    /// <summary>Range <paramref name="r"/> of the visible static chunks through
    /// whatever program is bound; the vertices drawn.</summary>
    int DrawStaticChunks(int r)
    {
        int drawn = 0;
        _gl.BindVertexArray(_worldVao);
        // Runs of neighbouring visible chunks are one draw.
        int first = -1, count = 0;
        for (int c = 0; c <= RetainedScene.Chunks; c++)
        {
            int k = r * RetainedScene.Chunks + c;
            bool take = c < RetainedScene.Chunks && _chunkVis[c] && RetainedScene.ChunkCount[k] > 0;
            if (take && first >= 0 && RetainedScene.ChunkStart[k] == first + count) { count += RetainedScene.ChunkCount[k]; continue; }
            if (first >= 0 && count > 0)
            {
                _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)count);
                drawn += count;
            }
            first = take ? RetainedScene.ChunkStart[k] : -1;
            count = take ? RetainedScene.ChunkCount[k] : 0;
        }
        return drawn;
    }

    // ---- which chunks a view can see --------------------------------------------

    readonly bool[] _chunkVis = new bool[RetainedScene.Chunks];
    long _chunksDrawn, _chunksTested;

    /// <summary>The static chunks this view can see: in front of the near plane,
    /// inside the picture, not wholly past where the fog is black, and for a mirror
    /// above the plane. A chunk is dropped only when all eight corners of its box
    /// fail one test. The fog is the game's depth cue, on view depth: a mirrored
    /// chunk is black once its nearest corner is deeper than the depth its own
    /// latest fog is black at (<paramref name="cueH"/> the game's H). A cube face
    /// is drawn unfogged and its hit fogged for the whole path at the picture's
    /// pixel, so there the bound is the frame's black depth over the cosine of the
    /// picture's widest ray (<paramref name="cubeReach"/>, infinite for none).</summary>
    void CullChunks(ReadOnlySpan<float> r, double camX, double camY, double camZ, float tx, float ty, float tz,
                    float h, float cx, float cy, float fbW, float fbH, bool mirror, float planeY, float cueH,
                    float cubeReach)
    {
        float oldReach = Math.Max(ScreenReflections.March(), 4096f) + 2048f;
        for (int c = 0; c < RetainedScene.Chunks; c++)
        {
            _chunkVis[c] = false;
            if (!RetainedScene.ChunkUsed[c]) continue;
            _chunksTested++;
            float x0 = RetainedScene.ChunkMin[c * 3], y0 = RetainedScene.ChunkMin[c * 3 + 1], z0 = RetainedScene.ChunkMin[c * 3 + 2];
            float x1 = RetainedScene.ChunkMax[c * 3], y1 = RetainedScene.ChunkMax[c * 3 + 1], z1 = RetainedScene.ChunkMax[c * 3 + 2];
            if (mirror)
            {
                // Nothing in it stands above the plane (Y is down).
                if (y0 >= planeY - PlaneBias) continue;
                (y0, y1) = (2f * planeY - y1, 2f * planeY - y0);
            }
            double nx = Math.Clamp(camX, x0, x1) - camX, ny = Math.Clamp(camY, y0, y1) - camY, nz = Math.Clamp(camZ, z0, z1) - camZ;
            double dist = Math.Sqrt(nx * nx + ny * ny + nz * nz);
            if (!mirror && dist > cubeReach) continue;
            // local x = cx + h*x/z inside [0, fbW]: h*x + cx*z >= 0 and h*x + (cx - fbW)*z <= 0;
            // the same for y. Out when every corner fails one of the five.
            int near = 0, left = 0, right = 0, top = 0, bottom = 0;
            float minZ = float.MaxValue;
            for (int k = 0; k < 8; k++)
            {
                double dx = ((k & 1) != 0 ? x1 : x0) - camX, dy = ((k & 2) != 0 ? y1 : y0) - camY, dz = ((k & 4) != 0 ? z1 : z0) - camZ;
                float qx = (float)(r[0] * dx + r[1] * dy + r[2] * dz) + tx;
                float qy = (float)(r[3] * dx + r[4] * dy + r[5] * dz) + ty;
                float qz = (float)(r[6] * dx + r[7] * dy + r[8] * dz) + tz;
                minZ = Math.Min(minZ, qz);
                if (qz < 16f) near++;
                if (h * qx + cx * qz < 0f) left++;
                if (h * qx + (cx - fbW) * qz > 0f) right++;
                if (h * qy + cy * qz < 0f) top++;
                if (h * qy + (cy - fbH) * qz > 0f) bottom++;
            }
            if (near == 8 || left == 8 || right == 8 || top == 8 || bottom == 8) continue;
            if (mirror)
            {
                float q = RetainedScene.ChunkFogQ[c];
                if (q > 0f && minZ > cueH * 65536f / q) continue;
                if (RetainedScene.Probe && dist > oldReach)
                {
                    RetainedScene.OldCullVisible++;
                    float keep = RetainedScene.FogKeep(RetainedScene.ChunkFogOf[c], cueH, Math.Max(minZ, 16f));
                    RetainedScene.OldCullKeep = Math.Max(RetainedScene.OldCullKeep, keep);
                }
            }
            _chunkVis[c] = true;
            _chunksDrawn++;
        }
    }

    /// <summary>The target's planar texture, made at its size the first time.</summary>
    GlDisplayRt RetainedPlanarOf(GlDisplayRt rt)
    {
        var p = rt.Planar;
        if (p == null || p.W != rt.W || p.H != rt.H || p.Margin != rt.Margin || p.CreatedScale != GlVram.Scale)
        {
            p?.Destroy(_gl);
            p = new GlDisplayRt { X = rt.X, Y = rt.Y, W = rt.W, H = rt.H, Margin = rt.Margin, IsPlanar = true };
            p.Create(_gl);
            _gl.BindTexture(TextureTarget.Texture2D, p.Tex);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
            _gl.BindTexture(TextureTarget.Texture2D, 0);
            rt.Planar = p;
            rt.PlanarSerial = -1;
        }
        return p;
    }

    void DrawRetainedPlanar(GlDisplayRt src, RetainedScene.Frame f)
    {
        var p = RetainedPlanarOf(src);
        var v = f.View;
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, p.Fbo);
        _gl.Viewport(0, 0, (uint)p.TexW, (uint)p.TexH);
        _gl.ColorMask(true, true, true, true);
        _gl.DepthMask(true);
        _gl.ClearColor(0f, 0f, 0f, 0f);
        _gl.ClearDepth(1.0);
        _gl.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);

        Span<float> r = [v.R00, v.R01, v.R02, v.R10, v.R11, v.R12, v.R20, v.R21, v.R22];
        float cx = v.Cx + src.Margin;
        SetWorldView(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, v.H, cx, v.Cy, src.Wide1x, src.H, v.H, true,
                     p.CreatedScale);
        if (_uwMirror >= 0) _gl.Uniform1(_uwMirror, 1);
        if (_uwPlaneBias >= 0) _gl.Uniform1(_uwPlaneBias, PlaneBias);
        if (_uwMaskOn >= 0) _gl.Uniform1(_uwMaskOn, 1);
        if (_uwMaskTol >= 0) _gl.Uniform1(_uwMaskTol, Math.Max(1f, PlanarReflections.Tolerance));
        if (_uwMaskCentre >= 0) _gl.Uniform2(_uwMaskCentre, cx, v.Cy);
        if (_uwMaskH >= 0) _gl.Uniform1(_uwMaskH, Math.Max(1f, v.H));
        if (_uwMaskSize >= 0) _gl.Uniform2(_uwMaskSize, (float)src.Wide1x, src.H);
        _gl.ActiveTexture(TextureUnit.Texture0 + MaskUnit);
        _gl.BindTexture(TextureTarget.Texture2D, src.Surface);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.Enable(EnableCap.ClipDistance0);

        // A plane Y = h in this view: a view position's world Y is column 1 of R
        // dotted with it less the translation, plus the camera's.
        float tdot = v.R01 * v.Tx + v.R11 * v.Ty + v.R21 * v.Tz;
        int n = Math.Min(f.PlaneCount, RetainedScene.MaxPlanes);
        for (int k = 0; k < n; k++)
        {
            float h = f.Planes[k];
            _retPlanes[k * 4] = v.R01;
            _retPlanes[k * 4 + 1] = v.R11;
            _retPlanes[k * 4 + 2] = v.R21;
            _retPlanes[k * 4 + 3] = (float)(v.CamY - h) - tdot;
            if (_uwPlaneY >= 0) _gl.Uniform1(_uwPlaneY, h);
            CullChunks(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, v.H, cx, v.Cy, src.Wide1x, src.H, true, h, v.H,
                       float.PositiveInfinity);
            if (_uwMaskPlane >= 0)
                _gl.Uniform4(_uwMaskPlane, _retPlanes[k * 4], _retPlanes[k * 4 + 1], _retPlanes[k * 4 + 2], _retPlanes[k * 4 + 3]);
            SendWorldLights(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, cx, v.Cy, v.H, true, h);
            DrawWorldRanges(f);
            if (RetainedScene.Probe) CountFacing(f);
        }

        _gl.Disable(EnableCap.ClipDistance0);
        if (_uwMaskOn >= 0) _gl.Uniform1(_uwMaskOn, 0);
        if (_uwMirror >= 0) _gl.Uniform1(_uwMirror, 0);
        _gl.ActiveTexture(TextureUnit.Texture0 + MaskUnit);
        _gl.BindTexture(TextureTarget.Texture2D, 0);
        _gl.ActiveTexture(TextureUnit.Texture0);
        p.LastDrawFrame = _frame;
        _retPlanar = p;
        _retPlaneN = n;
        RetainedScene.PlanarDraws++;
    }

    // The six faces as rows of a world-to-face rotation, in OpenGL's cube-map
    // convention (the spec's table of major axis, s and t), so a direction looks
    // up the texel this face drew for it.
    static readonly float[][] CubeFaces =
    [
        [0, 0, -1, 0, -1, 0, 1, 0, 0],   // +X
        [0, 0, 1, 0, -1, 0, -1, 0, 0],   // -X
        [1, 0, 0, 0, 0, 1, 0, 1, 0],     // +Y
        [1, 0, 0, 0, 0, -1, 0, -1, 0],   // -Y
        [1, 0, 0, 0, -1, 0, 0, 0, 1],    // +Z
        [-1, 0, 0, 0, -1, 0, 0, 0, -1],  // -Z
    ];

    unsafe void EnsureCube(int n)
    {
        if (_cubeTex != 0 && _cubeMade == n) return;
        if (_cubeTex == 0)
        {
            _cubeTex = _gl.GenTexture();
            _cubeDepth = _gl.GenTexture();
            _cubeFbo = _gl.GenFramebuffer();
        }
        int levels = 1 + (int)Math.Floor(Math.Log2(n));
        _gl.BindTexture(TextureTarget.TextureCubeMap, _cubeTex);
        for (int i = 0; i < 6; i++)
            _gl.TexImage2D(TextureTarget.TextureCubeMapPositiveX + i, 0, InternalFormat.Rgba8, (uint)n, (uint)n, 0,
                PixelFormat.Rgba, PixelType.UnsignedByte, null);
        _gl.GenerateMipmap(TextureTarget.TextureCubeMap);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMinFilter, (int)GLEnum.LinearMipmapLinear);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapR, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMaxLevel, levels - 1);
        _gl.BindTexture(TextureTarget.TextureCubeMap, _cubeDepth);
        for (int i = 0; i < 6; i++)
            _gl.TexImage2D(TextureTarget.TextureCubeMapPositiveX + i, 0, InternalFormat.DepthComponent24, (uint)n, (uint)n, 0,
                PixelFormat.DepthComponent, PixelType.Float, null);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureCompareMode, (int)GLEnum.None);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.TextureCubeMap, TextureParameterName.TextureWrapR, (int)GLEnum.ClampToEdge);
        _gl.BindTexture(TextureTarget.TextureCubeMap, 0);
        _cubeMade = n;
    }

    void DrawRetainedCube(RetainedScene.Frame f, GlDisplayRt src)
    {
        int n = Math.Clamp(RetainedScene.CubeSize, 16, 2048);
        EnsureCube(n);
        var v = f.View;
        // A hit's image is fogged at the picture pixel's depth along the whole path,
        // and that depth is the path times the cosine of the pixel's ray, which is
        // least at the picture's corners.
        float cxm = v.Cx + src.Margin, hh = Math.Max(1f, v.H);
        float ex = Math.Max(cxm, src.Wide1x - cxm) / hh, ey = Math.Max(v.Cy, src.H - v.Cy) / hh;
        float cubeReach = ScreenReflections.FogBlackDepth() * MathF.Sqrt(ex * ex + ey * ey + 1f);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _cubeFbo);
        _gl.Viewport(0, 0, (uint)n, (uint)n);
        if (_uwMirror >= 0) _gl.Uniform1(_uwMirror, 0);
        if (_uwMaskOn >= 0) _gl.Uniform1(_uwMaskOn, 0);
        for (int i = 0; i < 6; i++)
        {
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.TextureCubeMapPositiveX + i, _cubeTex, 0);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.DepthAttachment,
                TextureTarget.TextureCubeMapPositiveX + i, _cubeDepth, 0);
            _gl.ColorMask(true, true, true, true);
            _gl.DepthMask(true);
            _gl.ClearColor(0f, 0f, 0f, 0f);
            _gl.ClearDepth(1.0);
            _gl.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);
            SetWorldView(CubeFaces[i], v.CamX, v.CamY, v.CamZ, 0f, 0f, 0f, n * 0.5f, n * 0.5f, n * 0.5f, n, n, v.H,
                         false, 1);
            CullChunks(CubeFaces[i], v.CamX, v.CamY, v.CamZ, 0f, 0f, 0f, n * 0.5f, n * 0.5f, n * 0.5f, n, n, false, 0f,
                       v.H, cubeReach);
            SendWorldLights(CubeFaces[i], v.CamX, v.CamY, v.CamZ, 0f, 0f, 0f, n * 0.5f, n * 0.5f, n * 0.5f, false, 0f);
            DrawWorldRanges(f);
        }
        _gl.BindTexture(TextureTarget.TextureCubeMap, _cubeTex);
        _gl.GenerateMipmap(TextureTarget.TextureCubeMap);
        _gl.BindTexture(TextureTarget.TextureCubeMap, 0);
        _retCube = true;
        RetainedScene.CubeDraws++;
    }

    /// <summary>The reflection pass's half: the planes and the cubemap this present
    /// drew, or both off.</summary>
    void BindRetainedForSsr()
    {
        bool planar = _retPlanar != null && _retPlaneN > 0;
        if (_uSsrRetN >= 0) _gl.Uniform1(_uSsrRetN, planar ? _retPlaneN : 0);
        if (planar)
        {
            if (_uSsrRetPlane >= 0) _gl.Uniform4(_uSsrRetPlane, (uint)_retPlaneN, _retPlanes.AsSpan(0, _retPlaneN * 4));
            if (_uSsrPlanarTol >= 0) _gl.Uniform1(_uSsrPlanarTol, Math.Max(1f, PlanarReflections.Tolerance));
            if (_uSsrRipple >= 0) _gl.Uniform1(_uSsrRipple, Math.Max(0f, PlanarReflections.Ripple));
            _gl.ActiveTexture(TextureUnit.Texture4);
            _gl.BindTexture(TextureTarget.Texture2D, _retPlanar!.Depth);
            _gl.ActiveTexture(TextureUnit.Texture3);
            _gl.BindTexture(TextureTarget.Texture2D, _retPlanar.Tex);
            _gl.ActiveTexture(TextureUnit.Texture0);
        }
        bool cube = _retCube && _retFrame != null;
        if (_uSsrCubeOn >= 0) _gl.Uniform1(_uSsrCubeOn, cube ? 1 : 0);
        if (cube)
        {
            var v = _retFrame!.View;
            // The view-to-world rotation is R transposed; GLSL's column-major read
            // of the row-major R is exactly that.
            Span<float> r = [v.R00, v.R01, v.R02, v.R10, v.R11, v.R12, v.R20, v.R21, v.R22];
            if (_uSsrToWorld >= 0) _gl.UniformMatrix3(_uSsrToWorld, 1, false, r);
            if (_uSsrViewT >= 0) _gl.Uniform3(_uSsrViewT, v.Tx, v.Ty, v.Tz);
            if (_uSsrCubeSteps >= 0) _gl.Uniform1(_uSsrCubeSteps, Math.Clamp(RetainedScene.CubeSteps, 1, 256));
            if (_uSsrCubeSize >= 0) _gl.Uniform1(_uSsrCubeSize, (float)_cubeMade);
            _gl.ActiveTexture(TextureUnit.Texture0 + CubeDepthUnit);
            _gl.BindTexture(TextureTarget.TextureCubeMap, _cubeDepth);
            _gl.ActiveTexture(TextureUnit.Texture0 + CubeUnit);
            _gl.BindTexture(TextureTarget.TextureCubeMap, _cubeTex);
            _gl.ActiveTexture(TextureUnit.Texture0);
        }
    }

    // ---- authored lights, glows and mipmaps in the world program -----------------

    /// <summary>The frame's lights and glows reach the retained scene: the corners carry
    /// their RGBC while either is on, so with neither the draws are as they were.</summary>
    void BeginWorldLights()
    {
        _wLightN = RetainedScene.Lit && RemasterUniforms.Active && _uwLightN >= 0 ? RemasterUniforms.LightCount : 0;
        bool emit = RetainedScene.Lit && (SurfaceMaterial.AnyEmissive || (SurfaceMaterial.AnySpecular && _wLightN != 0)) && _uwEmitOn >= 0;
        RetainedScene.LitLights = _wLightN;
        RetainedScene.LitGlow = emit;
        if (emit) BindMaterials();
        _gl.UseProgram(_progWorld);
        if (_uwEmitOn >= 0) _gl.Uniform1(_uwEmitOn, emit ? 1 : 0);
        if (_uwWorldLit >= 0) _gl.Uniform1(_uwWorldLit, emit || _wLightN != 0 ? 1 : 0);
        if (_uwLightN >= 0) _gl.Uniform1(_uwLightN, _wLightN);
        if (_wLightN == 0) return;
        _gl.Uniform4(_uwLightCol, (uint)_wLightN, new ReadOnlySpan<float>(RemasterUniforms.LightCol, 0, _wLightN * 4));
        if (_uwLightShadow < 0) return;
        int mask = Math.Max(_shadowReadyMask, 0);
        for (int i = 0; i < RemasterUniforms.MaxLights; i++)
        {
            int sl = i < _wLightN ? RemasterUniforms.LightShadow[i] : -1;
            _shadowSend[i] = sl >= 0 && sl < RemasterUniforms.MaxShadows && (mask & (1 << sl)) != 0 ? sl : -1;
        }
        _gl.Uniform1(_uwLightShadow, (uint)_shadowSend.Length, _shadowSend);
        if (mask == 0) return;
        _gl.Uniform1(_uwShadowSize, (float)Math.Clamp(RemasterUniforms.ShadowSize, 64, 4096));
        _gl.Uniform1(_uwShadowOffset, RemasterUniforms.ShadowOffset);
        _gl.Uniform1(_uwShadowBias, RemasterUniforms.ShadowBias);
        _gl.Uniform1(_uwShadowSoft, RemasterUniforms.ShadowSoft);
        for (int sl = 0; sl < RemasterUniforms.MaxShadows; sl++)
            if ((mask & (1 << sl)) != 0)
            {
                _gl.ActiveTexture(TextureUnit.Texture0 + ShadowUnit + sl);
                _gl.BindTexture(TextureTarget.TextureCubeMap, _shadowBind[sl]);
            }
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    /// <summary>BeginWorldLights' textures again, its uniforms left as they are.</summary>
    void BindWorldLights()
    {
        if (RetainedScene.LitGlow) BindMaterials();
        if (_wLightN == 0 || _uwLightShadow < 0) return;
        int mask = Math.Max(_shadowReadyMask, 0);
        for (int sl = 0; sl < RemasterUniforms.MaxShadows; sl++)
            if ((mask & (1 << sl)) != 0)
            {
                _gl.ActiveTexture(TextureUnit.Texture0 + ShadowUnit + sl);
                _gl.BindTexture(TextureTarget.TextureCubeMap, _shadowBind[sl]);
            }
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    /// <summary>Back to the program as the shadow cubemaps draw with it: no light, no
    /// glow, nothing shadowed.</summary>
    void EndWorldLights()
    {
        if (_uwLightN >= 0) _gl.Uniform1(_uwLightN, 0);
        if (_uwEmitOn >= 0) _gl.Uniform1(_uwEmitOn, 0);
        if (_uwWorldLit >= 0) _gl.Uniform1(_uwWorldLit, 0);
        if (_uwMipOn >= 0) _gl.Uniform1(_uwMipOn, 0f);
        if (_uwLightShadow >= 0) _gl.Uniform1(_uwLightShadow, (uint)NoShadows.Length, NoShadows);
        _wLightN = 0;
    }

    /// <summary>The lights in one view's space: what the view's rotation and translation
    /// make of their world positions, mirrored first in <paramref name="planeY"/> for a
    /// planar pass, whose geometry is mirrored the same way; and the projection the
    /// shader rebuilds a fragment's position with. The shadow lookup turns a view
    /// offset back to world axes with the same rotation, mirror included.</summary>
    void SendWorldLights(ReadOnlySpan<float> r, double camX, double camY, double camZ, float tx, float ty, float tz,
                         float cx, float cy, float h, bool mirror, float planeY)
    {
        if (_wLightN == 0) return;
        for (int i = 0; i < _wLightN; i++)
        {
            int o = i * 4;
            double wx = RemasterUniforms.LightWorldPos[o], wy = RemasterUniforms.LightWorldPos[o + 1], wz = RemasterUniforms.LightWorldPos[o + 2];
            float dx = RemasterUniforms.LightWorldDir[o], dy = RemasterUniforms.LightWorldDir[o + 1], dz = RemasterUniforms.LightWorldDir[o + 2];
            if (mirror) { wy = 2.0 * planeY - wy; dy = -dy; }
            double px = wx - camX, py = wy - camY, pz = wz - camZ;
            _wLightPos[o] = (float)(r[0] * px + r[1] * py + r[2] * pz) + tx;
            _wLightPos[o + 1] = (float)(r[3] * px + r[4] * py + r[5] * pz) + ty;
            _wLightPos[o + 2] = (float)(r[6] * px + r[7] * py + r[8] * pz) + tz;
            _wLightPos[o + 3] = RemasterUniforms.LightWorldPos[o + 3];
            _wLightDir[o] = r[0] * dx + r[1] * dy + r[2] * dz;
            _wLightDir[o + 1] = r[3] * dx + r[4] * dy + r[5] * dz;
            _wLightDir[o + 2] = r[6] * dx + r[7] * dy + r[8] * dz;
            _wLightDir[o + 3] = RemasterUniforms.LightDir[o + 3];
        }
        _gl.Uniform4(_uwLightPos, (uint)_wLightN, new ReadOnlySpan<float>(_wLightPos, 0, _wLightN * 4));
        _gl.Uniform4(_uwLightDir, (uint)_wLightN, new ReadOnlySpan<float>(_wLightDir, 0, _wLightN * 4));
        if (_uwLightCentre >= 0) _gl.Uniform2(_uwLightCentre, cx, cy);
        if (_uwLightH >= 0) _gl.Uniform1(_uwLightH, Math.Max(1f, h));
        if (_uwShadowToWorld >= 0)
        {
            // GLSL reads the row-major array as its transpose, the view-to-world turn;
            // a mirrored view's is that turn with Y flipped after it: R with column 1 negated.
            Span<float> m = [r[0], mirror ? -r[1] : r[1], r[2], r[3], mirror ? -r[4] : r[4], r[5], r[6], mirror ? -r[7] : r[7], r[8]];
            _gl.UniformMatrix3(_uwShadowToWorld, 1, false, m);
        }
    }

    // The mip atlas entries of the static map, by distinct texture, and the frame's models.
    // 0085: a static corner carries its texture's index (plus one) and the world
    // program looks the entry up in a table of one word per texture, so an entry that
    // moves costs a word, not the map's vertex buffer.
    readonly Dictionary<(int, int, uint), int> _mipKeyAt = new();
    readonly List<(int TPage, int Clut, uint Rect)> _mipKeys = new();
    uint[] _mipKeyEntry = [];
    uint[] _dynMip = [];
    int _mipKeysGen = -1;
    bool _mipTableDirty;
    uint _mipTableBuf, _mipTableTex;
    int _uwMipIndirect = -1;
    const int MipTableUnit = 17;

    /// <summary>The atlas entry of every retained texture, looked up each present so the
    /// entries the reflections use stay in the atlas and are rebuilt when VRAM under them
    /// changes; the table is uploaded again only when one moved. False when mipmaps are
    /// off, and every entry then reads 0.</summary>
    // Each map half's distinct textures, by half id less one: _halfKeyAt[h].._halfKeyAt[h+1]
    // in _halfKeys. A key no half owns (none on the map today) is looked up every time.
    int[] _halfKeyAt = [], _halfKeys = [], _mipLooked = [], _loneKeys = [];
    int _mipLookSerial;

    /// <summary>With <paramref name="halves"/>, only the textures of the halves it
    /// weighs above zero are looked up: the atlas holds what is drawn, as it does for
    /// the frame's own packets, and a key left out keeps its last entry, which no
    /// drawn corner reads.</summary>
    unsafe bool UpdateWorldMips(RetainedScene.Frame f, byte[]? halves = null)
    {
        bool on = RetainedScene.Mips && GteDepth.Mipmaps && _mip != null && _uwMipOn >= 0;
        var st = RetainedScene.Static;
        if (_mipKeysGen != RetainedScene.StaticGeneration)
        {
            _mipKeysGen = RetainedScene.StaticGeneration;
            _mipKeyAt.Clear();
            _mipKeys.Clear();
            var index = new uint[Math.Max(st.Length, 1)];
            for (int i = 0; i + 2 < st.Length; i += 3)
            {
                uint k = (uint)(MipKey(st[i]) + 1);
                index[i] = index[i + 1] = index[i + 2] = k;
            }
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _worldMipVbo);
            _gl.BufferData<uint>(BufferTargetARB.ArrayBuffer, index, BufferUsageARB.StaticDraw);
            _mipKeyEntry = new uint[Math.Max(_mipKeys.Count, 1)];
            _mipLooked = new int[Math.Max(_mipKeys.Count, 1)];
            _mipTableDirty = true;
            IndexHalfKeys(st, index);
        }
        long l0 = System.Diagnostics.Stopwatch.GetTimestamp();
        if (halves == null)
            for (int k = 0; k < _mipKeys.Count; k++) LookKey(k, on);
        else
        {
            _mipLookSerial++;
            foreach (int k in _loneKeys) LookKey(k, on);
            for (int h = 0; h + 1 < _halfKeyAt.Length && h < halves.Length; h++)
            {
                if (halves[h] == 0) continue;
                for (int i = _halfKeyAt[h]; i < _halfKeyAt[h + 1]; i++)
                {
                    int k = _halfKeys[i];
                    if (_mipLooked[k] == _mipLookSerial) continue;
                    _mipLooked[k] = _mipLookSerial;
                    LookKey(k, on);
                }
            }
        }
        int found = 0;
        foreach (var e in _mipKeyEntry) if (e != 0) found++;
        RetainedScene.MipsFound = found;
        RetainedScene.MipsKeys = _mipKeys.Count;
        if (_mipTableDirty)
        {
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, _mipTableBuf);
            _gl.BufferData<uint>(BufferTargetARB.TextureBuffer, _mipKeyEntry, BufferUsageARB.DynamicDraw);
            _gl.BindBuffer(BufferTargetARB.TextureBuffer, 0);
            _gl.BindTexture(TextureTarget.TextureBuffer, _mipTableTex);
            _gl.TexBuffer(TextureTarget.TextureBuffer, SizedInternalFormat.R32ui, _mipTableBuf);
            _gl.BindTexture(TextureTarget.TextureBuffer, 0);
            _mipTableDirty = false;
            RetainedScene.MipTableUploads++;
        }
        _gl.ActiveTexture(TextureUnit.Texture0 + MipTableUnit);
        _gl.BindTexture(TextureTarget.TextureBuffer, _mipTableTex);
        _gl.ActiveTexture(TextureUnit.Texture0);
        var d = f.SortedDynamic();
        if (_dynMip.Length < Math.Max(d.Length, 1)) _dynMip = new uint[Math.Max(Math.Max(d.Length, 1), _dynMip.Length * 2)];
        for (int i = 0; i + 2 < d.Length; i += 3)
        {
            uint e = on ? DynMip(d[i]) : 0u;
            _dynMip[i] = _dynMip[i + 1] = _dynMip[i + 2] = e;
        }
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _worldDynMipVbo);
        _gl.BufferData<uint>(BufferTargetARB.ArrayBuffer, new ReadOnlySpan<uint>(_dynMip, 0, Math.Max(d.Length, 1)), BufferUsageARB.StreamDraw);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        long l1 = System.Diagnostics.Stopwatch.GetTimestamp();
        // Decode what the lookups queued before anything reads the atlas.
        if (on && _mip!.HasPending) _mip.Process(_vram.SampleTexture);
        RetainedScene.MipTicks[0] += l1 - l0;
        RetainedScene.MipTicks[1] += System.Diagnostics.Stopwatch.GetTimestamp() - l1;
        return on;
    }

    void LookKey(int k, bool on)
    {
        var (tp, cl, rect) = _mipKeys[k];
        uint e = on ? MipOf(tp, cl, rect) : 0u;
        if (e != _mipKeyEntry[k]) { _mipKeyEntry[k] = e; _mipTableDirty = true; }
    }

    /// <summary>Which textures each half's corners carry, from the key indices just
    /// built (one plus the key, 0 for none) and the half in each corner's flags.</summary>
    void IndexHalfKeys(ReadOnlySpan<RetainedScene.Vertex> st, uint[] index)
    {
        int halves = RetainedScene.HalvesW * RetainedScene.HalvesH;
        var pairs = new List<long>();
        var lone = new HashSet<int>();
        for (int i = 0; i + 2 < st.Length; i += 3)
        {
            if (index[i] == 0) continue;
            int key = (int)index[i] - 1;
            uint hid = (st[i].Flags >> RetainedScene.HalfShift) & RetainedScene.HalfBits;
            if (hid == 0) lone.Add(key);
            else pairs.Add((long)(hid - 1) << 32 | (uint)key);
        }
        pairs.Sort();
        _halfKeyAt = new int[halves + 1];
        var keys = new List<int>();
        int p = 0;
        for (int h = 0; h < halves; h++)
        {
            _halfKeyAt[h] = keys.Count;
            long last = -1;
            for (; p < pairs.Count && (pairs[p] >> 32) == h; p++)
                if (pairs[p] != last) { keys.Add((int)(pairs[p] & 0xFFFFFFFF)); last = pairs[p]; }
        }
        _halfKeyAt[halves] = keys.Count;
        _halfKeys = keys.ToArray();
        _loneKeys = lone.ToArray();
    }

    int MipKey(in RetainedScene.Vertex v)
    {
        int tp = (int)(v.Texpage + 0.5f), cl = (int)(v.Clut + 0.5f);
        if ((tp & 0x8000) != 0 || (v.Flags & RetainedScene.FlagRect) == 0) return -1;
        var key = (tp & 0x1FF, (tp >> 7 & 3) == 2 ? 0 : cl, v.Rect);
        if (_mipKeyAt.TryGetValue(key, out int k)) return k;
        _mipKeyAt[key] = k = _mipKeys.Count;
        _mipKeys.Add(key);
        return k;
    }

    uint DynMip(in RetainedScene.Vertex v)
    {
        int tp = (int)(v.Texpage + 0.5f), cl = (int)(v.Clut + 0.5f);
        if ((tp & 0x8000) != 0 || (v.Flags & RetainedScene.FlagRect) == 0) return 0u;
        return MipOf(tp & 0x1FF, cl, v.Rect);
    }

    /// <summary>As <see cref="MipEntry"/> takes it, less the magnification test: a
    /// scrolling texture has none, since its fraction is the shader's.</summary>
    uint MipOf(int tpage, int clut, uint rect)
    {
        int u0 = (int)(rect & 0xFF), v0 = (int)((rect >> 8) & 0xFF);
        int w = (int)((rect >> 16) & 0xFF) - u0 + 1, h = (int)(rect >> 24) - v0 + 1;
        if (w <= 0 || h <= 0 || FluidOverlap(tpage, u0, v0, w, h)) return 0u;
        return _mip!.Lookup(tpage, clut, rect, _frame);
    }

    // ---- GPU time, for the probe ------------------------------------------------

    uint _planarQuery, _cubeQuery;

    uint BeginRetainedTimer()
    {
        // Queries do not nest, and a frame capture's are running around the pass.
        if (!RetainedScene.Probe || !_timerQueries || GpuTrace.Sink != null || Diagnostics.GpuTimes.Enabled) return 0;
        var q = _gl.GenQuery();
        _gl.BeginQuery(QueryTarget.TimeElapsed, q);
        return q;
    }

    void EndRetainedTimer(uint q, ref uint slot, bool planar)
    {
        if (q == 0) return;
        _gl.EndQuery(QueryTarget.TimeElapsed);
        // Read the previous one, a present later, rather than wait on this one.
        if (slot != 0)
        {
            long ns = GpuTimeNs(slot);
            if (ns < 0) DeleteGpuTimer(slot);
            else if (planar) RetainedScene.PlanarGpuNs = ns;
            else RetainedScene.CubeGpuNs = ns;
        }
        slot = q;
    }
}
