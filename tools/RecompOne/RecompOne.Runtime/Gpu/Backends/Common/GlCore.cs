using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

public sealed partial class GlCore : IGpuBackend
{
    [StructLayout(LayoutKind.Sequential)]
    // W is the clip W the vertex shader divides by: the view depth GteDepth
    // recovered, or exactly 1 for everything that had none, which is every vertex
    // this renderer ever saw before.
    struct GlVertex { public float X, Y; public float R, G, B; public float Clut, Texpage; public float U, V; public float W, Z; }

    // 0048. GteLightMap's inputs, in a buffer of their own: widening GlVertex took
    // its VBO to exactly 16 MiB, and that alone cost area 1 two thirds of its frame
    // rate with the feature off. Uploaded only for a batch that carries them.
    // 0071. Mat is the packet's material, for an emissive one's glow.
    struct GlLight { public float Lx, Ly, Lz, Fog; public uint Light, Mat; }

    // 0060. The texture rectangle and the atlas entry, in a buffer of their own for
    // the same reason; uploaded only for a batch that carries them.
    struct GlTex { public uint Rect, Entry; }

    const int MaxVerts = 0x40000;

    readonly GL _gl;
    readonly IGlVram _vram;
    readonly VramCheck? _check = VramCheck.On ? new() : null;
    readonly List<uint> _images = [];
    readonly GlDisplayRt?[] _rts = new GlDisplayRt?[2];
    long _rtStamp;
    long _frame;

    /// <summary>
    /// Upstream advances the frame counter from HostWindow, once per host frame.
    /// This backend cannot: 0016 requires the advance to sit immediately after
    /// the trailing Flush in PresentDisplay, because the depth clear keys on
    /// LastDrawFrame != _frame and bumping it anywhere else makes the tail of the
    /// outgoing frame look like the head of the next one -- which is the bug that
    /// left every frame inheriting the last batch's depths. So this is a no-op and
    /// the counter is advanced where it must be.
    /// </summary>
    public void AdvanceFrame()
    {
    }

    uint _vao, _vbo, _presentVao, _presentVbo, _progPrim, _progPresent, _progPresent24;
    uint _presentFbo, _presentTex;
    int _presentW, _presentH;
    bool _presentNearest;

    // Ambient occlusion: two full-screen passes between the finished render target
    // and the present blit. Core profile only -- GLSL 120 has no textureless
    // fullscreen conveniences worth the second copy of the shader, and that backend
    // has never been available to test on.
    uint _progAo, _progAoBlur;
    uint _aoFbo, _aoTex, _aoBlurFbo, _aoBlurTex;
    int _aoW, _aoH;
    bool _aoFull;
    int _uAoOrigin, _uAoSize, _uAoTexSize, _uAoTexel, _uAoProjH, _uAoCentre;
    int _uAoRadius, _uAoStrength, _uAoBias, _uAoMaxDepth, _uAoSamples;
    int _uAoBOrigin, _uAoBSize, _uAoBTexSize, _uAoBTexel, _uAoBEdge;
    int _uAoNormalOn, _uAoNormalCompare;
    // 0059. The floor-plan term.
    int _uAoWorldOn, _uAoViewR, _uAoCam, _uAoWorldStrength, _uAoWorldRadius, _uAoTileUnits, _uAoSpan, _uAoWallHeight;
    uint _aoHeightTex;
    int _aoHeightGen = -1;
    int _uPresentAoOn;

    // 0058. The normal buffer the occlusion pass reads: the frame's own geometry,
    // redrawn into the target's own coordinates once the frame is finished.
    uint _progNormal, _nrmVao, _nrmVbo;
    int _nrmVboVerts;
    int _uNrmPosBias, _uNrmFbInv, _uNrmProjH, _uNrmCentre, _uNrmScale;

    // 0067. Screen-space reflections: one full-screen pass at present, reading the
    // target's colour, depth and surface buffer into its own premultiplied texture.
    uint _progSsr, _ssrFbo, _ssrTex, _ssrInfoTex;
    int _ssrW, _ssrH;
    bool _ssrInfo;
    int _uSsrOrigin, _uSsrSize, _uSsrTexSize, _uSsrProjH, _uSsrCentre;
    int _uSsrMaxDist, _uSsrThickness, _uSsrSky, _uSsrSteps;
    int _uSsrDqa, _uSsrDqb, _uSsrFogCurve;
    int _uPresentSsrOn;
    int _uPresentAoMatOn;
    // The reflection blur's mip chains of the picture and of the planar texture,
    // each with its framebuffer to blit level 0 through.
    uint _colorMipTex, _colorMipFbo, _planarMipTex, _planarMipFbo;
    int _colorMipW, _colorMipH, _planarMipW, _planarMipH;
    int _uSsrColorMipH, _uSsrPlanarMipH;
    // 0068. The planar texture the reflection pass reads first, and the clip plane
    // the prim program discards the water's underside with while drawing into one.
    int _uSsrPlanarOn, _uSsrPlanarPlane, _uSsrPlanarTol, _uSsrRipple, _uSsrCompare;
    int _uClipOn, _uClipPlane, _uClipCentre, _uClipH;
    int _clipOnSent = -1;
    // 0071. Authored lights.
    int _uLightN, _uLightPos, _uLightCol, _uLightDir, _uLightCentre, _uLightH;
    // 0071. Emissive materials, and 0067's table they are read from.
    int _uEmitOn, _emitOnSent;
    int _lightNSent = -1, _lightsSentGen = -1, _kRemasterGen;

    uint _postProg, _postFbo, _postTex;
    int _postW, _postH, _postVersion = -1;
    int _uPostTexSize, _uPostOutputSize, _uPostTime, _uPostFrame;
    int _postFrame, _postParamVersion = -1;
    (string Name, float Value)[] _postParams = [];
    int[] _postParamLoc = [];
    readonly System.Diagnostics.Stopwatch _postClock = System.Diagnostics.Stopwatch.StartNew();

    readonly GlVertex[] _verts = new GlVertex[MaxVerts];
    int _count;
    readonly GlLight[] _lights = new GlLight[MaxVerts];
    // Light slots written so far this batch; 0 means none, and the attributes stay off.
    int _litFilled;
    uint _vboLight;
    readonly GlTex[] _texs = new GlTex[MaxVerts];
    int _texFilled;
    uint _vboTex;
    bool _texAttribs;
    GlTexCache? _mip;
    int _uMipOn;
    // The last lookup, since a quad asks twice.
    uint _mipLastRect, _mipLastEntry;
    int _mipLastTPage = -1, _mipLastClut, _mipLastClock;
    long _mipLastFrame = -1;
    // Where the next batch's vertices go in _vbo and _vboLight. Appending keeps an
    // upload off the range the previous batch's draw may still be reading, which
    // the driver would otherwise wait for; the buffers are orphaned on wrap.
    int _vboCursor;
    bool _lightAttribs;
    float _drawMinX, _drawMinY, _drawMaxX, _drawMaxY;
    bool _drawEmpty = true;

    HleDrawEnv _env;

    GlDisplayRt? _kTarget;
    bool _kTransparent;
    int _kImage = -1;
    int _kBlend, _kSetMask, _kCheckMask;
    int _kZMode;
    // 0048. The BK/LCM generation the batch's directional triangles were lit with; -1 none.
    int _kLightGen = -1;
    int _uLightBk, _uLcmR, _uLcmG, _uLcmB;
    // The last render target a depth-testing batch was drawn to, for the diagnostic
    // readback: it is the only unambiguous answer to "which depth buffer is this
    // frame's". Diagnostic only — nothing else reads it.
    GlDisplayRt? _lastZRt;
    int _kTwAndX, _kTwAndY, _kTwOrX, _kTwOrY;
    int _kClipX0, _kClipY0, _kClipX1, _kClipY1;
    uint _kRepTex, _kRepClut;
    float _kRepX, _kRepY, _kRepW, _kRepH;
    int _kRepClutCount;
    int _uTexWindow, _uBlend, _uBlendOpaque, _uSetMask, _uCheckMask, _uPosBias, _uFbInv;
    int _uTrueColor;
    int _uAniso;
    int _uFluidN;
    readonly int[] _uFluidRect = new int[8];
    readonly int[] _uFluidOff = new int[8];
    // What the prim program holds already: a uniform keeps its value across batches,
    // and re-sending these was a GL call per slot per batch.
    int _fluidSentN = -1;
    readonly GteDepth.FluidRec[] _fluidSent = new GteDepth.FluidRec[GteDepth.FluidSlots];
    int _uPrimScale, _primScaleSent = -1;
    int _uOpaqueDepth, _uDepthBias, _uDepthSlope;
    // The true-color flag the live display targets were built with. When it drifts
    // from GteDepth.TrueColor the targets carry the wrong pixel format, so they are
    // torn down at the next present and rebuilt (their content survives in VRAM).
    bool _rtsTrueColor;
    int _uRepRect, _uRepClutCount;
    int _uPresentOrigin, _uPresentSize, _uPresentTexSize, _uPresent24Origin, _uPresent24Size;

    public bool Ready { get; private set; }

    readonly bool _legacy;
    int _uVramSize, _uDestSize, _uSemiTrans, _uBlendMode;

    public GlCore(GL gl, IGlVram vram, bool legacy = false)
    {
        _gl = gl;
        _vram = vram;
        _legacy = legacy;
    }

    public unsafe void InitGl()
    {
        _vram.Init();
        // 0046. Core since 3.3; the 2.1 context needs the extension.
        _timerQueries = !_legacy || _gl.IsExtensionPresent("ARB_timer_query");

        string primVs = _legacy ? GlShaders.PrimVs120 : GlShaders.PrimVs;
        string primFs = _legacy ? GlShaders.PrimFs120 : GlShaders.PrimFs;
        string fullVs = _legacy ? GlShaders.FullscreenVs120 : GlShaders.FullscreenVs;
        string presentFs = _legacy ? GlShaders.PresentFs120 : GlShaders.PresentFs;
        string present24Fs = _legacy ? GlShaders.Present24Fs120 : GlShaders.Present24Fs;

        _progPrim = GlShaders.BuildPrim(_gl, primVs, primFs, "prim");
        _progPresent = GlShaders.BuildFullscreen(_gl, fullVs, presentFs, "present");
        _progPresent24 = GlShaders.BuildFullscreen(_gl, fullVs, present24Fs, "present24");
        if (_progPrim == 0 || _progPresent == 0 || _progPresent24 == 0) return;

        _uVramSize = _gl.GetUniformLocation(_progPrim, "uVramSize");
        _uDestSize = _gl.GetUniformLocation(_progPrim, "uDestSize");
        _uSemiTrans = _gl.GetUniformLocation(_progPrim, "uSemiTrans");
        _uBlendMode = _gl.GetUniformLocation(_progPrim, "uBlendMode");

        _uTexWindow = _gl.GetUniformLocation(_progPrim, "uTexWindow");
        _uBlend = _gl.GetUniformLocation(_progPrim, "uBlend");
        _uBlendOpaque = _gl.GetUniformLocation(_progPrim, "uBlendOpaque");
        _uSetMask = _gl.GetUniformLocation(_progPrim, "uSetMask");
        _uCheckMask = _gl.GetUniformLocation(_progPrim, "uCheckMask");
        _uPosBias = _gl.GetUniformLocation(_progPrim, "uPosBias");
        _uFbInv = _gl.GetUniformLocation(_progPrim, "uFbInv");
        _uTrueColor = _gl.GetUniformLocation(_progPrim, "uTrueColor");
        _uAniso = _gl.GetUniformLocation(_progPrim, "uAniso");
        _uMipOn = _gl.GetUniformLocation(_progPrim, "uMipOn");
        _uFluidN = _gl.GetUniformLocation(_progPrim, "uFluidN");
        for (int i = 0; i < 8; i++)
        {
            _uFluidRect[i] = _gl.GetUniformLocation(_progPrim, $"uFluidRect[{i}]");
            _uFluidOff[i] = _gl.GetUniformLocation(_progPrim, $"uFluidOff[{i}]");
        }
        _fluidSentN = -1;
        Array.Clear(_fluidSent);
        _uOpaqueDepth = _gl.GetUniformLocation(_progPrim, "uOpaqueDepth");
        _uDepthBias = _gl.GetUniformLocation(_progPrim, "uDepthBias");
        _uDepthSlope = _gl.GetUniformLocation(_progPrim, "uDepthSlope");
        _uLightBk = _gl.GetUniformLocation(_progPrim, "uLightBk");
        _uLcmR = _gl.GetUniformLocation(_progPrim, "uLcmR");
        _uLcmG = _gl.GetUniformLocation(_progPrim, "uLcmG");
        _uLcmB = _gl.GetUniformLocation(_progPrim, "uLcmB");
        GteLightMap.Supported = !_legacy && _uLightBk >= 0;
        _rtsTrueColor = GteDepth.TrueColor;
        _uClipOn = _gl.GetUniformLocation(_progPrim, "uClipOn");
        _uClipPlane = _gl.GetUniformLocation(_progPrim, "uClipPlane");
        _uClipCentre = _gl.GetUniformLocation(_progPrim, "uClipCentre");
        _uClipH = _gl.GetUniformLocation(_progPrim, "uClipH");
        _clipOnSent = -1;
        _uLightN = _gl.GetUniformLocation(_progPrim, "uLightN");
        _uLightPos = _gl.GetUniformLocation(_progPrim, "uLightPos");
        _uLightCol = _gl.GetUniformLocation(_progPrim, "uLightCol");
        _uLightDir = _gl.GetUniformLocation(_progPrim, "uLightDir");
        _uLightCentre = _gl.GetUniformLocation(_progPrim, "uLightCentre");
        _uLightH = _gl.GetUniformLocation(_progPrim, "uLightH");
        _lightNSent = _lightsSentGen = -1;
        RemasterUniforms.Supported = !_legacy && _uLightN >= 0 && _uLightPos >= 0;
        _uEmitOn = _gl.GetUniformLocation(_progPrim, "uEmitOn");
        _emitOnSent = -1;
        _uRepRect = _gl.GetUniformLocation(_progPrim, "uRepRect");
        _uRepClutCount = _gl.GetUniformLocation(_progPrim, "uRepClutCount");

        _gl.UseProgram(_progPrim);
        _gl.Uniform1(_gl.GetUniformLocation(_progPrim, "uVram"), 0);
        _gl.Uniform1(_gl.GetUniformLocation(_progPrim, "uDest"), 1);
        _gl.Uniform1(_gl.GetUniformLocation(_progPrim, "uExtTex"), 2);
        _gl.Uniform1(_gl.GetUniformLocation(_progPrim, "uRepTex"), 3);
        _gl.Uniform1(_gl.GetUniformLocation(_progPrim, "uRepClut"), 4);
        int uMip = _gl.GetUniformLocation(_progPrim, "uMip");
        if (uMip >= 0) _gl.Uniform1(uMip, 5);
        int uMatPrim = _gl.GetUniformLocation(_progPrim, "uMatTable");
        if (uMatPrim >= 0) _gl.Uniform1(uMatPrim, MatUnit);
        _uPrimScale = _gl.GetUniformLocation(_progPrim, "uScale");
        SetScaleUniform(_progPrim, GlVram.Scale);
        _primScaleSent = GlVram.Scale;
        if (_uVramSize >= 0) _gl.Uniform2(_uVramSize, (float)VramShadow.Width, VramShadow.Height);

        _uPresentOrigin = _gl.GetUniformLocation(_progPresent, "uOrigin");
        _uPresentSize = _gl.GetUniformLocation(_progPresent, "uSize");
        _uPresentTexSize = _gl.GetUniformLocation(_progPresent, "uTexSize");
        _uPresentAoOn = _gl.GetUniformLocation(_progPresent, "uAoOn");
        _gl.UseProgram(_progPresent);
        _gl.Uniform1(_gl.GetUniformLocation(_progPresent, "uVram"), 0);
        int uPresentAo = _gl.GetUniformLocation(_progPresent, "uAo");
        if (uPresentAo >= 0) _gl.Uniform1(uPresentAo, 1);
        if (_uPresentAoOn >= 0) _gl.Uniform1(_uPresentAoOn, 0f);
        _uPresentSsrOn = _gl.GetUniformLocation(_progPresent, "uSsrOn");
        int uPresentSsr = _gl.GetUniformLocation(_progPresent, "uSsr");
        if (uPresentSsr >= 0) _gl.Uniform1(uPresentSsr, 2);
        if (_uPresentSsrOn >= 0) _gl.Uniform1(_uPresentSsrOn, 0f);
        _uPresentAoMatOn = _gl.GetUniformLocation(_progPresent, "uAoMatOn");
        if (_uPresentAoMatOn >= 0)
        {
            _gl.Uniform1(_uPresentAoMatOn, 0f);
            _gl.Uniform1(_gl.GetUniformLocation(_progPresent, "uSurface"), 3);
            _gl.Uniform1(_gl.GetUniformLocation(_progPresent, "uMatTable"), MatUnit);
        }

        // Ambient occlusion. A failure here disables the pass and nothing else --
        // the backend is perfectly usable without it, so it is deliberately not
        // part of the early return above.
        if (!_legacy)
        {
            _progAo = GlShaders.BuildFullscreen(_gl, fullVs, GlShaders.AoFs, "ao");
            _progAoBlur = GlShaders.BuildFullscreen(_gl, fullVs, GlShaders.AoBlurFs, "aoblur");
            if (_progAo != 0)
            {
                _uAoOrigin = _gl.GetUniformLocation(_progAo, "uOrigin");
                _uAoSize = _gl.GetUniformLocation(_progAo, "uSize");
                _uAoTexSize = _gl.GetUniformLocation(_progAo, "uTexSize");
                _uAoTexel = _gl.GetUniformLocation(_progAo, "uTexel");
                _uAoProjH = _gl.GetUniformLocation(_progAo, "uProjH");
                _uAoCentre = _gl.GetUniformLocation(_progAo, "uCentre");
                _uAoRadius = _gl.GetUniformLocation(_progAo, "uRadius");
                _uAoStrength = _gl.GetUniformLocation(_progAo, "uStrength");
                _uAoBias = _gl.GetUniformLocation(_progAo, "uBias");
                _uAoMaxDepth = _gl.GetUniformLocation(_progAo, "uMaxDepth");
                _uAoSamples = _gl.GetUniformLocation(_progAo, "uSamples");
                _uAoNormalOn = _gl.GetUniformLocation(_progAo, "uNormalOn");
                _uAoNormalCompare = _gl.GetUniformLocation(_progAo, "uNormalCompare");
                _uAoWorldOn = _gl.GetUniformLocation(_progAo, "uWorldOn");
                _uAoViewR = _gl.GetUniformLocation(_progAo, "uViewR");
                _uAoCam = _gl.GetUniformLocation(_progAo, "uCam");
                _uAoWorldStrength = _gl.GetUniformLocation(_progAo, "uWorldStrength");
                _uAoWorldRadius = _gl.GetUniformLocation(_progAo, "uWorldRadius");
                _uAoTileUnits = _gl.GetUniformLocation(_progAo, "uTileUnits");
                _uAoSpan = _gl.GetUniformLocation(_progAo, "uSpan");
                _uAoWallHeight = _gl.GetUniformLocation(_progAo, "uWallHeight");
                _gl.UseProgram(_progAo);
                _gl.Uniform1(_gl.GetUniformLocation(_progAo, "uDepth"), 0);
                _gl.Uniform1(_gl.GetUniformLocation(_progAo, "uNormal"), 1);
                _gl.Uniform1(_gl.GetUniformLocation(_progAo, "uHeight"), 2);
                if (_uAoNormalOn >= 0) _gl.Uniform1(_uAoNormalOn, 0f);
                if (_uAoWorldOn >= 0) _gl.Uniform1(_uAoWorldOn, 0f);
            }
            if (_progAoBlur != 0)
            {
                _uAoBOrigin = _gl.GetUniformLocation(_progAoBlur, "uOrigin");
                _uAoBSize = _gl.GetUniformLocation(_progAoBlur, "uSize");
                _uAoBTexSize = _gl.GetUniformLocation(_progAoBlur, "uTexSize");
                _uAoBTexel = _gl.GetUniformLocation(_progAoBlur, "uTexel");
                _uAoBEdge = _gl.GetUniformLocation(_progAoBlur, "uEdge");
                _gl.UseProgram(_progAoBlur);
                _gl.Uniform1(_gl.GetUniformLocation(_progAoBlur, "uAo"), 0);
                _gl.Uniform1(_gl.GetUniformLocation(_progAoBlur, "uDepth"), 1);
            }

            // 0058. The normal buffer's own program and vertex array. It shares
            // nothing with the prim VAO: three floats and a position, drawn once a
            // frame from a list the port kept.
            _progNormal = GlShaders.Build(_gl, GlShaders.NormalVs, GlShaders.NormalFs, "aonormal",
                [(0, "inPos"), (1, "inZ"), (2, "inM")]);
            if (_progNormal != 0)
            {
                _uNrmPosBias = _gl.GetUniformLocation(_progNormal, "uPosBias");
                _uNrmFbInv = _gl.GetUniformLocation(_progNormal, "uFbInv");
                _uNrmProjH = _gl.GetUniformLocation(_progNormal, "uProjH");
                _uNrmCentre = _gl.GetUniformLocation(_progNormal, "uCentre");
                _uNrmScale = _gl.GetUniformLocation(_progNormal, "uScale");

                _nrmVao = _gl.GenVertexArray();
                _nrmVbo = _gl.GenBuffer();
                _gl.BindVertexArray(_nrmVao);
                _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _nrmVbo);
                uint ns = (uint)sizeof(AoGeometry.V);
                _gl.EnableVertexAttribArray(0); _gl.VertexAttribPointer(0, 2, VertexAttribPointerType.Float, false, ns, (void*)0);
                _gl.EnableVertexAttribArray(1); _gl.VertexAttribPointer(1, 1, VertexAttribPointerType.Float, false, ns, (void*)8);
                _gl.EnableVertexAttribArray(2); _gl.VertexAttribPointer(2, 1, VertexAttribPointerType.Float, false, ns, (void*)12);
                _gl.BindVertexArray(0);
                _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
            }

            // 0067.
            _progSsr = GlShaders.BuildFullscreen(_gl, fullVs, GlShaders.SsrFs, "ssr");
            if (_progSsr != 0)
            {
                _uSsrOrigin = _gl.GetUniformLocation(_progSsr, "uOrigin");
                _uSsrSize = _gl.GetUniformLocation(_progSsr, "uSize");
                _uSsrTexSize = _gl.GetUniformLocation(_progSsr, "uTexSize");
                _uSsrProjH = _gl.GetUniformLocation(_progSsr, "uProjH");
                _uSsrCentre = _gl.GetUniformLocation(_progSsr, "uCentre");
                _uSsrMaxDist = _gl.GetUniformLocation(_progSsr, "uMaxDist");
                _uSsrThickness = _gl.GetUniformLocation(_progSsr, "uThickness");
                _uSsrSky = _gl.GetUniformLocation(_progSsr, "uSky");
                _uSsrSteps = _gl.GetUniformLocation(_progSsr, "uSteps");
                _uSsrDqa = _gl.GetUniformLocation(_progSsr, "uDqa");
                _uSsrDqb = _gl.GetUniformLocation(_progSsr, "uDqb");
                _uSsrFogCurve = _gl.GetUniformLocation(_progSsr, "uFogCurve");
                _uSsrPlanarOn = _gl.GetUniformLocation(_progSsr, "uPlanarOn");
                _uSsrPlanarPlane = _gl.GetUniformLocation(_progSsr, "uPlanarPlane");
                _uSsrPlanarTol = _gl.GetUniformLocation(_progSsr, "uPlanarTol");
                _uSsrRipple = _gl.GetUniformLocation(_progSsr, "uRipple");
                _uSsrCompare = _gl.GetUniformLocation(_progSsr, "uCompare");
                _gl.UseProgram(_progSsr);
                _gl.Uniform1(_gl.GetUniformLocation(_progSsr, "uDepth"), 0);
                _gl.Uniform1(_gl.GetUniformLocation(_progSsr, "uSurface"), 1);
                _gl.Uniform1(_gl.GetUniformLocation(_progSsr, "uColor"), 2);
                int uPlanar = _gl.GetUniformLocation(_progSsr, "uPlanar");
                if (uPlanar >= 0) _gl.Uniform1(uPlanar, 3);
                int uPlanarDepth = _gl.GetUniformLocation(_progSsr, "uPlanarDepth");
                if (uPlanarDepth >= 0) _gl.Uniform1(uPlanarDepth, 4);
                int uMatSsr = _gl.GetUniformLocation(_progSsr, "uMatTable");
                if (uMatSsr >= 0) _gl.Uniform1(uMatSsr, MatUnit);
                int uColorMip = _gl.GetUniformLocation(_progSsr, "uColorMip");
                if (uColorMip >= 0) _gl.Uniform1(uColorMip, ColorMipUnit);
                int uPlanarMip = _gl.GetUniformLocation(_progSsr, "uPlanarMip");
                if (uPlanarMip >= 0) _gl.Uniform1(uPlanarMip, PlanarMipUnit);
                _uSsrColorMipH = _gl.GetUniformLocation(_progSsr, "uColorMipH");
                _uSsrPlanarMipH = _gl.GetUniformLocation(_progSsr, "uPlanarMipH");
                if (_uSsrPlanarOn >= 0) _gl.Uniform1(_uSsrPlanarOn, 0);
            }
            // 0068. Only this backend can draw into a planar texture; the port walks
            // nothing mirrored without it.
            PlanarReflections.Supported = _progSsr != 0 && _uClipOn >= 0 && _uSsrPlanarOn >= 0;
            // 0072.
            InitRetained(_progSsr);
        }

        _uPresent24Origin = _gl.GetUniformLocation(_progPresent24, "uOrigin");
        _uPresent24Size = _gl.GetUniformLocation(_progPresent24, "uSize");
        _gl.UseProgram(_progPresent24);
        _gl.Uniform1(_gl.GetUniformLocation(_progPresent24, "uVram"), 0);
        SetScaleUniform(_progPresent24);
        int uVramSize24 = _gl.GetUniformLocation(_progPresent24, "uVramSize");
        if (uVramSize24 >= 0) _gl.Uniform2(uVramSize24, (float)GlVram.Width, GlVram.Height);

        _vao = _gl.GenVertexArray();
        _vbo = _gl.GenBuffer();
        _gl.BindVertexArray(_vao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vbo);
        _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(MaxVerts * sizeof(GlVertex)), null, BufferUsageARB.DynamicDraw);
        uint stride = (uint)sizeof(GlVertex);
        _gl.EnableVertexAttribArray(0); _gl.VertexAttribPointer(0, 2, VertexAttribPointerType.Float, false, stride, (void*)0);
        _gl.EnableVertexAttribArray(1); _gl.VertexAttribPointer(1, 3, VertexAttribPointerType.Float, false, stride, (void*)8);
        _gl.EnableVertexAttribArray(2); _gl.VertexAttribPointer(2, 1, VertexAttribPointerType.Float, false, stride, (void*)20);
        _gl.EnableVertexAttribArray(3); _gl.VertexAttribPointer(3, 1, VertexAttribPointerType.Float, false, stride, (void*)24);
        _gl.EnableVertexAttribArray(4); _gl.VertexAttribPointer(4, 2, VertexAttribPointerType.Float, false, stride, (void*)28);
        _gl.EnableVertexAttribArray(5); _gl.VertexAttribPointer(5, 1, VertexAttribPointerType.Float, false, stride, (void*)36);
        _gl.EnableVertexAttribArray(6); _gl.VertexAttribPointer(6, 1, VertexAttribPointerType.Float, false, stride, (void*)40);
        // 0048. Integer attributes need GL 3.0, and only the core shaders read these.
        // Left disabled until a batch carries them, when the generic value (0) reads
        // as "no record".
        if (!_legacy)
        {
            _vboLight = _gl.GenBuffer();
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vboLight);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(MaxVerts * sizeof(GlLight)), null, BufferUsageARB.DynamicDraw);
            uint ls = (uint)sizeof(GlLight);
            _gl.VertexAttribPointer(7, 3, VertexAttribPointerType.Float, false, ls, (void*)0);
            _gl.VertexAttribPointer(8, 1, VertexAttribPointerType.Float, false, ls, (void*)12);
            _gl.VertexAttribIPointer(9, 1, VertexAttribIType.UnsignedInt, ls, (void*)16);
            _gl.VertexAttribIPointer(11, 1, VertexAttribIType.UnsignedInt, ls, (void*)20);
            LightDefaults();

            // 0060. Disabled until a batch carries them; the generic 0 is "none".
            _vboTex = _gl.GenBuffer();
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vboTex);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(MaxVerts * sizeof(GlTex)), null, BufferUsageARB.DynamicDraw);
            _gl.VertexAttribIPointer(10, 2, VertexAttribIType.UnsignedInt, (uint)sizeof(GlTex), (void*)0);
            TexDefaults();
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vbo);
        }

        // fullscreen quad for present, real vbo since gl_VertexID without arrays does not draw on mesa for some reason?? or i did it wrong?
        _presentVao = _gl.GenVertexArray();
        _presentVbo = _gl.GenBuffer();
        _gl.BindVertexArray(_presentVao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _presentVbo);
        float[] quad = { -1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f };
        fixed (float* qp = quad)
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(quad.Length * sizeof(float)), qp, BufferUsageARB.StaticDraw);
        _gl.EnableVertexAttribArray(0);
        _gl.VertexAttribPointer(0, 2, VertexAttribPointerType.Float, false, 2 * sizeof(float), (void*)0);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);

        _presentTex = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, _presentTex);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _presentFbo = _gl.GenFramebuffer();
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _presentFbo);
        _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0, TextureTarget.Texture2D, _presentTex, 0);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);

        _kClipX1 = 1023; _kClipY1 = 511;

        // 0060. A failure leaves the mip path off and nothing else.
        if (!_legacy && _uMipOn >= 0)
        {
            _mip = new GlTexCache(_gl);
            _mip.Init();
            if (!_mip.Ready) _mip = null;
        }
        GteDepth.MipmapsLive = _mip != null;
        Ready = true;
    }

    public void SetDrawEnv(in HleDrawEnv env) => _env = env;

    const int FbSlackW = 64;
    const int FbSlackH = 32;

    // Upstream caches this on the clip rect and GpuHle.ViewVersion, and
    // invalidates it from the one eviction site it knows about. The port's
    // GetOrCreateRt is not that site -- it also destroys a target when the
    // aspect moves the margin, and PresentDisplay destroys idle ones -- so the
    // cache would hand back a destroyed target. Left uncached.
    GlDisplayRt? Classify()
    {
        var rt = ClassifyDisplay();
        return rt != null && PlanarReflections.Capturing ? EnsurePlanar(rt) : rt;
    }

    /// <summary>0068. The planar texture of the target the game is drawing into,
    /// made at its size and cleared the first time a capture reaches it. It takes
    /// the capture's two planes with it, so the pass that reads it later reads the
    /// planes it was drawn with.</summary>
    GlDisplayRt EnsurePlanar(GlDisplayRt rt)
    {
        var p = rt.Planar;
        if (p == null || p.W != rt.W || p.H != rt.H || p.Margin != rt.Margin || p.CreatedScale != GlVram.Scale)
        {
            if (p != null)
            {
                if (_kTarget == p) Flush(FlushReason.Target);
                p.Destroy(_gl);
            }
            p = new GlDisplayRt { X = rt.X, Y = rt.Y, W = rt.W, H = rt.H, Margin = rt.Margin, IsPlanar = true };
            p.Create(_gl);
            // Never written back to VRAM and never presented, only sampled -- and
            // sampled off the pixel grid when the water bends it.
            _gl.BindTexture(TextureTarget.Texture2D, p.Tex);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
            _gl.BindTexture(TextureTarget.Texture2D, 0);
            rt.Planar = p;
            rt.PlanarSerial = -1;
        }
        if (rt.PlanarSerial != PlanarReflections.Serial)
        {
            Flush(FlushReason.Target);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, p.Fbo);
            _gl.Disable(EnableCap.ScissorTest);
            _gl.ColorMask(true, true, true, true);
            _gl.ClearColor(0f, 0f, 0f, 0f);
            _gl.ClearDepth(1.0);
            _gl.DepthMask(true);
            _gl.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
            p.LastDrawFrame = _frame;
            p.ZGen = GteDepth.Generation;
            rt.PlanarSerial = PlanarReflections.Serial;
            rt.PlanarFrame = _frame;
            PlanarReflections.ViewPlane.CopyTo(rt.PlanarPlane, 0);
            PlanarReflections.ClipPlane.CopyTo(p.ClipPlane, 0);
            PlanarReflections.Cleared++;
        }
        return p;
    }

    /// <summary>0068. A capture with no display target has nowhere to go: drawn
    /// through, it would land in VRAM over whatever the game keeps there.</summary>
    bool PlanarDrop()
    {
        if (!PlanarReflections.Capturing || ClassifyDisplay() != null) return false;
        PlanarReflections.Dropped++;
        return true;
    }

    GlDisplayRt? ClassifyDisplay()
    {
        int clipX = _env.ClipX0, clipY = _env.ClipY0;
        int clipW = _env.ClipX1 - _env.ClipX0 + 1, clipH = _env.ClipY1 - _env.ClipY0 + 1;
        if (clipW <= 0 || clipH <= 0) return null;

        long bestStamp = -1;
        int fbX = 0, fbY = 0, fbW = 0, fbH = 0;
        for (int i = 0; i < GpuHle.RectCount; i++)
        {
            var r = GpuHle.GetRect(i);
            if (!r.Valid || r.W <= 0 || r.H <= 0 || r.Stamp <= bestStamp) continue;

            bool clipInside = clipX >= r.X && clipX + clipW <= r.X + r.W &&
                              clipY >= r.Y && clipY + clipH <= r.Y + r.H;
            bool clipIsFb = clipX <= r.X && clipX + clipW >= r.X + r.W &&
                            clipY <= r.Y && clipY + clipH >= r.Y + r.H &&
                            clipW - r.W <= FbSlackW && clipH - r.H <= FbSlackH;
            if (clipInside) { bestStamp = r.Stamp; fbX = r.X; fbY = r.Y; fbW = r.W; fbH = r.H; }
            else if (clipIsFb) { bestStamp = r.Stamp; fbX = clipX; fbY = clipY; fbW = clipW; fbH = clipH; }
        }
        return bestStamp < 0 ? null : GetOrCreateRt(fbX, fbY, fbW, fbH);
    }

    GlDisplayRt GetOrCreateRt(int fbX, int fbY, int fbW, int fbH)
    {
        int slot = -1;
        for (int i = 0; i < _rts.Length; i++)
            if (_rts[i] is { } rt && rt.X == fbX && rt.Y == fbY)
            {
                bool sameW = rt.W == fbW;
                bool fitsH = rt.H >= fbH && rt.H - fbH <= FbSlackH;
                if (sameW && fitsH && rt.Margin == GpuHle.WideMargin(rt.W))
                {
                    rt.Stamp = ++_rtStamp;
                    return rt;
                }
                slot = i;
                break;
            }

        if (slot < 0)
        {
            slot = 0;
            for (int i = 1; i < _rts.Length; i++)
            {
                if (_rts[i] == null) { slot = i; break; }
                if (_rts[slot] != null && _rts[i]!.Stamp < _rts[slot]!.Stamp) slot = i;
            }
        }

        if (_rts[slot] is { } old)
        {
            if (old.Dirty) Writeback(old);
            old.Destroy(_gl);
            if (old == _lastZRt) _lastZRt = null;
            // A target thrown away takes its depth attachment with it, so anything
            // already depth-tested into it stops occluding what comes next. Worth
            // counting: if this happens inside a frame the Z-buffer is being reset
            // halfway through one.
            if (GteDepth.ZBuffer) GteDepth.ZRtRecreated++;
        }

        var fresh = new GlDisplayRt { X = fbX, Y = fbY, W = fbW, H = fbH, Margin = GpuHle.WideMargin(fbW), Stamp = ++_rtStamp, LastDrawFrame = _frame };
        fresh.Create(_gl);
        _rts[slot] = fresh;
        SyncRtFromVram(fresh, fbX, fbY, fbW, fbH, fromSample: true);
        return fresh;
    }

    void Writeback(GlDisplayRt rt)
    {
        var profile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Writeback);
        WritebackCore(rt);
        Diagnostics.Profiler.End(profile);
    }

    void WritebackCore(GlDisplayRt rt)
    {
        int s = GlVram.Scale;
        _gl.Disable(EnableCap.ScissorTest);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, rt.Fbo);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, _vram.Fbo);
        _gl.BlitFramebuffer(rt.Margin * s, 0, (rt.Margin + rt.W) * s, rt.H * s,
            rt.X * s, rt.Y * s, (rt.X + rt.W) * s, (rt.Y + rt.H) * s,
            ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        _vram.Publish(rt.X, rt.Y, rt.W, rt.H);
        _check?.Check(_vram, "writeback", rt.X, rt.Y, rt.W, rt.H, true);
        rt.Dirty = false;
        Assets.Textures.VramTracker.MarkGpuWrite(rt.X, rt.Y, rt.W, rt.H);
    }

    void SyncRtFromVram(GlDisplayRt rt, int rx, int ry, int rw, int rh, bool fromSample)
    {
        int x0 = Math.Max(rx, rt.X), y0 = Math.Max(ry, rt.Y);
        int x1 = Math.Min(rx + rw, rt.X + rt.W), y1 = Math.Min(ry + rh, rt.Y + rt.H);
        if (x0 >= x1 || y0 >= y1) return;
        int s = GlVram.Scale;
        int dx0 = (x0 - rt.X + rt.Margin) * s, dy0 = (y0 - rt.Y) * s;
        int dx1 = (x1 - rt.X + rt.Margin) * s, dy1 = (y1 - rt.Y) * s;
        if (fromSample)
        {
            _vram.BlitSample(x0, y0, x1 - x0, y1 - y0, rt.Fbo, rt.TexW, rt.TexH,
                dx0, dy0, dx1 - dx0, dy1 - dy0);
            return;
        }
        _gl.Disable(EnableCap.ScissorTest);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _vram.Fbo);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, rt.Fbo);
        _gl.BlitFramebuffer(x0 * s, y0 * s, x1 * s, y1 * s,
            dx0, dy0, dx1, dy1,
            ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    void SyncRtsFromVram(int x, int y, int w, int h, bool fromSample = true)
    {
        foreach (var rt in _rts)
            if (rt != null && rt.Intersects(x, y, w, h)) SyncRtFromVram(rt, x, y, w, h, fromSample);
    }

    void WritebackDirtyIntersecting(int x, int y, int w, int h)
    {
        foreach (var rt in _rts)
            if (rt is { Dirty: true } && rt.Intersects(x, y, w, h)) Writeback(rt);
    }

    void CheckTextureFeedback(in PrimFlags f)
    {
        if (!f.Textured || f.UseImage) return;
        int px = (f.TPage & 0xF) * 64;
        int py = ((f.TPage >> 4) & 1) * 256;
        int depth = (f.TPage >> 7) & 3;
        int pw = depth == 0 ? 64 : depth == 1 ? 128 : 256;
        foreach (var rt in _rts)
            if (rt is { Dirty: true } && rt.Intersects(px, py, pw, 256))
            {
                Flush(FlushReason.TextureFeedback);
                Writeback(rt);
            }
    }

    // 0046. Which state DesiredMatches found different; the first, when several are.
    FlushReason Mismatch(bool transparent, int blend, int image, int zMode)
    {
        if (_kRepTex != _pendingRepTex || _kRepClut != _pendingRepClut
            || (_pendingRepTex != 0 && (_kRepX != _pendingRepX || _kRepY != _pendingRepY
                                        || _kRepW != _pendingRepW || _kRepH != _pendingRepH)))
            return FlushReason.StateReplacement;
        if (_kTransparent != transparent) return FlushReason.StateSemi;
        if (_kBlend != blend) return FlushReason.StateBlend;
        if (_kImage != image) return FlushReason.StateImage;
        if (_kZMode != zMode) return FlushReason.StateDepthMode;
        if (_kSetMask != (_env.SetMask ? 1 : 0) || _kCheckMask != (_env.CheckMask ? 1 : 0)) return FlushReason.StateMask;
        if (_kClipX0 != _env.ClipX0 || _kClipY0 != _env.ClipY0 || _kClipX1 != _env.ClipX1 || _kClipY1 != _env.ClipY1)
            return FlushReason.StateClip;
        return FlushReason.StateTexWindow;
    }

    bool DesiredMatches(bool transparent, int blend, int image, int zMode)
    {
        int twAndX = ~(_env.TwMaskX * 8) & 0xFF, twAndY = ~(_env.TwMaskY * 8) & 0xFF;
        int twOrX = (_env.TwOffX & _env.TwMaskX) * 8, twOrY = (_env.TwOffY & _env.TwMaskY) * 8;
        return _kRepTex == _pendingRepTex && _kRepClut == _pendingRepClut
            && (_pendingRepTex == 0 || (_kRepX == _pendingRepX && _kRepY == _pendingRepY
                                        && _kRepW == _pendingRepW && _kRepH == _pendingRepH))
            && _kTransparent == transparent && _kBlend == blend && _kImage == image
            && _kZMode == zMode
            && _kSetMask == (_env.SetMask ? 1 : 0) && _kCheckMask == (_env.CheckMask ? 1 : 0)
            && _kTwAndX == twAndX && _kTwAndY == twAndY && _kTwOrX == twOrX && _kTwOrY == twOrY
            && _kClipX0 == _env.ClipX0 && _kClipY0 == _env.ClipY0 && _kClipX1 == _env.ClipX1 && _kClipY1 == _env.ClipY1;
    }

    void Begin(in PrimFlags f, int vertsNeeded, int zMode = 0)
    {
        bool transparent = f.SemiTrans;
        int blend = f.BlendMode;
        int image = f.UseImage ? f.Image : -1;
        var target = Classify();
        if (_count > 0)
        {
            if (target != _kTarget) Flush(FlushReason.Target);
            else if (!DesiredMatches(transparent, blend, image, zMode))
                Flush(GpuTrace.Sink != null ? Mismatch(transparent, blend, image, zMode) : FlushReason.Other);
        }
        if (_count + vertsNeeded > MaxVerts) Flush(FlushReason.Full);
        CheckTextureFeedback(f);

        _kTarget = target;
        _kImage = image;
        _kTransparent = transparent; _kBlend = blend;
        _kZMode = zMode;
        _kSetMask = _env.SetMask ? 1 : 0; _kCheckMask = _env.CheckMask ? 1 : 0;
        _kTwAndX = ~(_env.TwMaskX * 8) & 0xFF; _kTwAndY = ~(_env.TwMaskY * 8) & 0xFF;
        _kTwOrX = (_env.TwOffX & _env.TwMaskX) * 8; _kTwOrY = (_env.TwOffY & _env.TwMaskY) * 8;
        _kClipX0 = _env.ClipX0; _kClipY0 = _env.ClipY0; _kClipX1 = _env.ClipX1; _kClipY1 = _env.ClipY1;
        _kRepTex = _pendingRepTex; _kRepClut = _pendingRepClut; _kRepClutCount = _pendingRepClutCount;
        _kRepX = _pendingRepX; _kRepY = _pendingRepY; _kRepW = _pendingRepW; _kRepH = _pendingRepH;
    }

    uint _pendingRepTex, _pendingRepClut;
    int _pendingRepClutCount = 16;
    float _pendingRepX, _pendingRepY, _pendingRepW = 1, _pendingRepH = 1;

    readonly Dictionary<Assets.ReplacementTexture, uint> _repTextures = [];
    readonly Dictionary<Assets.ReplacementClut, uint> _repCluts = [];

    void ResolveReplacement(in PrimFlags f, int uMin, int vMin, int uMax, int vMax)
    {
        _pendingRepTex = 0;
        _pendingRepClut = 0;

        if (!f.Textured || f.UseImage) return;

        int twAndX = ~(_env.TwMaskX * 8) & 0xFF, twAndY = ~(_env.TwMaskY * 8) & 0xFF;
        int twOrX = (_env.TwOffX & _env.TwMaskX) * 8, twOrY = (_env.TwOffY & _env.TwMaskY) * 8;

        if (!Assets.Textures.TextureResolver.Resolve(f.TPage, f.Clut, uMin, vMin, uMax, vMax,
                twAndX, twAndY, twOrX, twOrY, out var res))
            return;

        if (res.Texture is { Mode: Assets.TextureMode.Rgba } tex)
        {
            _pendingRepTex = EnsureRepTexture(tex);
            _pendingRepX = res.Rect.U0;
            _pendingRepY = res.Rect.V0;
            _pendingRepW = res.Rect.W;
            _pendingRepH = res.Rect.H;
        }

        if (_pendingRepTex == 0 && res.Clut is { } clut && res.Rect.ClutCount > 0 && clut.Count == res.Rect.ClutCount)
        {
            _pendingRepClut = EnsureRepClut(clut);
            _pendingRepClutCount = clut.Count;
        }
    }

    unsafe uint EnsureRepTexture(Assets.ReplacementTexture tex)
    {
        if (_repTextures.TryGetValue(tex, out uint handle)) return handle;

        _gl.ActiveTexture(TextureUnit.Texture7);
        handle = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, handle);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)tex.Width, (uint)tex.Height, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, tex.Rgba);
        // 0073. A mip chain always; whether it is read is RepFilter's.
        _gl.GenerateMipmap(TextureTarget.Texture2D);
        _gl.ActiveTexture(TextureUnit.Texture0);

        _repTextures[tex] = handle;
        return handle;
    }

    readonly Dictionary<uint, int> _repFilterKey = [];

    /// <summary>0073. A replacement texture is filtered by the Texture filtering
    /// slider: trilinear with anisotropy to its level while mipmaps are on, bilinear
    /// off. Set on the bound texture when the slider has moved since.</summary>
    void RepFilter(uint tex)
    {
        bool mips = GteDepth.Mipmaps;
        int key = mips ? Math.Clamp(GteDepth.Anisotropy, 1, 16) : 0;
        if (_repFilterKey.TryGetValue(tex, out int had) && had == key) return;
        _repFilterKey[tex] = key;
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter,
            (int)(mips ? GLEnum.LinearMipmapLinear : GLEnum.Linear));
        _gl.TexParameter(TextureTarget.Texture2D, (TextureParameterName)0x84FE, (float)Math.Max(1, key));
        GteDepth.RepFilterSets++;
    }

    unsafe uint EnsureRepClut(Assets.ReplacementClut clut)
    {
        if (_repCluts.TryGetValue(clut, out uint handle)) return handle;

        _gl.ActiveTexture(TextureUnit.Texture7);
        handle = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, handle);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)clut.Count, 1, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, clut.Rgba);
        _gl.ActiveTexture(TextureUnit.Texture0);

        _repCluts[clut] = handle;
        return handle;
    }

    bool DitherOf(in PrimFlags f) => _env.Dither && (f.Gouraud || (f.Textured && !f.RawTexture));

    // Crossing vertices a single display flip must deliver before the target's
    // margin counts as carrying a world. See the latch in V.
    const int MarginVertsToLatch = 32;

    GlVertex V(in HleVertex v, in PrimFlags f, bool dither)
    {
        // The latch records where the game itself drew past its own edge, and it
        // takes a world's worth of crossings in one display flip to do it: a
        // frame of gameplay puts hundreds of vertices out there, while an
        // oversized clear rect -- genuine game output, and the splash draws one
        // every MDEC frame -- contributes two. A primitive the widescreen patch
        // widened crosses the edge by construction and never counts at all.
        if (!GpuHle.PortWidenedPrim && _kTarget is { Margin: > 0 } && (v.X < _env.ClipX0 || v.X > _env.ClipX1))
        {
            if (_kTarget.MarginVertFlip != GpuHle.DisplayFlip)
            {
                _kTarget.MarginVertFlip = GpuHle.DisplayFlip;
                _kTarget.MarginVerts = 0;
            }
            if (++_kTarget.MarginVerts >= MarginVertsToLatch)
                _kTarget.MarginContentFlip = GpuHle.DisplayFlip;
        }
        bool raw = f.Textured && f.RawTexture;
        float cr = raw ? 128f : v.R, cg = raw ? 128f : v.G, cb = raw ? 128f : v.B;
        int tpage = f.UseImage ? 0x4000 : f.Textured ? (f.TPage & 0x1FF) : 0x8000;
        if (dither && _pendingRepTex == 0) tpage |= 0x400;
        if (_pendingRepTex != 0) tpage |= 0x2000;
        else if (_pendingRepClut != 0) tpage |= 0x1000;
        // 0073. Every caller writes `_verts[_count++] = V(...)`, which increments _count
        // before V runs, so `_count == 0` never held here and the bounds grew from
        // boot: an untargeted batch marked VRAM GPU-dirty up to 748x481, and the
        // replacement resolver refused every texture under it. See "Phase 4, the
        // first slice" in docs/REMASTER.md.
        if (_drawEmpty)
        {
            _drawEmpty = false;
            _drawMinX = _drawMaxX = v.X;
            _drawMinY = _drawMaxY = v.Y;
        }
        else
        {
            if (v.X < _drawMinX) _drawMinX = v.X;
            if (v.X > _drawMaxX) _drawMaxX = v.X;
            if (v.Y < _drawMinY) _drawMinY = v.Y;
            if (v.Y > _drawMaxY) _drawMaxY = v.Y;
        }

        return new GlVertex
        {
            X = v.X, Y = v.Y,
            R = cr, G = cg, B = cb,
            Clut = f.Clut & 0x7FFF,
            Texpage = tpage,
            U = v.U, V = v.V,
            W = v.HasPersp && v.Z > 0f ? v.Z : 1f,
            Z = v.HasGteZ && v.Z > 0f ? v.Z : 0f,
        };
    }

    public void DrawTri(in HleVertex a, in HleVertex b, in HleVertex c, in PrimFlags f)
    {
        if (PlanarReflections.Capturing && PlanarDrop()) return;
        // 0073. A replacement is keyed on the face's texture rectangle (0060), not the
        // triangle's: a clipped fan's triangles each cover part of the face, and
        // a key per part is a key the pack never names.
        if (a.HasTexRect && Assets.Textures.TextureResolver.KeyOnFaceRect)
            ResolveReplacement(f, (int)(a.TexRect & 0xFF), (int)((a.TexRect >> 8) & 0xFF),
                (int)((a.TexRect >> 16) & 0xFF), (int)(a.TexRect >> 24));
        else
            ResolveReplacement(f,
                (int)Math.Min(a.U, Math.Min(b.U, c.U)), (int)Math.Min(a.V, Math.Min(b.V, c.V)),
                (int)Math.Max(a.U, Math.Max(b.U, c.U)), (int)Math.Max(a.V, Math.Max(b.V, c.V)));
        // 0 = painter's (2D, or a vertex missed). 1 = opaque 3D, test and write.
        // 2 = semi-transparent 3D, test but leave Z so overlapping additives still
        // blend in table order. 3 = the occlusion pass's mask: an opaque primitive
        // with no recovered depth stamps the far plane, so the HUD, the menus and
        // any triangle the vertex map missed read as "no surface" rather than as
        // the geometry standing behind them. Semi-transparent stays at 0 there,
        // because a death fade or a damage flash is something you see the world
        // *through* and must not erase its depth. 4 = 2 on a packet the port
        // called solid (a blended model, the secret door): the occlusion pass
        // sees all of it.
        int zMode = 0;
        if (a.HasGteZ && b.HasGteZ && c.HasGteZ)
            zMode = !f.SemiTrans ? 1 : a.Solid && GteDepth.SurfacesWanted ? 4 : 2;
        else if (GteDepth.SurfacesWanted && !f.SemiTrans)
            zMode = 3;
        // 0048. A directional triangle needs its batch's BK and LCM to be the ones
        // it was lit with.
        int lightGen = (a.Light & (GteLightMap.Directional << 24)) != 0 ? a.LightGen : -1;
        if (lightGen >= 0 && _kLightGen >= 0 && lightGen != _kLightGen) Flush(FlushReason.StateLight);
        // 0071. A batch is drawn with the light list it was built under.
        if (RemasterUniforms.Generation != _kRemasterGen)
        {
            if (_count > 0) Flush(FlushReason.StateLight);
            _kRemasterGen = RemasterUniforms.Generation;
        }
        Begin(f, 3, zMode);
        // 0058. Keep the triangle for the normal buffer. zMode 1 is exactly the
        // geometry that writes depth and is opaque, which is the geometry the
        // occlusion pass shades; the order it is kept in is the order it is drawn
        // in, and that is what makes the redraw agree with painter's order.
        // 0067. With reflections on, a triangle also carries its material, and a
        // blended one is kept when it has one -- water, which writes no depth and
        // so reaches the surface buffer and nothing else.
        // A 2D primitive (no corner the GTE projected) is kept too, as Overlay, so
        // the reflection pass can tell the HUD from the scene under it.
        if (_kTarget != null && AoGeometry.Active && !_kTarget.IsPlanar)
        {
            byte m = SurfaceMaterial.None;
            if (zMode == 1 || zMode == 4 || zMode == 2)
            {
                m = zMode == 2 ? SurfaceMaterial.None : SurfaceMaterial.Opaque;
                if (GteDepth.Reflections)
                    m = SurfaceMaterial.Classify(a.Material, f.Textured && !f.UseImage, f.SemiTrans && zMode == 2,
                        f.BlendMode, f.TPage,
                        (int)Math.Min(a.U, Math.Min(b.U, c.U)), (int)Math.Min(a.V, Math.Min(b.V, c.V)),
                        (int)Math.Max(a.U, Math.Max(b.U, c.U)), (int)Math.Max(a.V, Math.Max(b.V, c.V)));
            }
            else if (GteDepth.Reflections && !a.Projected && !b.Projected && !c.Projected
                     && !CoversTarget(Math.Min(a.X, Math.Min(b.X, c.X)), Math.Min(a.Y, Math.Min(b.Y, c.Y)),
                                      Math.Max(a.X, Math.Max(b.X, c.X)), Math.Max(a.Y, Math.Max(b.Y, c.Y))))
                m = SurfaceMaterial.Overlay;
            if (m == SurfaceMaterial.Overlay) SurfaceMaterial.Overlays++;
            // 0068. The plane a planar reflection mirrors in is found here, from
            // the water the frame actually drew.
            if (m == SurfaceMaterial.Water && PlanarReflections.Enabled)
            {
                float ox = _kTarget.X + GteDepth.ProjCx, oy = _kTarget.Y + GteDepth.ProjCy;
                PlanarReflections.NoteWater(a.X - ox, a.Y - oy, a.Z, b.X - ox, b.Y - oy, b.Z, c.X - ox, c.Y - oy, c.Z,
                    -GteDepth.ProjCx - _kTarget.Margin, -GteDepth.ProjCy,
                    _kTarget.W - GteDepth.ProjCx + _kTarget.Margin, _kTarget.H - GteDepth.ProjCy);
            }
            if (m != SurfaceMaterial.None)
            {
                // A blended triangle carries 128 over its material (NormalFs).
                float gm = zMode == 2 ? m + SurfaceMaterial.BlendedFlag : m;
                _kTarget.Geo.Frame(_frame, GteDepth.Generation);
                _kTarget.Geo.Add(GeoVert(a, gm), GeoVert(b, gm), GeoVert(c, gm));
            }
        }
        if (lightGen >= 0) _kLightGen = lightGen;
        if (a.Light != 0 && _vboLight != 0)
        {
            if (_litFilled < _count) Array.Clear(_lights, _litFilled, _count - _litFilled);
            _lights[_count] = new GlLight { Lx = a.Lx, Ly = a.Ly, Lz = a.Lz, Fog = a.Fog, Light = a.Light, Mat = a.Material };
            _lights[_count + 1] = new GlLight { Lx = b.Lx, Ly = b.Ly, Lz = b.Lz, Fog = b.Fog, Light = b.Light, Mat = a.Material };
            _lights[_count + 2] = new GlLight { Lx = c.Lx, Ly = c.Ly, Lz = c.Lz, Fog = c.Fog, Light = c.Light, Mat = a.Material };
            _litFilled = _count + 3;
        }
        if (a.HasTexRect && _vboTex != 0 && f.Textured && !f.UseImage)
        {
            if (_texFilled < _count) Array.Clear(_texs, _texFilled, _count - _texFilled);
            var t = new GlTex { Rect = a.TexRect, Entry = 0x80000000u | MipEntry(a, b, c, f) };
            _texs[_count] = t; _texs[_count + 1] = t; _texs[_count + 2] = t;
            _texFilled = _count + 3;
        }
        bool dith = DitherOf(f);
        _verts[_count++] = V(a, f, dith); _verts[_count++] = V(b, f, dith); _verts[_count++] = V(c, f, dith);
    }

    /// <summary>0060. The atlas entry for a textured triangle, or 0: none for a
    /// replacement texture, a texture window, a scrolling texture (its fraction is
    /// the shader's), or a polygon large enough on screen that it is magnified.</summary>
    uint MipEntry(in HleVertex a, in HleVertex b, in HleVertex c, in PrimFlags f)
    {
        if (!GteDepth.Mipmaps || _mip == null || _pendingRepTex != 0 || _pendingRepClut != 0
            || _env.TwMaskX != 0 || _env.TwMaskY != 0)
            return 0;
        uint rect = a.TexRect;
        int u0 = (int)(rect & 0xFF), v0 = (int)((rect >> 8) & 0xFF);
        int w = (int)((rect >> 16) & 0xFF) - u0 + 1, h = (int)(rect >> 24) - v0 + 1;
        // Texels against the pixels of the quad this is half of, at the render scale.
        float area = Math.Abs((b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X));
        int s = GlVram.Scale;
        if ((float)w * h * 32f < area * s * s) return 0;

        int clock = Assets.Textures.VramTracker.Clock;
        if (rect == _mipLastRect && f.TPage == _mipLastTPage && f.Clut == _mipLastClut
            && _frame == _mipLastFrame && clock == _mipLastClock)
            return _mipLastEntry;

        uint entry = FluidOverlap(f.TPage, u0, v0, w, h) ? 0u : _mip.Lookup(f.TPage, f.Clut, rect, _frame);
        _mipLastRect = rect; _mipLastTPage = f.TPage; _mipLastClut = f.Clut;
        _mipLastFrame = _frame; _mipLastClock = Assets.Textures.VramTracker.Clock;
        _mipLastEntry = entry;
        return entry;
    }

    static bool FluidOverlap(int tpage, int u0, int v0, int w, int h)
    {
        if (GteDepth.FluidN == 0) return false;
        int mode = (tpage >> 7) & 3;
        int div = mode == 0 ? 4 : mode == 1 ? 2 : 1;
        float x0 = (tpage & 0xF) * 64 + u0 / div, x1 = (tpage & 0xF) * 64 + (u0 + w - 1) / div + 1;
        float y0 = ((tpage >> 4) & 1) * 256 + v0, y1 = y0 + h;
        for (int i = 0; i < GteDepth.FluidN; i++)
        {
            ref var r = ref GteDepth.Fluid[i];
            if (x0 < r.X + r.W && x1 > r.X && y0 < r.Y + r.H && y1 > r.Y) return true;
        }
        return false;
    }

    /// <summary>0058. A vertex as the normal pass wants it: the position the colour
    /// pass is about to draw, and the view depth the plane is reconstructed from.</summary>
    static AoGeometry.V GeoVert(in HleVertex v, float m) =>
        new() { X = v.X, Y = v.Y, Z = m == SurfaceMaterial.Overlay ? 0f : v.Z, M = m };

    /// <summary>0067. A 2D primitive over most of the target is a fade or a flash
    /// the world is seen through, not a piece of the HUD; it is not an overlay.</summary>
    bool CoversTarget(float x0, float y0, float x1, float y1) =>
        _kTarget != null && x1 - x0 >= _kTarget.W * 0.9f && y1 - y0 >= _kTarget.H * 0.9f;

    /// <summary>The zMode a primitive with no recovered depth takes: 3 (stamp the
    /// far plane) while the occlusion pass is on and the primitive is opaque, 0 --
    /// which is what everything did before this existed -- otherwise.</summary>
    static int FarMask(in PrimFlags f) => GteDepth.SurfacesWanted && !f.SemiTrans ? 3 : 0;

    public void DrawRect(in HleRect r, in PrimFlags f)
    {
        if (PlanarReflections.Capturing && PlanarDrop()) return;
        ResolveReplacement(f, r.U, r.V, r.U + Math.Max(0, r.W - 1), r.V + Math.Max(0, r.H - 1));
        // A sprite never carries a recovered depth, so under the occlusion pass it
        // is the mask (see DrawTri): opaque stamps the far plane, semi-transparent
        // leaves the depth under it alone.
        Begin(f, 6, FarMask(f));
        var a = new HleVertex { X = r.X, Y = r.Y, R = r.R, G = r.G, B = r.B, U = r.U, V = r.V };
        var b = new HleVertex { X = r.X + r.W, Y = r.Y, R = r.R, G = r.G, B = r.B, U = (short)(r.U + r.W), V = r.V };
        var c = new HleVertex { X = r.X, Y = r.Y + r.H, R = r.R, G = r.G, B = r.B, U = r.U, V = (short)(r.V + r.H) };
        var d = new HleVertex { X = r.X + r.W, Y = r.Y + r.H, R = r.R, G = r.G, B = r.B, U = (short)(r.U + r.W), V = (short)(r.V + r.H) };
        _verts[_count++] = V(a, f, false); _verts[_count++] = V(b, f, false); _verts[_count++] = V(c, f, false);
        _verts[_count++] = V(b, f, false); _verts[_count++] = V(d, f, false); _verts[_count++] = V(c, f, false);
        // 0067. A sprite is 2D: the HUD's, as far as the reflection pass is concerned.
        if (GteDepth.Reflections && _kTarget is { IsPlanar: false } && AoGeometry.Active
            && !CoversTarget(r.X, r.Y, r.X + r.W, r.Y + r.H))
        {
            float m = SurfaceMaterial.Overlay;
            SurfaceMaterial.Overlays += 2;
            _kTarget.Geo.Frame(_frame, GteDepth.Generation);
            _kTarget.Geo.Add(GeoVert(a, m), GeoVert(b, m), GeoVert(c, m));
            _kTarget.Geo.Add(GeoVert(b, m), GeoVert(d, m), GeoVert(c, m));
        }
    }

    public void DrawLine(in HleVertex a, in HleVertex b, in PrimFlags f)
    {
        if (PlanarReflections.Capturing && PlanarDrop()) return;
        _pendingRepTex = 0;
        _pendingRepClut = 0;
        Begin(f, 6, FarMask(f));
        bool dith = _env.Dither;
        float x1 = a.X, y1 = a.Y;
        float x2 = b.X, y2 = b.Y;
        float dx = x2 - x1, dy = y2 - y1;

        if (dx == 0 && dy == 0)
        {
            LineVert(x1, y1, a, f, dith); LineVert(x1 + 1, y1, a, f, dith); LineVert(x1 + 1, y1 + 1, a, f, dith);
            LineVert(x1 + 1, y1 + 1, a, f, dith); LineVert(x1, y1 + 1, a, f, dith); LineVert(x1, y1, a, f, dith);
            return;
        }

        float xo, yo;
        if (Math.Abs(dx) > Math.Abs(dy)) { xo = 0; yo = 1; if (dx > 0) x2++; else x1++; }
        else { xo = 1; yo = 0; if (dy > 0) y2++; else y1++; }

        LineVert(x1, y1, a, f, dith); LineVert(x2, y2, b, f, dith); LineVert(x2 + xo, y2 + yo, b, f, dith);
        LineVert(x2 + xo, y2 + yo, b, f, dith); LineVert(x1 + xo, y1 + yo, a, f, dith); LineVert(x1, y1, a, f, dith);
    }

    void LineVert(float x, float y, in HleVertex src, in PrimFlags f, bool dither)
    {
        var v = src; v.X = x; v.Y = y;
        _verts[_count++] = V(v, f, dither);
    }

    public void FillRect(int x, int y, int w, int h, ushort color15)
    {
        Flush(FlushReason.Fill);
        _vram.Fill(x, y, w, h, color15);
        if (_check != null) { _check.Fill(x, y, w, h, color15); _check.Check(_vram, "fill", x, y, w, h, false); }
        foreach (var rt in _rts)
        {
            if (rt == null || !rt.Intersects(x, y, w, h)) continue;
            if (rt.Covers(x, y, x + w - 1, y + h - 1))
            {
                FillRtFull(rt, color15);
                rt.Dirty = false;
                rt.LastDrawFrame = _frame;
                if (rt.Margin > 0) rt.MarginContentFlip = GpuHle.DisplayFlip;
            }
            else SyncRtFromVram(rt, x, y, w, h, fromSample: true);
        }
    }

    void FillRtFull(GlDisplayRt rt, ushort color15)
    {
        float r = (color15 & 0x1F) / 31f, g = ((color15 >> 5) & 0x1F) / 31f, b = ((color15 >> 10) & 0x1F) / 31f;
        float a = (color15 & 0x8000) != 0 ? 1f : 0f;
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, rt.Fbo);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.ClearColor(r, g, b, a);
        _gl.ClearDepth(1.0);
        _gl.DepthMask(true);
        _gl.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);
        rt.ZGen = GteDepth.Generation;
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    // ---- framebuffer readback snapshots -------------------------------------
    // A modal sub-loop -- the in-game menu, a shop, an NPC's message box -- keeps
    // the world behind it by reading the finished frame out of VRAM into system
    // RAM once (StoreImage) and blitting it back at the head of every iteration
    // (LoadImage), so each pass erases the last one's drawing without redrawing
    // the world. That roundtrip goes through the console's own resolution: VRAM
    // is read at 1x whatever the render scale is, so the restore stamped a 1x
    // picture over the display area every frame and every one of those scenes
    // rendered at one sample per game pixel. It shows as exactly the game's own
    // 320 columns dropping to 1x with the widescreen margin -- which never goes
    // through VRAM (Writeback copies a target's middle W columns only) -- staying
    // at full scale beside them.
    //
    // So a readback also takes a *scaled* copy of the region on the GPU, and an
    // upload whose 1x pixels are byte-identical to that readback is served by
    // blitting the copy back rather than by uploading. The key is the content and
    // the size, not the address, because the frame may be restored into either
    // display buffer. Anything the game actually changed in RAM between the read
    // and the write fails the compare and takes the 1x path, which is what every
    // upload did before -- so a texture built in RAM, an MDEC frame or a decoded
    // sprite is untouched.
    sealed class VramSnap
    {
        public int W, H, Scale;
        public uint Tex, Fbo;
        public ushort[] Data = [];
        public long Stamp;
    }

    readonly VramSnap?[] _snaps = new VramSnap?[2];
    long _snapStamp;
    static int _snapHit, _snapMiss;
    bool _snapVerified;
    static double _snapWindow;
    int _lastVerdict = int.MinValue;
    static int _snapMissW, _snapMissH;
    static int _snapRefused, _snapRefusedW, _snapRefusedH;

    // Below this a readback is a sprite or a small tile rather than a frame, and
    // a snapshot of it would evict the one that matters.
    const int SnapMinArea = 64 * 64;

    void SnapProbe()
    {
        if (!GlVram.SnapshotProbe) return;
        double now = Environment.TickCount64 / 1000.0;
        if (now - _snapWindow < 2.0) return;
        Console.WriteLine($"[vramsnap] restored {_snapHit}, uploaded 1x {_snapMiss}" +
                          (_snapMiss > 0 ? $", widest miss {_snapMissW}x{_snapMissH}" : "") +
                          (_snapRefused > 0 ? $", {_snapRefused} readback(s) outside a display target not copied, widest {_snapRefusedW}x{_snapRefusedH}" : ""));
        _snapMissW = _snapMissH = 0;
        _snapRefused = _snapRefusedW = _snapRefusedH = 0;
        _snapHit = _snapMiss = 0;
        _snapWindow = now;
    }

    void SnapTake(int x, int y, int w, int h, ReadOnlySpan<ushort> px)
    {
        if (!GlVram.Snapshots) return;
        int n = w * h;
        if (w <= 0 || h <= 0 || n < SnapMinArea || px.Length < n) return;
        // Neither ReadRect nor WriteRect wraps, so a rect off the end of VRAM is
        // already undefined; do not carry one into a copy that could be restored
        // somewhere else entirely.
        if (x < 0 || y < 0 || x + w > VramShadow.Width || y + h > VramShadow.Height) return;

        // The scaled copy has to come from somewhere that holds these pixels now.
        // Since 0054 an upload reaches 1x sample VRAM and any target it touches, but
        // not the scaled atlas, so outside a display target the atlas is stale and a
        // restore from it wrote old texels over the textures (a shop's walls). A
        // display target is kept current by draws and uploads alike.
        GlDisplayRt? src = null;
        foreach (var rt in _rts)
            if (rt != null && x >= rt.X && y >= rt.Y && x + w <= rt.X + rt.W && y + h <= rt.Y + rt.H) { src = rt; break; }
        if (src == null)
        {
            _snapRefused++;
            if ((long)w * h > (long)_snapRefusedW * _snapRefusedH) { _snapRefusedW = w; _snapRefusedH = h; }
            return;
        }

        int s = GlVram.Scale;
        int slot = -1;
        for (int i = 0; i < _snaps.Length; i++)
            if (_snaps[i] is { } fit && fit.W == w && fit.H == h && fit.Scale == s) { slot = i; break; }
        if (slot < 0)
        {
            slot = 0;
            for (int i = 1; i < _snaps.Length; i++)
            {
                if (_snaps[i] == null) { slot = i; break; }
                if (_snaps[slot] != null && _snaps[i]!.Stamp < _snaps[slot]!.Stamp) slot = i;
            }
        }

        var snap = _snaps[slot];
        if (snap == null || snap.W != w || snap.H != h || snap.Scale != s)
        {
            if (snap != null) SnapDestroy(snap);
            snap = new VramSnap { W = w, H = h, Scale = s, Data = new ushort[n] };
            snap.Tex = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, snap.Tex);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
            _gl.TexImage2D<ushort>(TextureTarget.Texture2D, 0, InternalFormat.Rgb5A1, (uint)(w * s), (uint)(h * s), 0,
                PixelFormat.Rgba, PixelType.UnsignedShort1555Rev, new ushort[(long)w * s * h * s].AsSpan());
            snap.Fbo = _gl.GenFramebuffer();
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, snap.Fbo);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.Texture2D, snap.Tex, 0);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
            _snaps[slot] = snap;
        }

        px[..n].CopyTo(snap.Data);
        snap.Stamp = ++_snapStamp;

        _gl.Disable(EnableCap.ScissorTest);
        int sx = (x - src.X + src.Margin) * s, sy = (y - src.Y) * s;
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, src.Fbo);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, snap.Fbo);
        _gl.BlitFramebuffer(sx, sy, sx + w * s, sy + h * s,
            0, 0, w * s, h * s, ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    bool SnapRestore(int x, int y, int w, int h, ReadOnlySpan<ushort> px)
    {
        if (!GlVram.Snapshots) return false;
        int n = w * h;
        if (w <= 0 || h <= 0 || n < SnapMinArea || px.Length < n) return false;

        int s = GlVram.Scale;
        foreach (var snap in _snaps)
        {
            if (snap is null || snap.W != w || snap.H != h || snap.Scale != s) continue;
            if (!px[..n].SequenceEqual(snap.Data)) continue;

            _gl.Disable(EnableCap.ScissorTest);
            _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, snap.Fbo);
            _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, _vram.Fbo);
            _gl.BlitFramebuffer(0, 0, w * s, h * s,
                x * s, y * s, (x + w) * s, (y + h) * s,
                ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
            snap.Stamp = ++_snapStamp;
            _snapHit++;
            // Counting restores says the path fires, not that it wrote the right
            // pixels. Under the probe the first one is read straight back out of
            // VRAM at 1x and compared against the upload it replaced: a scaled
            // copy of the same frame must downsample to the same picture, so any
            // disagreement is the blit's geometry and not the resolution.
            if (GlVram.SnapshotProbe && !_snapVerified)
            {
                _snapVerified = true;
                var back = new ushort[n];
                _vram.ReadRect(x, y, w, h, back);
                int diff = 0;
                for (int i = 0; i < n; i++) if (back[i] != px[i]) diff++;
                Console.WriteLine($"[vramsnap] verify {w}x{h} at {x},{y}: {diff} of {n} pixels differ " +
                                  $"({diff * 100.0 / n:F2}%)");
            }
            return true;
        }
        _snapMiss++;
        if ((long)w * h > (long)_snapMissW * _snapMissH) { _snapMissW = w; _snapMissH = h; }
        return false;
    }

    void SnapDestroy(VramSnap snap)
    {
        if (snap.Fbo != 0) _gl.DeleteFramebuffer(snap.Fbo);
        if (snap.Tex != 0) _gl.DeleteTexture(snap.Tex);
        snap.Fbo = snap.Tex = 0;
    }

    public void CopyVram(int sx, int sy, int dx, int dy, int w, int h)
    {
        Flush(FlushReason.Copy);
        WritebackDirtyIntersecting(sx, sy, w, h);
        _vram.CopyRect(sx, sy, dx, dy, w, h);
        if (_check != null) { _check.Copy(sx, sy, dx, dy, w, h); _check.Check(_vram, "copy", dx, dy, w, h, false); }
        SyncRtsFromVram(dx, dy, w, h);
    }

    public void WriteVram(int x, int y, int w, int h, ReadOnlySpan<ushort> px)
    {
        Flush(FlushReason.Upload);
        // A restore of a frame this backend read out at scale writes the scaled
        // copy instead of the 1x pixels the game is handing back; everything else
        // uploads as it always did.
        bool restored = SnapRestore(x, y, w, h, px);
        if (restored) _vram.Publish(x, y, w, h);
        else
        {
            _vram.WriteRect(x, y, w, h, px);
            // 0054, amended. The present reads the scaled framebuffer wherever no
            // target serves the display, so an upload no target will present goes
            // there too. "No target at all" was not that: one idle target from the
            // boot clear kept every MDEC frame of the first intro movie off the screen.
            if (!ServedByTarget(x, y, w, h)) _vram.Promote(x, y, w, h);
        }
        if (_check != null) { _check.Upload(x, y, w, h, px); _check.Check(_vram, restored ? "restore" : "upload", x, y, w, h, false); }
        SyncRtsFromVram(x, y, w, h, fromSample: !restored);
        SnapProbe();
    }

    /// <summary>0054, amended. Whether a live display target contains the
    /// rectangle and the present would take it, which is where an upload is
    /// carried by <see cref="SyncRtsFromVram"/> rather than by the scaled
    /// framebuffer.</summary>
    bool ServedByTarget(int x, int y, int w, int h)
    {
        foreach (var rt in _rts)
            if (rt != null && !MarginRefused(rt)
                && x >= rt.X && y >= rt.Y && x + w <= rt.X + rt.W && y + h <= rt.Y + rt.H)
                return true;
        return false;
    }

    static bool MarginRefused(GlDisplayRt rt) => rt is { Margin: > 0, MarginContentFlip: < 0 };

    public void ReadVram(int x, int y, int w, int h, Span<ushort> px) => ReadVram(x, y, w, h, px, true);

    /// <summary>0046. <paramref name="snapshot"/> false reads without offering the
    /// pixels to 0039's scaled restore copies, so a diagnostic read of the whole of
    /// VRAM does not evict the copy a menu is being restored from.</summary>
    public void ReadVram(int x, int y, int w, int h, Span<ushort> px, bool snapshot)
    {
        Flush(FlushReason.Readback);
        WritebackDirtyIntersecting(x, y, w, h);
        _vram.ReadRect(x, y, w, h, px);
        if (snapshot) SnapTake(x, y, w, h, px);
    }

    public int RegisterImage(ReadOnlySpan<byte> rgba, int width, int height)
    {
        _gl.ActiveTexture(TextureUnit.Texture7);
        uint t = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, t);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)width, (uint)height, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, rgba);
        _gl.ActiveTexture(TextureUnit.Texture0);

        _images.Add(t);
        return _images.Count - 1;
    }

    public void Flush() => Flush(FlushReason.Other);

    void Flush(FlushReason why)
    {
        if (_count == 0) return;

        //0046.
        var trace = GpuTrace.Sink;
        trace?.Flush(why, _count);
        //0045.
        var profile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.GlFlush);
        var query = BeginGpuTimer();
        FlushCore();
        EndGpuTimer(query, GpuWork.Batch, 0);
        Diagnostics.Profiler.End(profile);
        trace?.Flushed();
    }

    // 0046. GPU time for a frame capture: a GL_TIME_ELAPSED query around the work,
    // read back frames later with GpuTimeNs. Nothing is queried unless a capture is
    // tracing, and the queries never nest -- a flush, the AO pass and the composite
    // run one after another.
    bool _timerQueries;

    uint BeginGpuTimer()
    {
        if (!_timerQueries || GpuTrace.Sink == null) return 0;
        var q = _gl.GenQuery();
        _gl.BeginQuery(QueryTarget.TimeElapsed, q);
        return q;
    }

    void EndGpuTimer(uint query, GpuWork what, long start)
    {
        if (query != 0) _gl.EndQuery(QueryTarget.TimeElapsed);
        if (GpuTrace.Sink is { } t)
            t.Work(what, start, what == GpuWork.Batch ? 0 : System.Diagnostics.Stopwatch.GetTimestamp(), query);
        else if (query != 0) _gl.DeleteQuery(query);
    }

    /// <summary>0046. A timer query's result in nanoseconds, deleting it; -1 while the
    /// GPU has not finished it.</summary>
    public long GpuTimeNs(uint query)
    {
        if (query == 0) return -1;
        _gl.GetQueryObject(query, QueryObjectParameterName.ResultAvailable, out int ready);
        if (ready == 0) return -1;
        _gl.GetQueryObject(query, QueryObjectParameterName.Result, out long ns);
        _gl.DeleteQuery(query);
        return ns;
    }

    public void DeleteGpuTimer(uint query)
    {
        if (query != 0) _gl.DeleteQuery(query);
    }

    public bool TimerQueries => _timerQueries;

    private void FlushCore()
    {
        // 0060. Decode what this batch's polygons asked the atlas for, from VRAM as
        // they saw it: anything that changes VRAM flushes before it does.
        if (_mip != null && _mip.HasPending) _mip.Process(_vram.SampleTexture);

        var rt = _kTarget;
        uint destTex;
        uint vramTex;
        if (rt == null)
        {
            // 0054. A draw with no display target used to land in the scaled atlas
            // and then Publish that AABB down onto 1x sample VRAM. Uploads no longer
            // reach the atlas, so that copy erased the texture pages -- fetch
            // returned 0, paletted fragments discarded, objects sampled leftover
            // framebuffer texels. Draw into 1x and sample a copy of it.
            vramTex = _vram.BeginSampleRead();
            _vram.BindSampleDraw();
            destTex = _vram.SampleTexture;
        }
        else
        {
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, rt.Fbo);
            _gl.Viewport(0, 0, (uint)rt.TexW, (uint)rt.TexH);
            destTex = rt.Tex;
            vramTex = _vram.SampleTexture;
        }
        int destW = rt == null ? VramShadow.Width : rt.TexW;
        int destH = rt == null ? VramShadow.Height : rt.TexH;

        GpuGlAccess.Gl = _gl;
        GpuGlAccess.TargetFbo = rt == null ? _vram.SampleFbo : rt.Fbo;
        GpuGlAccess.TargetWidth = destW;
        GpuGlAccess.TargetHeight = destH;
        GpuGlAccess.TargetOriginX = rt == null ? 0 : rt.X;
        GpuGlAccess.TargetOriginY = rt == null ? 0 : rt.Y;
        GpuGlAccess.TargetMargin = rt == null ? 0 : rt.Margin;

        _gl.Disable(EnableCap.CullFace);
        _gl.Enable(EnableCap.ScissorTest);
        int s = rt == null ? 1 : GlVram.Scale;

        // Depth test is per-batch: opaque 3D writes, semi-transparent 3D tests
        // without writing, 2D (and everything while the setting is off) keeps
        // the console's painter's algorithm. First draw onto an RT after Present
        // — or after the setting was flipped — clears the attachment so last
        // frame's depths cannot occlude this one. The clear is not gated on this
        // batch's mode, so a 2D primitive arriving first cannot skip it.
        if (rt != null && GteDepth.DepthWanted && (rt.LastDrawFrame != _frame || rt.ZGen != GteDepth.Generation))
        {
            _gl.Disable(EnableCap.ScissorTest);
            _gl.DepthMask(true);
            _gl.ClearDepth(1.0);
            _gl.Clear(ClearBufferMask.DepthBufferBit);
            _gl.Enable(EnableCap.ScissorTest);
            rt.ZGen = GteDepth.Generation;
        }
        if (_kZMode == 3)
        {
            // The far-plane mask: write, never reject. GL_ALWAYS rather than
            // GL_LEQUAL because the far plane loses every LEQUAL test against a
            // surface already stamped there, and this has to overwrite one -- the
            // HUD is drawn over the world, and the point of the mask is that the
            // world's depth under it is gone.
            if (rt is { IsPlanar: false }) _lastZRt = rt;
            _gl.Enable(EnableCap.DepthTest);
            _gl.DepthFunc(DepthFunction.Always);
            _gl.DepthMask(true);
        }
        else if (_kZMode != 0)
        {
            if (rt != null) { GteDepth.ZBatchRt++; if (!rt.IsPlanar) _lastZRt = rt; } else GteDepth.ZBatchVram++;
            _gl.Enable(EnableCap.DepthTest);
            // The two consumers of the attachment differ here and nowhere else.
            // The Z-buffer rejects what the recovered depth says is behind; the
            // occlusion pass wants the buffer filled and the ordering table left
            // in sole charge of what is visible, which is GL_ALWAYS -- and under
            // painter's order the last write at a pixel is then the nearest
            // visible surface, which is exactly the depth the pass reads back.
            _gl.DepthFunc(GteDepth.ZBuffer ? DepthFunction.Lequal : DepthFunction.Always);
            _gl.DepthMask(_kZMode == 1);
        }
        else
        {
            _gl.Disable(EnableCap.DepthTest);
            _gl.DepthMask(false);
        }

        int clipX0, clipY0, clipX1, clipY1;
        if (rt == null)
        {
            clipX0 = _kClipX0; clipY0 = _kClipY0; clipX1 = _kClipX1; clipY1 = _kClipY1;
        }
        else
        {
            clipX0 = _kClipX0 - rt.X + rt.Margin; clipY0 = _kClipY0 - rt.Y;
            clipX1 = _kClipX1 - rt.X + rt.Margin; clipY1 = _kClipY1 - rt.Y;
            if (rt.Margin > 0 && _kClipX0 <= rt.X && _kClipX1 >= rt.X + rt.W - 1) { clipX0 = 0; clipX1 = rt.Wide1x - 1; }
        }

        int bx0 = (int)Math.Floor(_drawMinX) + (rt == null ? 0 : rt.Margin - rt.X);
        int by0 = (int)Math.Floor(_drawMinY) - (rt == null ? 0 : rt.Y);
        int bx1 = (int)Math.Ceiling(_drawMaxX) + (rt == null ? 0 : rt.Margin - rt.X);
        int by1 = (int)Math.Ceiling(_drawMaxY) - (rt == null ? 0 : rt.Y);

        int rx0 = Math.Max(clipX0, bx0), ry0 = Math.Max(clipY0, by0);
        int rx1 = Math.Min(clipX1, bx1), ry1 = Math.Min(clipY1, by1);

        _gl.Scissor(clipX0 * s, clipY0 * s,
            (uint)Math.Max(0, (clipX1 - clipX0 + 1) * s), (uint)Math.Max(0, (clipY1 - clipY0 + 1) * s));

        int readX = Math.Max(0, rx0 * s);
        int readY = Math.Max(0, ry0 * s);
        int readW = Math.Max(0, (rx1 - rx0 + 1) * s);
        int readH = Math.Max(0, (ry1 - ry0 + 1) * s);
        // Dest is sampled only for the mask bit, and on the 2.1 path for blend.
        // Copying or barrier-ing it on every batch was waiting on the scaled
        // atlas after a texture upload -- the 0.7 ms Flush. A draw into 1x VRAM
        // already isolated its sample source, so dest can reuse that copy.
        bool sampleDest = _kCheckMask != 0 || (_legacy && _kTransparent) || (_kTransparent && _kBlend == 2);
        if (sampleDest)
            destTex = rt == null ? vramTex : _vram.BeginDestRead(destTex, destW, destH, readX, readY, readW, readH);
        else
            destTex = vramTex;
        RebindTarget(rt);

        _gl.UseProgram(_progPrim);
        if (s != _primScaleSent)
        {
            // 0055, amended. `_legacy ? (float)s : s` is a float either way, which
            // the core shader's int uScale refuses.
            if (_uPrimScale >= 0)
            {
                if (_legacy) _gl.Uniform1(_uPrimScale, (float)s);
                else _gl.Uniform1(_uPrimScale, s);
            }
            _primScaleSent = s;
        }
        _gl.BindVertexArray(_vao);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, vramTex);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, destTex);
        if (_kImage >= 0 && _kImage < _images.Count)
        {
            _gl.ActiveTexture(TextureUnit.Texture2);
            _gl.BindTexture(TextureTarget.Texture2D, _images[_kImage]);
        }
        if (_kRepTex != 0)
        {
            _gl.ActiveTexture(TextureUnit.Texture3);
            _gl.BindTexture(TextureTarget.Texture2D, _kRepTex);
            RepFilter(_kRepTex);
            _gl.Uniform4(_uRepRect, _kRepX, _kRepY, _kRepW, _kRepH);
        }
        if (_kRepClut != 0)
        {
            _gl.ActiveTexture(TextureUnit.Texture4);
            _gl.BindTexture(TextureTarget.Texture2D, _kRepClut);
            _gl.Uniform1(_uRepClutCount, (float)_kRepClutCount);
        }
        _gl.ActiveTexture(TextureUnit.Texture0);
        if (rt != null)
        {
            _gl.Uniform2(_uPosBias, (float)(rt.Margin - rt.X), (float)(-rt.Y));
            _gl.Uniform2(_uFbInv, 2f / rt.Wide1x, 2f / rt.H);
        }
        else
        {
            _gl.Uniform2(_uPosBias, 0f, 0f);
            _gl.Uniform2(_uFbInv, 2f / VramShadow.Width, 2f / VramShadow.Height);
        }
        if (_uTrueColor >= 0) _gl.Uniform1(_uTrueColor, GteDepth.TrueColor ? 1f : 0f);
        // 0068. Drawing into a planar texture: nothing on the camera's side of the
        // water, which from under it is the pool's own floor and walls.
        int clipOn = rt is { IsPlanar: true } ? 1 : 0;
        if (_uClipOn >= 0 && (clipOn != 0 || _clipOnSent != 0))
        {
            if (clipOn != _clipOnSent) _gl.Uniform1(_uClipOn, clipOn);
            _clipOnSent = clipOn;
            if (clipOn != 0)
            {
                var cp = rt!.ClipPlane;
                _gl.Uniform4(_uClipPlane, cp[0], cp[1], cp[2], cp[3]);
                _gl.Uniform2(_uClipCentre, GteDepth.ProjCx + rt.Margin, GteDepth.ProjCy);
                _gl.Uniform1(_uClipH, Math.Max(1f, GteDepth.ProjH));
            }
        }
        // 0071. The light list, sent when the port publishes a new one. Not into
        // VRAM or a planar texture, whose view is not the one the lights are in.
        int lightN = RemasterUniforms.Active && rt is { IsPlanar: false } ? RemasterUniforms.LightCount : 0;
        if (_uLightN >= 0 && (lightN != 0 || _lightNSent != 0))
        {
            if (lightN != _lightNSent) _gl.Uniform1(_uLightN, lightN);
            _lightNSent = lightN;
            if (lightN != 0)
            {
                if (_lightsSentGen != RemasterUniforms.Generation)
                {
                    _gl.Uniform4(_uLightPos, (uint)lightN, new ReadOnlySpan<float>(RemasterUniforms.LightPos, 0, lightN * 4));
                    _gl.Uniform4(_uLightCol, (uint)lightN, new ReadOnlySpan<float>(RemasterUniforms.LightCol, 0, lightN * 4));
                    _gl.Uniform4(_uLightDir, (uint)lightN, new ReadOnlySpan<float>(RemasterUniforms.LightDir, 0, lightN * 4));
                    _lightsSentGen = RemasterUniforms.Generation;
                    RemasterUniforms.Uploads++;
                }
                _gl.Uniform2(_uLightCentre, GteDepth.ProjCx + rt!.Margin, GteDepth.ProjCy);
                _gl.Uniform1(_uLightH, Math.Max(1f, GteDepth.ProjH));
                RemasterUniforms.LitBatches++;
            }
        }
        // 0071. A batch with light records glows where its material says so.
        // A highlight needs a light to come from.
        int emit = (SurfaceMaterial.AnyEmissive || (SurfaceMaterial.AnySpecular && lightN != 0))
                   && _litFilled > 0 && _uEmitOn >= 0 ? 1 : 0;
        if (emit != 0) BindMaterials();
        if (_uEmitOn >= 0 && emit != _emitOnSent)
        {
            _gl.Uniform1(_uEmitOn, emit);
            _emitOnSent = emit;
        }
        // A plain uniform the next batch reads: unlike true color, changing the
        // anisotropy rebuilds nothing.
        GteDepth.AnisotropyLive = _uAniso >= 0;
        if (_uAniso >= 0) _gl.Uniform1(_uAniso, (float)GteDepth.Anisotropy);
        if (_uMipOn >= 0) _gl.Uniform1(_uMipOn, GteDepth.Mipmaps && _mip != null ? 1f : 0f);
        if (_mip != null && _texFilled > 0)
        {
            _gl.ActiveTexture(TextureUnit.Texture5);
            _gl.BindTexture(TextureTarget.Texture2D, _mip.Texture);
            _gl.ActiveTexture(TextureUnit.Texture0);
        }
        GteDepth.FluidLive = _uFluidN >= 0;
        if (_uFluidN >= 0 && GteDepth.FluidN != _fluidSentN)
        {
            _gl.Uniform1(_uFluidN, (float)GteDepth.FluidN);
            _fluidSentN = GteDepth.FluidN;
        }
        for (int i = 0; i < GteDepth.FluidN; i++)
        {
            ref var slot = ref GteDepth.Fluid[i];
            ref var sent = ref _fluidSent[i];
            if (slot.X != sent.X || slot.Y != sent.Y || slot.W != sent.W || slot.H != sent.H)
                if (_uFluidRect[i] >= 0) _gl.Uniform4(_uFluidRect[i], slot.X, slot.Y, slot.W, slot.H);
            if (slot.Off != sent.Off && _uFluidOff[i] >= 0) _gl.Uniform1(_uFluidOff[i], slot.Off);
            sent = slot;
        }
        if (_kLightGen >= 0 && _uLightBk >= 0)
        {
            int g = _kLightGen;
            _gl.Uniform3(_uLightBk, (float)GteLightMap.Bk(g, 0), GteLightMap.Bk(g, 1), GteLightMap.Bk(g, 2));
            _gl.Uniform3(_uLcmR, (float)GteLightMap.LcmAt(g, 0), GteLightMap.LcmAt(g, 1), GteLightMap.LcmAt(g, 2));
            _gl.Uniform3(_uLcmG, (float)GteLightMap.LcmAt(g, 3), GteLightMap.LcmAt(g, 4), GteLightMap.LcmAt(g, 5));
            _gl.Uniform3(_uLcmB, (float)GteLightMap.LcmAt(g, 6), GteLightMap.LcmAt(g, 7), GteLightMap.LcmAt(g, 8));
            _kLightGen = -1;
        }
        if (_legacy)
        {
            _gl.Uniform4(_uTexWindow, (float)_kTwAndX, _kTwAndY, _kTwOrX, _kTwOrY);
            _gl.Uniform1(_uSetMask, _kSetMask == 1 ? 1f : 0f);
            _gl.Uniform1(_uCheckMask, _kCheckMask == 1 ? 1f : 0f);
            if (_uDestSize >= 0) _gl.Uniform2(_uDestSize, (float)destW, destH);
            if (_uSemiTrans >= 0) _gl.Uniform1(_uSemiTrans, _kTransparent ? 1f : 0f);
            if (_uBlendMode >= 0) _gl.Uniform1(_uBlendMode, (float)_kBlend);
        }
        else
        {
            _gl.Uniform4(_uTexWindow, _kTwAndX, _kTwAndY, _kTwOrX, _kTwOrY);
            _gl.Uniform1(_uSetMask, _kSetMask == 1 ? 1f : 0f);
            _gl.Uniform1(_uCheckMask, _kCheckMask);
            _gl.Uniform4(_uBlendOpaque, 1f, 1f, 1f, 0f);
        }

        if (_vboCursor + _count > MaxVerts)
        {
            Orphan(_vbo, MaxVerts * Unsafe.SizeOf<GlVertex>());
            if (_vboLight != 0) Orphan(_vboLight, MaxVerts * Unsafe.SizeOf<GlLight>());
            if (_vboTex != 0) Orphan(_vboTex, MaxVerts * Unsafe.SizeOf<GlTex>());
            _vboCursor = 0;
        }
        int first = _vboCursor;
        _vboCursor += _count;
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vbo);
        _gl.BufferSubData<GlVertex>(BufferTargetARB.ArrayBuffer, first * Unsafe.SizeOf<GlVertex>(), _verts.AsSpan(0, _count));
        if (_litFilled > 0)
        {
            if (_litFilled < _count) Array.Clear(_lights, _litFilled, _count - _litFilled);
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vboLight);
            _gl.BufferSubData<GlLight>(BufferTargetARB.ArrayBuffer, first * Unsafe.SizeOf<GlLight>(), _lights.AsSpan(0, _count));
        }
        if ((_litFilled > 0) != _lightAttribs)
        {
            _lightAttribs = _litFilled > 0;
            foreach (uint i in LightAttribs)
                if (_lightAttribs) _gl.EnableVertexAttribArray(i); else _gl.DisableVertexAttribArray(i);
            if (!_lightAttribs) LightDefaults();
        }
        if (_texFilled > 0)
        {
            if (_texFilled < _count) Array.Clear(_texs, _texFilled, _count - _texFilled);
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vboTex);
            _gl.BufferSubData<GlTex>(BufferTargetARB.ArrayBuffer, first * Unsafe.SizeOf<GlTex>(), _texs.AsSpan(0, _count));
        }
        if ((_texFilled > 0) != _texAttribs)
        {
            _texAttribs = _texFilled > 0;
            if (_texAttribs) _gl.EnableVertexAttribArray(10); else { _gl.DisableVertexAttribArray(10); TexDefaults(); }
        }

        // 0051. A tested batch draws against a depth pulled towards the camera, so a
        // coplanar overlap goes to the later table entry. An opaque one first writes
        // its true depths with colour off, which keeps crossings inside the batch
        // resolved, then draws colour without writing.
        bool zBias = GteDepth.ZBuffer && (_kZMode == 1 || _kZMode == 2 || _kZMode == 4)
                  && (GteDepth.DepthBias > 0f || GteDepth.DepthSlope > 0f);
        SetDepthBias(zBias);
        if (zBias && _kZMode == 1)
        {
            SetDepthBias(false);
            _gl.Disable(EnableCap.Blend);
            _gl.ColorMask(false, false, false, false);
            _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
            _gl.ColorMask(true, true, true, true);
            _gl.DepthMask(false);
            SetDepthBias(true);
            GteDepth.ZPrepasses++;
        }

        if (_legacy)
        {
            _gl.Disable(EnableCap.Blend);
            _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
        }
        else if (!_kTransparent)
        {
            _gl.Disable(EnableCap.Blend);
            _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
        }
        else
        {
            _gl.Enable(EnableCap.Blend);
            _gl.BlendFuncSeparate(BlendingFactor.Src1Color, BlendingFactor.Src1Alpha, BlendingFactor.One, BlendingFactor.Zero);
            if (_kBlend == 2)
            {
                _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
                SetBlend(0f, 1f);
                _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);

                _vram.BeginDestRead(destTex, destW, destH, readX, readY, readW, readH);
                RebindTarget(rt);
                _gl.BlendEquationSeparate(BlendEquationModeEXT.FuncReverseSubtract, BlendEquationModeEXT.FuncAdd);
                SetBlend(1f, 1f);
                _gl.Uniform4(_uBlendOpaque, 0f, 0f, 0f, 1f);
                _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
            }
            else
            {
                _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
                SetBlend(_kBlend switch { 0 => 0.5f, 3 => 0.25f, _ => 1f }, _kBlend == 0 ? 0.5f : 1f);
                _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
            }

            // Texels without the semi-transparency bit draw opaque, so they must hide what is behind them from the occlusion pass.
            // A solid packet (zMode 4) hides it with every texel.
            if (GteDepth.SurfacesWanted && _uOpaqueDepth >= 0)
            {
                _gl.Disable(EnableCap.Blend);
                _gl.ColorMask(false, false, false, false);
                _gl.Enable(EnableCap.DepthTest);
                _gl.DepthFunc(GteDepth.ZBuffer && (_kZMode == 2 || _kZMode == 4) ? DepthFunction.Lequal : DepthFunction.Always);
                _gl.DepthMask(true);
                _gl.Uniform1(_uOpaqueDepth, _kZMode == 4 ? 2 : 1);
                SetDepthBias(false);
                _gl.DrawArrays(PrimitiveType.Triangles, first, (uint)_count);
                _gl.Uniform1(_uOpaqueDepth, 0);
                _gl.ColorMask(true, true, true, true);
            }
        }

        _gl.Disable(EnableCap.ScissorTest);
        if (rt != null)
        {
            rt.Dirty = true;
            rt.LastDrawFrame = _frame;
            if (!rt.IsPlanar) rt.RetainedSerial = RetainedScene.Serial;
        }
        else
        {
            int x0 = Math.Max(_kClipX0, (int)Math.Floor(_drawMinX));
            int y0 = Math.Max(_kClipY0, (int)Math.Floor(_drawMinY));
            int x1 = Math.Min(_kClipX1, (int)Math.Ceiling(_drawMaxX));
            int y1 = Math.Min(_kClipY1, (int)Math.Ceiling(_drawMaxY));
            if (x1 >= x0 && y1 >= y0)
            {
                Assets.Textures.VramTracker.MarkGpuWrite(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                _vram.CommitDraw(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                _vram.Promote(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                _check?.Check(_vram, "draw", x0, y0, x1 - x0 + 1, y1 - y0 + 1, true);
            }
        }
        _count = 0;
        _drawEmpty = true;
        _litFilled = 0;
        _texFilled = 0;
    }

    unsafe void Orphan(uint buffer, int bytes)
    {
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, buffer);
        _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)bytes, null, BufferUsageARB.DynamicDraw);
    }

    void SetBlend(float src, float dst) => _gl.Uniform4(_uBlend, src, src, src, dst);

    // What a disabled 0048/0060 attribute reads: no record. Each value is given in
    // its input's own type -- `inLight` and `inTex` are integers, and GL's initial
    // generic value is the float (0,0,0,1), which an integer input reads as
    // undefined -- and is set again whenever the arrays go off, because the
    // current value of an attribute whose array was enabled for a draw is not
    // guaranteed to survive it.
    void LightDefaults()
    {
        _gl.VertexAttrib4(7, 0f, 0f, 0f, 1f);
        _gl.VertexAttrib4(8, 0f, 0f, 0f, 1f);
        _gl.VertexAttribI4(9, 0u, 0u, 0u, 0u);
        _gl.VertexAttribI4(11, 0u, 0u, 0u, 0u);
    }

    static readonly uint[] LightAttribs = [7, 8, 9, 11];

    void TexDefaults() => _gl.VertexAttribI4(10, 0u, 0u, 0u, 0u);

    /// <summary>0067. The texture unit SurfaceMaterial's table is bound to, for the prim
    /// and reflection shaders both.</summary>
    const int MatUnit = 6;
    uint _matTex;
    int _matGen = -1;
    const int MatRows = 3;
    readonly float[] _matRows = new float[SurfaceMaterial.Count * 4 * MatRows];

    /// <summary>0067. SurfaceMaterial's table as a 256x2 texture, uploaded when the port
    /// changes it, and bound. Row 0: reflectivity, F0, roughness. Row 1: the emissive
    /// colour (0071).</summary>
    unsafe void BindMaterials()
    {
        if (_legacy) return;
        if (_matTex == 0)
        {
            _matTex = _gl.GenTexture();
            _gl.ActiveTexture(TextureUnit.Texture0 + MatUnit);
            _gl.BindTexture(TextureTarget.Texture2D, _matTex);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba32f, (uint)SurfaceMaterial.Count, MatRows, 0,
                PixelFormat.Rgba, PixelType.Float, null);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)TextureMinFilter.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMaxLevel, 0);
            _matGen = -1;
        }
        _gl.ActiveTexture(TextureUnit.Texture0 + MatUnit);
        _gl.BindTexture(TextureTarget.Texture2D, _matTex);
        if (_matGen != SurfaceMaterial.Generation)
        {
            const int n = SurfaceMaterial.Count;
            for (int i = 0; i < n; i++)
            {
                _matRows[i * 4] = SurfaceMaterial.Reflectivity[i];
                _matRows[i * 4 + 1] = SurfaceMaterial.F0[i];
                _matRows[i * 4 + 2] = SurfaceMaterial.Roughness[i];
                _matRows[i * 4 + 3] = SurfaceMaterial.Metalness[i];
                _matRows[(n + i) * 4] = SurfaceMaterial.Emissive[i * 3];
                _matRows[(n + i) * 4 + 1] = SurfaceMaterial.Emissive[i * 3 + 1];
                _matRows[(n + i) * 4 + 2] = SurfaceMaterial.Emissive[i * 3 + 2];
                // Flags: 1 additive, 2 unfogged.
                _matRows[(n + i) * 4 + 3] = (SurfaceMaterial.EmissiveAdditive[i] ? 1f : 0f)
                                          + (SurfaceMaterial.EmissiveUnfogged[i] ? 2f : 0f);
                _matRows[(2 * n + i) * 4] = SurfaceMaterial.Specular[i];
                _matRows[(2 * n + i) * 4 + 1] = 1f - SurfaceMaterial.Occlusion[i];
                _matRows[(2 * n + i) * 4 + 2] = 0f;
                _matRows[(2 * n + i) * 4 + 3] = 0f;
            }
            fixed (float* p = _matRows)
                _gl.TexSubImage2D(TextureTarget.Texture2D, 0, 0, 0, (uint)n, MatRows, PixelFormat.Rgba, PixelType.Float, p);
            _matGen = SurfaceMaterial.Generation;
            SurfaceMaterial.Uploads++;
        }
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    void SetDepthBias(bool on)
    {
        if (_uDepthBias >= 0) _gl.Uniform1(_uDepthBias, on ? GteDepth.DepthBias / 65536f : 0f);
        if (_uDepthSlope >= 0) _gl.Uniform1(_uDepthSlope, on ? GteDepth.DepthSlope : 0f);
    }

    void SetScaleUniform(uint prog) => SetScaleUniform(prog, GlVram.Scale);

    void SetScaleUniform(uint prog, int scale)
    {
        int loc = _gl.GetUniformLocation(prog, "uScale");
        if (loc < 0) return;
        if (_legacy) _gl.Uniform1(loc, (float)scale);
        else _gl.Uniform1(loc, scale);
    }

    /// <summary>Read the finished frame's depth attachment back and hand it to
    /// GteDepth to reduce. Diagnostic only, one frame when asked: it stalls the
    /// pipeline, and it is the only way to see what the Z-buffer actually decided
    /// rather than what the submission order suggests it should have.</summary>
    unsafe void CaptureDepthMap(GlDisplayRt rt)
    {
        int w = rt.TexW, h = rt.TexH;
        if (w <= 0 || h <= 0) return;

        var buf = new float[(long)w * h];
        _gl.Disable(EnableCap.ScissorTest);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, rt.Fbo);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 4);
        fixed (float* p = buf)
            _gl.ReadPixels(0, 0, (uint)w, (uint)h, PixelFormat.DepthComponent, PixelType.Float, p);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);

        GteDepth.SetDepthMap(buf, w, h);
    }

    void RebindTarget(GlDisplayRt? rt)
    {
        if (rt == null) _vram.BindSampleDraw();
        else
        {
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, rt.Fbo);
            _gl.Viewport(0, 0, (uint)rt.TexW, (uint)rt.TexH);
        }
    }

    public void ClearMarginLatches()
    {
        foreach (var t in _rts)
        {
            if (t == null) continue;
            t.MarginContentFlip = -1000;
            t.MarginVerts = 0;
            t.MarginVertFlip = -1;
        }
    }

    public void Present(in HleDispEnv disp) => PresentDisplay(disp.X, disp.Y, disp.W, disp.H, disp.Rgb24);

    public unsafe (uint tex, int w, int h, float aspect) PresentDisplay(int dispX, int dispY, int w, int h, bool rgb24 = false, int outW = 0, int outH = 0)
    {
        if (!Ready || w <= 0 || h <= 0) return (0, 0, 0, GpuHle.OutputAspect);
        long presentStart = System.Diagnostics.Stopwatch.GetTimestamp();
        // Flush before advancing the counter. The depth clear keys on
        // LastDrawFrame != _frame, so bumping the frame first makes this trailing
        // flush — the tail of the frame that is ending — look like the head of the
        // next one: it clears the depth buffer and stamps the new frame number, so
        // the next frame's real first draw skips its clear and inherits whatever
        // this last batch wrote.
        Flush(FlushReason.Present);
        _frame++;

        // True color was toggled: the live targets have the wrong pixel format.
        // Flush already drained this frame's batch, so no draw is mid-flight; write
        // each target's content back to VRAM and drop it. The next draw recreates
        // it in the new format and re-syncs from VRAM, and this present falls back
        // to VRAM (which just received the writeback) for the one transition frame.
        if (_rtsTrueColor != GteDepth.TrueColor)
        {
            _rtsTrueColor = GteDepth.TrueColor;
            for (int i = 0; i < _rts.Length; i++)
                if (_rts[i] is { } rt)
                {
                    if (rt.Dirty) Writeback(rt);
                    rt.Destroy(_gl);
                    _rts[i] = null;
                }
            _kTarget = null;
            _lastZRt = null;
        }

        for (int i = 0; i < _rts.Length; i++)
        {
            if (_rts[i] is not { } rt) continue;
            if (rt.Dirty) Writeback(rt);
            if (_frame - rt.LastDrawFrame > 300)
            {
                rt.Destroy(_gl);
                _rts[i] = null;
            }
        }

        GlDisplayRt? src = null;
        if (!rgb24)
            foreach (var rt in _rts)
            {
                // No freshness gate on the present counter: the host can present
                // many times between two drawn frames -- VSync off, or a monitor
                // refresh the game's 30 fps cannot match -- and a count-based gate
                // then rejects both targets and drops the picture to the plain
                // VRAM texture at 4:3, which is the wide margins flashing black.
                // Writeback above copies every dirty target's middle columns into
                // VRAM on every present and WriteVram syncs direct writes back
                // into targets, so a target containing the display area is never
                // staler than the fallback it replaces; idle targets are
                // destroyed at 300 frames below.
                if (rt == null) continue;
                if (dispX < rt.X || dispY < rt.Y || dispX + w > rt.X + rt.W || dispY + h > rt.Y + rt.H) continue;
                if (src == null || rt.LastDrawFrame > src.LastDrawFrame) src = rt;
            }
        // A wide target whose margin columns no scene has ever drawn would present
        // invented picture at the sides -- the boot splash, whose frames arrive by
        // MDEC, flapped between such a target and the 4:3 fallback. Refuse targets
        // that never latched margin content; a target that did keeps serving, which
        // is what keeps the in-game menu, dialogs, shops and signs wide instead of
        // collapsing to the 320-wide 4:3 fallback the moment the world render stops.
        bool latchRefused = src != null && MarginRefused(src);
        if (latchRefused)
            src = null;
        // KF2_PRESENT_PROBE=2: the census cannot say *why* a present dropped to the
        // 4:3 fallback -- no target covered the display area, the margin latch
        // refused the one that did, or the target has no margin at all. Print the
        // display rect and every live target the frame a verdict changes, which is
        // the only frame that carries the answer.
        if (GpuHle.PresentVerdictProbe && !rgb24)
        {
            int verdict = src is { Margin: > 0 } ? 2 : src != null ? 1 : latchRefused ? -1 : 0;
            if (verdict != _lastVerdict)
            {
                _lastVerdict = verdict;
                string name = verdict switch
                {
                    2 => "wide", 1 => "plain (margin 0)",
                    -1 => "vram fallback (margin latch refused)",
                    _ => "vram fallback (no target covers the display)"
                };
                var sb = new System.Text.StringBuilder();
                sb.Append($"[present] -> {name}; display {w}x{h} at {dispX},{dispY}, frame {_frame}");
                for (int i = 0; i < _rts.Length; i++)
                {
                    if (_rts[i] is not { } t) { sb.Append($"; rt{i} none"); continue; }
                    sb.Append($"; rt{i} {t.W}x{t.H} at {t.X},{t.Y} margin {t.Margin} latch {t.MarginContentFlip} idle {_frame - t.LastDrawFrame}");
                }
                Console.WriteLine(sb.ToString());
            }
        }
        // Only the non-rgb24 path searches for a target, so src is meaningful only
        // there; an rgb24 present (FMV) draws raw VRAM by a different route and would
        // otherwise be miscounted as the 4:3 margin fallback the census is watching for.
        if (GpuHle.PresentProbe && !rgb24)
        {
            if (src is { Margin: > 0 }) GpuHle.PresentWide++;
            else if (src != null) GpuHle.PresentPlain++;
            else GpuHle.PresentFallback++;
            double now = Environment.TickCount64 / 1000.0;
            if (now - GpuHle.PresentWindowStart >= 2.0)
            {
                Console.WriteLine($"[present] wide {GpuHle.PresentWide}, plain {GpuHle.PresentPlain}, " +
                                  $"vram fallback {GpuHle.PresentFallback}");
                GpuHle.PresentWide = GpuHle.PresentPlain = GpuHle.PresentFallback = 0;
                GpuHle.PresentWindowStart = now;
            }
        }

        // The target the frame's depth batches actually went to — not the one being
        // presented (with two buffers that is last frame's) and not the most
        // recently drawn (a full-screen fill stamps LastDrawFrame too, so that can
        // be a buffer which was just cleared).
        // A target that has since been destroyed has Fbo 0, which is the *default*
        // framebuffer — reading that would report an empty depth buffer rather than
        // no answer. Leave the request standing and try the next frame instead.
        if (GteDepth.WantDepthMap && _lastZRt is { Fbo: not 0 }) CaptureDepthMap(_lastZRt);

        int w1x = src != null ? w + src.Margin * 2 : w;
        int h1x = h;
        float aspect = src is { Margin: > 0 } ? GpuHle.WideAspect : src != null ? GpuHle.SourceAspect : GpuHle.OutputAspect;


        GpuHle.LastDisplayW = w;
        GpuHle.LastDisplayH = h;

        int presentScale = GlVram.Scale;
        int fbW = w1x * presentScale;
        int fbH = h1x * presentScale;
        EnsurePresentSize(fbW, fbH, GlVram.Scale == 1);

        // Ambient occlusion, between the finished target and the present blit. It
        // reads that target's depth attachment and writes only its own texture, so
        // nothing the game can read back -- VRAM, the display buffers, the frame a
        // menu restores -- carries the shading. Present is where it has to happen:
        // the pass needs the *whole* frame's depth, and painter's order means that
        // does not exist until the last primitive has been drawn.
        bool aoOn = AoReady(src, rgb24);
        bool ssrOn = SsrReady(src, rgb24);
        // 0058, 0067. The frame's geometry is redrawn once into the normal and
        // surface buffers both passes read, timed with whichever pass runs first --
        // the occlusion pass's, when it runs, as it always was.
        bool surfaces = false;
        int gScale = Math.Max(aoOn ? AoScale : 1, ssrOn ? SsrScale : 1);
        if (aoOn)
        {
            var aoProfile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Ao);
            long aoStart = System.Diagnostics.Stopwatch.GetTimestamp();
            var aoQuery = BeginGpuTimer();
            surfaces = DrawSurfaces(src!, gScale);
            RunAo(src!, dispX - src!.X, dispY - src.Y, w1x, h1x, fbW, fbH, surfaces && GteDepth.AoNormals);
            EndGpuTimer(aoQuery, GpuWork.AmbientOcclusion, aoStart);
            Diagnostics.Profiler.End(aoProfile);
        }
        else if (GteDepth.AmbientOcclusion && !rgb24)
            GteDepth.AoNoTarget++;
        if (ssrOn)
        {
            var ssrProfile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Ssr);
            long ssrStart = System.Diagnostics.Stopwatch.GetTimestamp();
            var ssrQuery = BeginGpuTimer();
            if (!aoOn) surfaces = DrawSurfaces(src!, gScale);
            // A surface buffer that was not drawn this present holds another frame's.
            ssrOn = surfaces && src!.Surface != 0;
            if (ssrOn)
            {
                DrawRetained(src!);
                RunSsr(src!, dispX - src!.X, dispY - src.Y, w1x, h1x);
            }
            EndGpuTimer(ssrQuery, GpuWork.Reflections, ssrStart);
            Diagnostics.Profiler.End(ssrProfile);
        }
        else if (GteDepth.Reflections && !rgb24)
            ScreenReflections.NoTarget++;

        var compProfile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Composite);
        long compStart = System.Diagnostics.Stopwatch.GetTimestamp();
        var compQuery = BeginGpuTimer();
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _presentFbo);
        _gl.Viewport(0, 0, (uint)fbW, (uint)fbH);
        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);

        _gl.UseProgram(rgb24 ? _progPresent24 : _progPresent);
        _gl.BindVertexArray(_presentVao);
        if (!rgb24 && _uPresentAoOn >= 0)
        {
            _gl.Uniform1(_uPresentAoOn, aoOn ? 1f : 0f);
            if (aoOn)
            {
                _gl.ActiveTexture(TextureUnit.Texture1);
                _gl.BindTexture(TextureTarget.Texture2D, _aoBlurTex);
            }
        }
        if (!rgb24 && _uPresentAoMatOn >= 0)
        {
            bool aoMat = aoOn && surfaces && SurfaceMaterial.AnyOcclusion && src!.Surface != 0;
            _gl.Uniform1(_uPresentAoMatOn, aoMat ? 1f : 0f);
            if (aoMat)
            {
                BindMaterials();
                _gl.UseProgram(_progPresent);
                _gl.ActiveTexture(TextureUnit.Texture3);
                _gl.BindTexture(TextureTarget.Texture2D, src!.Surface);
            }
        }
        if (!rgb24 && _uPresentSsrOn >= 0)
        {
            _gl.Uniform1(_uPresentSsrOn, ssrOn ? 1f : 0f);
            if (ssrOn)
            {
                _gl.ActiveTexture(TextureUnit.Texture2);
                _gl.BindTexture(TextureTarget.Texture2D, _ssrTex);
            }
        }
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, src?.Tex ?? _vram.Texture);
        if (rgb24)
        {
            _gl.Uniform2(_uPresent24Origin, (float)dispX, dispY);
            _gl.Uniform2(_uPresent24Size, (float)w, h);
        }
        else if (src != null)
        {
            _gl.Uniform2(_uPresentOrigin, (float)(dispX - src.X), dispY - src.Y);
            _gl.Uniform2(_uPresentSize, (float)w1x, h1x);
            _gl.Uniform2(_uPresentTexSize, (float)src.Wide1x, src.H);
        }
        else
        {
            _gl.Uniform2(_uPresentOrigin, (float)dispX, dispY);
            _gl.Uniform2(_uPresentSize, (float)w, h);
            _gl.Uniform2(_uPresentTexSize, (float)VramShadow.Width, VramShadow.Height);
        }
        _gl.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);

        uint outTex = ApplyPostFx(_presentTex, fbW, fbH);
        EndGpuTimer(compQuery, GpuWork.Composite, compStart);
        Diagnostics.Profiler.End(compProfile);
        if (PresentSnap.Due(dispY)) SnapPresent(outTex == _postTex ? _postFbo : _presentFbo, fbW, fbH, dispX, dispY);

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        GpuTrace.Sink?.Work(GpuWork.Present, presentStart, System.Diagnostics.Stopwatch.GetTimestamp(), 0);
        return (outTex, fbW, fbH, aspect);
    }
    
    /// <summary>0069. The composited picture, read back for <see cref="PresentSnap"/>.
    /// The present texture holds the top row first, as the Output panel draws it.</summary>
    unsafe void SnapPresent(uint fbo, int w, int h, int dispX, int dispY)
    {
        var rgba = new byte[(long)w * h * 4];
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, fbo);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 1);
        fixed (byte* p = rgba)
            _gl.ReadPixels(0, 0, (uint)w, (uint)h, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 4);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        PresentSnap.Deliver(rgba, w, h, dispX, dispY);
    }

    //support for post-fx shaders to be loaded, so you can have cool shaders (this was too anonying to implement)
    unsafe uint ApplyPostFx(uint srcTex, int w, int h)
    {
        if (!PostFx.Active) return srcTex;
        if (!EnsurePostProgram()) return srcTex;

        if (_postTex == 0)
        {
            _postTex = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, _postTex);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
            _postFbo = _gl.GenFramebuffer();
        }
        if (w != _postW || h != _postH)
        {
            _gl.BindTexture(TextureTarget.Texture2D, _postTex);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0,
                PixelFormat.Rgba, PixelType.UnsignedByte, null);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _postFbo);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.Texture2D, _postTex, 0);
            _postW = w; _postH = h;
        }

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _postFbo);
        _gl.Viewport(0, 0, (uint)w, (uint)h);
        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.ScissorTest);

        _gl.UseProgram(_postProg);
        _gl.BindVertexArray(_presentVao);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, srcTex);
        if (_uPostTexSize >= 0) _gl.Uniform2(_uPostTexSize, (float)w, h);
        if (_uPostOutputSize >= 0) _gl.Uniform2(_uPostOutputSize, (float)w, h);
        if (_uPostTime >= 0) _gl.Uniform1(_uPostTime, (float)_postClock.Elapsed.TotalSeconds);
        if (_uPostFrame >= 0) _gl.Uniform1(_uPostFrame, _postFrame++);
        ApplyPostParams();
        _gl.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);

        return _postTex;
    }

    void ApplyPostParams()
    {
        int version = PostFx.ParamVersion;
        if (version != _postParamVersion)
        {
            _postParamVersion = version;
            _postParams = PostFx.SnapshotParams();
            _postParamLoc = new int[_postParams.Length];
            for (int i = 0; i < _postParams.Length; i++)
                _postParamLoc[i] = _gl.GetUniformLocation(_postProg, _postParams[i].Name);
        }

        for (int i = 0; i < _postParams.Length; i++)
            if (_postParamLoc[i] >= 0) _gl.Uniform1(_postParamLoc[i], _postParams[i].Value);
    }

    bool EnsurePostProgram()
    {
        int version = PostFx.Version;
        if (version == _postVersion) return _postProg != 0;
        _postVersion = version;

        if (_postProg != 0) { _gl.DeleteProgram(_postProg); _postProg = 0; }

        string? src = PostFx.Source;
        if (src == null) return false;

        _postProg = GlShaders.Build(_gl, GlShaders.FullscreenVs, src, "postfx", out string? error);
        if (_postProg == 0)
        {
            PostFx.Error = error ?? "shader fails to build";
            Console.WriteLine($"[gpu-pfx] {PostFx.Error}");
            return false;
        }

        PostFx.Error = null;
        _gl.UseProgram(_postProg);
        _gl.Uniform1(_gl.GetUniformLocation(_postProg, "uTex"), 0);
        _uPostTexSize = _gl.GetUniformLocation(_postProg, "uTexSize");
        _uPostOutputSize = _gl.GetUniformLocation(_postProg, "uOutputSize");
        _uPostTime = _gl.GetUniformLocation(_postProg, "uTime");
        _uPostFrame = _gl.GetUniformLocation(_postProg, "uFrame");
        _postParamVersion = -1;
        return true;
    }

    /// <summary>
    /// Whether an occlusion pass can run for this present. It needs the setting,
    /// the two programs, and a render target — the VRAM fallback and an MDEC frame
    /// have no depth attachment behind them and are left alone.
    /// </summary>
    bool AoReady(GlDisplayRt? src, bool rgb24) =>
        GteDepth.AmbientOcclusion && !rgb24 && !_legacy
        && _progAo != 0 && _progAoBlur != 0 && src is { Depth: not 0 };

    /// <summary>
    /// The two occlusion passes: the finished frame's depth attachment in, a
    /// blurred occlusion factor in <see cref="_aoBlurTex"/> out, at
    /// <see cref="AoScale"/> times the display area; the present reads it by its own
    /// uv, filtered, so any size lines up.
    ///
    /// <paramref name="ox"/>/<paramref name="oy"/> and
    /// <paramref name="sw"/>/<paramref name="sh"/> are the display area inside the
    /// target in the game's own pixels — the same rectangle the present blit uses,
    /// margin included — so both passes address one rectangle and cannot drift
    /// apart.
    /// </summary>
    unsafe void RunAo(GlDisplayRt src, float ox, float oy, int sw, int sh, int fbW, int fbH, bool normals)
    {
        int aoScale = AoScale;
        int aoW = sw * aoScale, aoH = sh * aoScale;
        EnsureAoSize(aoW, aoH);
        if (_aoTex == 0 || _aoBlurTex == 0) return;

        // The GTE's projection centre, as a fraction of the display area, so the
        // shader needs no pixel arithmetic of its own.
        //
        // Follow one coordinate the whole way rather than trusting the shape of
        // this. A vertex reaches the target at `ny + drawOff - rt.Y` and the prim
        // shader's uPosBias is `-rt.Y`, so a display target's drawing offset *is*
        // its own origin -- were it not, every polygon would already be landing in
        // the wrong place. The two therefore cancel and the target's coordinate is
        // simply `ny`, which the present samples at `(dispY - rt.Y) + uv*sh`. The
        // margin is the one asymmetry: the target's column 0 sits `margin` to the
        // left of the game's own.
        //
        // The version before this one recorded the drawing offset of the last
        // depth-writing triangle and added it, which is right only when the buffer
        // being drawn is the buffer being presented -- with two display buffers it
        // is not, on alternate frames, and half the frames reconstructed with the
        // projection centre a whole screen below the picture. It read
        // `0.500,1.500`, which is why the probe prints this number.
        float cx = (GteDepth.ProjCx + src.Margin - ox) / Math.Max(1, sw);
        float cy = (GteDepth.ProjCy - oy) / Math.Max(1, sh);
        GteDepth.AoCentreX = cx; GteDepth.AoCentreY = cy;
        // One depth texel as a step in this pass's uv. The attachment is at the
        // render scale, so this is not 1/size: at scale 4 the useful structure in
        // the depth buffer is four times finer than the game's own pixels.
        float tx = 1f / Math.Max(1, sw * GlVram.Scale);
        float ty = 1f / Math.Max(1, sh * GlVram.Scale);

        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);
        _gl.DepthMask(false);
        _gl.BindVertexArray(_presentVao);

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _aoFbo);
        _gl.Viewport(0, 0, (uint)aoW, (uint)aoH);
        _gl.UseProgram(_progAo);
        if (_uAoOrigin >= 0) _gl.Uniform2(_uAoOrigin, ox, oy);
        if (_uAoSize >= 0) _gl.Uniform2(_uAoSize, (float)sw, sh);
        if (_uAoTexSize >= 0) _gl.Uniform2(_uAoTexSize, (float)src.Wide1x, src.H);
        if (_uAoTexel >= 0) _gl.Uniform2(_uAoTexel, tx, ty);
        if (_uAoProjH >= 0) _gl.Uniform1(_uAoProjH, Math.Max(1f, GteDepth.ProjH));
        if (_uAoCentre >= 0) _gl.Uniform2(_uAoCentre, cx, cy);
        if (_uAoRadius >= 0) _gl.Uniform1(_uAoRadius, Math.Max(1f, GteDepth.AoRadius));
        if (_uAoStrength >= 0) _gl.Uniform1(_uAoStrength, GteDepth.AoStrength);
        if (_uAoBias >= 0) _gl.Uniform1(_uAoBias, GteDepth.AoBias);
        if (_uAoMaxDepth >= 0) _gl.Uniform1(_uAoMaxDepth, GteDepth.AoMaxDepth);
        if (_uAoSamples >= 0) _gl.Uniform1(_uAoSamples, Math.Clamp(GteDepth.AoSamples, 1, 64));
        if (_uAoNormalOn >= 0) _gl.Uniform1(_uAoNormalOn, normals ? 1f : 0f);
        // Only the frame the census reads back pays for the second reconstruction.
        if (_uAoNormalCompare >= 0)
            _gl.Uniform1(_uAoNormalCompare, normals && GteDepth.WantAoMap ? 1f : 0f);
        if (normals)
        {
            _gl.ActiveTexture(TextureUnit.Texture1);
            _gl.BindTexture(TextureTarget.Texture2D, src.Normal);
        }

        // 0059. The floor plan, and the transform back into it.
        bool world = GteDepth.AoWorld && GteDepth.AoWorldReady && UploadHeightfield();
        if (world) GteDepth.AoWorldFrames++;
        else if (GteDepth.AoWorld) GteDepth.AoWorldUnready++;
        if (_uAoWorldOn >= 0) _gl.Uniform1(_uAoWorldOn, world ? 1f : 0f);
        if (world)
        {
            // Uploaded untransposed on purpose: GLSL reads it column-major, which
            // is the inverse of the row-major world-to-view matrix the game keeps.
            if (_uAoViewR >= 0) _gl.UniformMatrix3(_uAoViewR, 1, false, GteDepth.AoViewR);
            if (_uAoCam >= 0) _gl.Uniform3(_uAoCam, GteDepth.AoCamX, GteDepth.AoCamY, GteDepth.AoCamZ);
            if (_uAoWorldStrength >= 0) _gl.Uniform1(_uAoWorldStrength, Math.Clamp(GteDepth.AoWorldStrength, 0f, 1f));
            if (_uAoWorldRadius >= 0) _gl.Uniform1(_uAoWorldRadius, Math.Max(1f, GteDepth.AoWorldRadius));
            if (_uAoTileUnits >= 0) _gl.Uniform1(_uAoTileUnits, (float)GteDepth.AoTileUnits);
            if (_uAoSpan >= 0) _gl.Uniform1(_uAoSpan, (float)GteDepth.AoHeightSpan);
            if (_uAoWallHeight >= 0) _gl.Uniform1(_uAoWallHeight, GteDepth.AoWallHeight);
            _gl.ActiveTexture(TextureUnit.Texture2);
            _gl.BindTexture(TextureTarget.Texture2D, _aoHeightTex);
        }
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, src.Depth);
        _gl.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _aoBlurFbo);
        _gl.Viewport(0, 0, (uint)aoW, (uint)aoH);
        _gl.UseProgram(_progAoBlur);
        if (_uAoBOrigin >= 0) _gl.Uniform2(_uAoBOrigin, ox, oy);
        if (_uAoBSize >= 0) _gl.Uniform2(_uAoBSize, (float)sw, sh);
        if (_uAoBTexSize >= 0) _gl.Uniform2(_uAoBTexSize, (float)src.Wide1x, src.H);
        // One occlusion texel, so the 4x4 box still spans the 4x4 rotation.
        if (_uAoBTexel >= 0) _gl.Uniform2(_uAoBTexel, 1f / Math.Max(1, aoW), 1f / Math.Max(1, aoH));
        // A twentieth of the depth: wide enough that a wall's own slope never
        // splits the kernel, tight enough that a doorway's edge does.
        if (_uAoBEdge >= 0) _gl.Uniform1(_uAoBEdge, 0.05f);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, _aoTex);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, src.Depth);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);

        GteDepth.AoPasses++;

        // The census, on request and for one frame: the only reading that can tell
        // a pass that shaded something from a pass that ran and returned white.
        if (GteDepth.WantAoMap) CaptureAoMap(aoW, aoH);
    }

    /// <summary>
    /// 0058. The frame's depth-carrying triangles, drawn again into the target's own
    /// normal buffer.
    ///
    /// No depth test and no depth write: the list is in submission order, so the
    /// last normal written at a pixel belongs to the last triangle drawn there,
    /// which under painter's order is the surface whose depth the occlusion pass is
    /// about to read. Agreeing with the depth buffer is therefore a property of the
    /// order rather than of a test that could disagree with it.
    ///
    /// The one place it can be wrong is a textured polygon that discarded a texel:
    /// it wrote no depth there and does write a normal. That costs a wrong normal on
    /// the see-through parts of a grate, never a wrong depth, and the pass's own
    /// fallback is what those pixels used to get.
    /// </summary>
    ///
    /// 0067. It is drawn once for both passes and it is the surface buffer too: a
    /// second attachment holding the last surface drawn at each pixel, water
    /// included. The normal attachment is blended so that a translucent surface
    /// leaves the opaque one under it, whose depth is the one the occlusion pass
    /// reads; see NormalFs.
    unsafe bool RenderSurfaces(GlDisplayRt src, int scale)
    {
        if ((!GteDepth.AoNormals && !GteDepth.Reflections) || _progNormal == 0 || src.Geo.Count == 0) return false;
        EnsureNormalTarget(src, scale);
        if (src.Normal == 0) return false;

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, src.NormalFbo);
        _gl.Viewport(0, 0, (uint)src.NormalW, (uint)src.NormalH);
        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);
        _gl.DepthMask(false);
        _gl.ColorMask(true, true, true, true);
        // Alpha 0 is "no normal here", which is the pass's own reconstruction, and
        // material 0 is "no surface".
        _gl.ClearColor(0f, 0f, 0f, 0f);
        _gl.Clear(ClearBufferMask.ColorBufferBit);
        _gl.Enable(EnableCap.Blend, 0);
        if (src.Surface != 0) _gl.Disable(EnableCap.Blend, 1);
        _gl.BlendEquation(BlendEquationModeEXT.FuncAdd);
        _gl.BlendFunc(BlendingFactor.One, BlendingFactor.OneMinusSrcAlpha);

        _gl.UseProgram(_progNormal);
        // The same transform the colour pass used on this target, so a triangle
        // lands on the pixels it landed on there.
        if (_uNrmPosBias >= 0) _gl.Uniform2(_uNrmPosBias, (float)(src.Margin - src.X), (float)(-src.Y));
        if (_uNrmFbInv >= 0) _gl.Uniform2(_uNrmFbInv, 2f / src.Wide1x, 2f / src.H);
        if (_uNrmProjH >= 0) _gl.Uniform1(_uNrmProjH, Math.Max(1f, GteDepth.ProjH));
        // The GTE's centre, in the target's own 1x pixels: its column 0 sits a
        // margin to the left of the game's.
        if (_uNrmCentre >= 0) _gl.Uniform2(_uNrmCentre, GteDepth.ProjCx + src.Margin, GteDepth.ProjCy);
        if (_uNrmScale >= 0) _gl.Uniform1(_uNrmScale, (float)scale);

        var verts = src.Geo.Verts;
        _gl.BindVertexArray(_nrmVao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _nrmVbo);
        if (verts.Length > _nrmVboVerts)
        {
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(verts.Length * sizeof(AoGeometry.V)), null,
                           BufferUsageARB.DynamicDraw);
            _nrmVboVerts = verts.Length;
        }
        _gl.BufferSubData<AoGeometry.V>(BufferTargetARB.ArrayBuffer, 0, verts);
        _gl.DrawArrays(PrimitiveType.Triangles, 0, (uint)verts.Length);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        _gl.Disable(EnableCap.Blend);
        AoGeometry.Passes++;
        return true;
    }

    /// <summary>0067. Whether a reflection pass can run: the setting, the program,
    /// and a render target with a depth attachment, as for the occlusion pass.</summary>
    bool DrawSurfaces(GlDisplayRt src, int scale)
    {
        var profile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Surfaces);
        bool drawn = RenderSurfaces(src, scale);
        Diagnostics.Profiler.End(profile);
        return drawn;
    }

    bool SsrReady(GlDisplayRt? src, bool rgb24) =>
        GteDepth.Reflections && !rgb24 && !_legacy && _progSsr != 0 && _progNormal != 0
        && src is { Depth: not 0 };

    /// <summary>0067. The reflection pass's scale: the render scale, capped by
    /// <see cref="ScreenReflections.Resolution"/>.</summary>
    static int SsrScale => ScreenReflections.Resolution <= 0 ? GlVram.Scale
        : Math.Clamp(ScreenReflections.Resolution, 1, GlVram.Scale);

    /// <summary>
    /// 0067. The reflection pass: the target's depth, surface buffer and colour in,
    /// a premultiplied reflection over the display area out, which the present
    /// composites. The rectangle and the projection are the occlusion pass's, so
    /// the two cannot drift apart.
    /// </summary>
    unsafe void RunSsr(GlDisplayRt src, float ox, float oy, int sw, int sh)
    {
        int sc = SsrScale;
        int w = sw * sc, h = sh * sc;
        EnsureSsrSize(w, h);
        if (_ssrTex == 0) return;

        // The GTE's centre as a fraction of the display area; see RunAo.
        float cx = (GteDepth.ProjCx + src.Margin - ox) / Math.Max(1, sw);
        float cy = (GteDepth.ProjCy - oy) / Math.Max(1, sh);

        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.CullFace);
        _gl.DepthMask(false);
        _gl.BindVertexArray(_presentVao);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _ssrFbo);
        _gl.Viewport(0, 0, (uint)w, (uint)h);
        _gl.UseProgram(_progSsr);
        if (_uSsrOrigin >= 0) _gl.Uniform2(_uSsrOrigin, ox, oy);
        if (_uSsrSize >= 0) _gl.Uniform2(_uSsrSize, (float)sw, sh);
        if (_uSsrTexSize >= 0) _gl.Uniform2(_uSsrTexSize, (float)src.Wide1x, src.H);
        if (_uSsrProjH >= 0) _gl.Uniform1(_uSsrProjH, Math.Max(1f, GteDepth.ProjH));
        if (_uSsrCentre >= 0) _gl.Uniform2(_uSsrCentre, cx, cy);
        if (_uSsrMaxDist >= 0) _gl.Uniform1(_uSsrMaxDist, Math.Max(64f, ScreenReflections.March()));
        if (_uSsrThickness >= 0) _gl.Uniform1(_uSsrThickness, Math.Max(1f, ScreenReflections.Thickness));
        if (_uSsrSky >= 0) _gl.Uniform1(_uSsrSky, Math.Clamp(ScreenReflections.Sky, 0f, 1f));
        if (_uSsrSteps >= 0) _gl.Uniform1(_uSsrSteps, Math.Clamp(ScreenReflections.Steps, 1, 128));
        if (_uSsrDqa >= 0) _gl.Uniform1(_uSsrDqa, (float)GteDepth.ProjDqa);
        if (_uSsrDqb >= 0) _gl.Uniform1(_uSsrDqb, (float)GteDepth.ProjDqb);
        if (_uSsrFogCurve >= 0) _gl.Uniform1(_uSsrFogCurve, ScreenReflections.FogCurve);
        BindMaterials();
        // 0068. The planar texture, when this target's picture and its capture are
        // the same frame's; otherwise the march alone, as before.
        var planar = src.Planar;
        bool planarOn = PlanarReflections.Enabled && planar is { Tex: not 0 } && src.PlanarFrame == src.LastDrawFrame;
        if (_uSsrPlanarOn >= 0) _gl.Uniform1(_uSsrPlanarOn, planarOn ? 1 : 0);
        bool retCompare = _retPlanar != null && _retPlaneN > 0 && _retCube;
        if (_uSsrCompare >= 0) _gl.Uniform1(_uSsrCompare, (planarOn || retCompare) && _ssrInfo && ScreenReflections.WantMap ? 1 : 0);
        if (planarOn)
        {
            var pp = src.PlanarPlane;
            if (_uSsrPlanarPlane >= 0) _gl.Uniform4(_uSsrPlanarPlane, pp[0], pp[1], pp[2], pp[3]);
            if (_uSsrPlanarTol >= 0) _gl.Uniform1(_uSsrPlanarTol, Math.Max(1f, PlanarReflections.Tolerance));
            if (_uSsrRipple >= 0) _gl.Uniform1(_uSsrRipple, Math.Max(0f, PlanarReflections.Ripple));
            _gl.ActiveTexture(TextureUnit.Texture4);
            _gl.BindTexture(TextureTarget.Texture2D, planar!.Depth);
            _gl.ActiveTexture(TextureUnit.Texture3);
            _gl.BindTexture(TextureTarget.Texture2D, planar.Tex);
            PlanarReflections.Read++;
        }
        // 0072. The retained scene's planes and cubemap, drawn for this target.
        BindRetainedForSsr();
        if (_retPlanar != null && _retPlaneN > 0) planar = _retPlanar;
        // Rough materials read the picture, and the planar texture, from a mip chain.
        bool rough = SurfaceMaterial.AnyRoughness;
        float colorMipH = rough ? BuildMip(src, ref _colorMipTex, ref _colorMipFbo, ref _colorMipW, ref _colorMipH, ColorMipUnit) : 0f;
        float planarMipH = rough && (planarOn || (_retPlanar != null && _retPlaneN > 0))
            ? BuildMip(planar!, ref _planarMipTex, ref _planarMipFbo, ref _planarMipW, ref _planarMipH, PlanarMipUnit) : 0f;
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _ssrFbo);
        _gl.Viewport(0, 0, (uint)w, (uint)h);
        _gl.UseProgram(_progSsr);
        if (_uSsrColorMipH >= 0) _gl.Uniform1(_uSsrColorMipH, colorMipH);
        if (_uSsrPlanarMipH >= 0) _gl.Uniform1(_uSsrPlanarMipH, planarMipH);
        _gl.ActiveTexture(TextureUnit.Texture2);
        _gl.BindTexture(TextureTarget.Texture2D, src.Tex);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, src.Surface);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, src.Depth);
        _gl.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);
        ScreenReflections.Passes++;
        if (planarOn || _retPlanar != null)
        {
            // The next capture draws into these; leave nothing bound that a prim
            // batch could read back while writing it.
            _gl.ActiveTexture(TextureUnit.Texture4);
            _gl.BindTexture(TextureTarget.Texture2D, 0);
            _gl.ActiveTexture(TextureUnit.Texture3);
            _gl.BindTexture(TextureTarget.Texture2D, 0);
            _gl.ActiveTexture(TextureUnit.Texture0);
        }

        if (ScreenReflections.WantMap && _ssrInfo) CaptureSsrMap(w, h);
    }

    const int ColorMipUnit = 7, PlanarMipUnit = 8;

    /// <summary>A target's colour, shrunk to half size and mipped, bound on
    /// <paramref name="unit"/>; its level-0 height in texels.</summary>
    unsafe float BuildMip(GlDisplayRt rt, ref uint tex, ref uint fbo, ref int mw, ref int mh, int unit)
    {
        int s = Math.Max(1, rt.CreatedScale);
        int sw = rt.Wide1x * s, sh = rt.H * s;
        int w = Math.Max(1, sw / 2), h = Math.Max(1, sh / 2);
        if (tex == 0)
        {
            tex = _gl.GenTexture();
            fbo = _gl.GenFramebuffer();
        }
        _gl.ActiveTexture(TextureUnit.Texture0 + unit);
        _gl.BindTexture(TextureTarget.Texture2D, tex);
        if (w != mw || h != mh)
        {
            int levels = 1 + (int)Math.Floor(Math.Log2(Math.Max(w, h)));
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0,
                PixelFormat.Rgba, PixelType.UnsignedByte, null);
            _gl.GenerateMipmap(TextureTarget.Texture2D);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.LinearMipmapLinear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMaxLevel, levels - 1);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, fbo);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.Texture2D, tex, 0);
            mw = w; mh = h;
        }
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, rt.Fbo);
        _gl.ReadBuffer(ReadBufferMode.ColorAttachment0);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, fbo);
        _gl.BlitFramebuffer(0, 0, sw, sh, 0, 0, w, h, ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Linear);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        _gl.GenerateMipmap(TextureTarget.Texture2D);
        _gl.ActiveTexture(TextureUnit.Texture0);
        ScreenReflections.MipBuilds++;
        return h;
    }

    unsafe void EnsureSsrSize(int w, int h)
    {
        bool info = ScreenReflections.Probe;
        if (w == _ssrW && h == _ssrH && _ssrTex != 0 && info == _ssrInfo) return;
        if (_ssrTex == 0)
        {
            _ssrTex = MakeAoTexture();
            _ssrFbo = _gl.GenFramebuffer();
        }
        _gl.BindTexture(TextureTarget.Texture2D, _ssrTex);
        _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, null);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _ssrFbo);
        _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, _ssrTex, 0);
        // The probe's second attachment: what each reflective pixel found.
        if (info)
        {
            if (_ssrInfoTex == 0) _ssrInfoTex = MakeAoTexture();
            _gl.BindTexture(TextureTarget.Texture2D, _ssrInfoTex);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0,
                PixelFormat.Rgba, PixelType.UnsignedByte, null);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment1,
                TextureTarget.Texture2D, _ssrInfoTex, 0);
            _gl.DrawBuffers([DrawBufferMode.ColorAttachment0, DrawBufferMode.ColorAttachment1]);
        }
        else
            _gl.DrawBuffers([DrawBufferMode.ColorAttachment0]);
        _ssrW = w; _ssrH = h; _ssrInfo = info;
    }

    unsafe void CaptureSsrMap(int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        var rgba = new byte[(long)w * h * 4];
        var info = new byte[(long)w * h * 4];
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _ssrFbo);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 1);
        _gl.ReadBuffer(ReadBufferMode.ColorAttachment0);
        fixed (byte* p = rgba)
            _gl.ReadPixels(0, 0, (uint)w, (uint)h, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.ReadBuffer(ReadBufferMode.ColorAttachment1);
        fixed (byte* p = info)
            _gl.ReadPixels(0, 0, (uint)w, (uint)h, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.ReadBuffer(ReadBufferMode.ColorAttachment0);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 4);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        ScreenReflections.SetMap(rgba, info, w, h);
    }

    /// <summary>0059. The area's floor plan as an 80x80 two-channel texture, uploaded
    /// only when the port has refilled it.</summary>
    unsafe bool UploadHeightfield()
    {
        var h = GteDepth.AoHeight;
        if (h == null) return false;
        if (_aoHeightTex != 0 && _aoHeightGen == GteDepth.AoHeightGen) return true;

        if (_aoHeightTex == 0)
        {
            _aoHeightTex = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, _aoHeightTex);
            // Nearest: a tile is a step, and an interpolated step is a ramp that is
            // not in the room.
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        }
        else
            _gl.BindTexture(TextureTarget.Texture2D, _aoHeightTex);

        _gl.PixelStore(PixelStoreParameter.UnpackAlignment, 1);
        fixed (byte* p = h)
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8,
                           (uint)GteDepth.AoHeightSpan, (uint)GteDepth.AoHeightSpan, 0,
                           PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.PixelStore(PixelStoreParameter.UnpackAlignment, 4);
        _aoHeightGen = GteDepth.AoHeightGen;
        return true;
    }

    unsafe void EnsureNormalTarget(GlDisplayRt rt, int scale)
    {
        int w = rt.Wide1x * scale, h = rt.H * scale;
        bool surface = GteDepth.Reflections;
        if (rt.Normal != 0 && rt.NormalW == w && rt.NormalH == h && (rt.Surface != 0) == surface) return;
        if (rt.Normal == 0)
        {
            rt.Normal = _gl.GenTexture();
            rt.NormalFbo = _gl.GenFramebuffer();
        }
        _gl.BindTexture(TextureTarget.Texture2D, rt.Normal);
        // Nearest: this is a G-buffer, and a filtered normal between two surfaces
        // is a direction neither of them faces.
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0,
                       PixelFormat.Rgba, PixelType.UnsignedByte, null);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, rt.NormalFbo);
        _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                                 TextureTarget.Texture2D, rt.Normal, 0);

        // 0067. Half floats: an octahedral normal, the depth over 65536 and a small
        // material id all survive them exactly enough, at half RGBA32F's bytes.
        if (surface)
        {
            if (rt.Surface == 0) rt.Surface = _gl.GenTexture();
            _gl.BindTexture(TextureTarget.Texture2D, rt.Surface);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
            _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba16f, (uint)w, (uint)h, 0,
                           PixelFormat.Rgba, PixelType.HalfFloat, null);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment1,
                                     TextureTarget.Texture2D, rt.Surface, 0);
            _gl.DrawBuffers([DrawBufferMode.ColorAttachment0, DrawBufferMode.ColorAttachment1]);
        }
        else
        {
            if (rt.Surface != 0)
            {
                _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment1,
                                         TextureTarget.Texture2D, 0, 0);
                _gl.DeleteTexture(rt.Surface);
                rt.Surface = 0;
            }
            _gl.DrawBuffers([DrawBufferMode.ColorAttachment0]);
        }
        rt.NormalW = w; rt.NormalH = h;
    }

    unsafe void CaptureAoMap(int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        var buf = new byte[(long)w * h * 4];
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _aoBlurFbo);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 1);
        fixed (byte* p = buf)
            _gl.ReadPixels(0, 0, (uint)w, (uint)h, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 4);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        GteDepth.SetAoMap(buf, w, h);
    }

    /// <summary>The occlusion pass's scale: the render scale, capped by
    /// <see cref="GteDepth.AoResolution"/>.</summary>
    static int AoScale => GteDepth.AoResolution <= 0 ? GlVram.Scale : Math.Clamp(GteDepth.AoResolution, 1, GlVram.Scale);

    unsafe void EnsureAoSize(int w, int h)
    {
        // Only red is drawn; the other three are the census's, so without the probe
        // the textures are one channel and the blur reads a quarter of the bytes.
        bool full = GteDepth.AoProbe;
        if (w == _aoW && h == _aoH && _aoTex != 0 && full == _aoFull) return;
        if (_aoTex == 0)
        {
            _aoTex = MakeAoTexture();
            _aoBlurTex = MakeAoTexture();
            _aoFbo = _gl.GenFramebuffer();
            _aoBlurFbo = _gl.GenFramebuffer();
        }
        Resize(_aoTex, _aoFbo);
        Resize(_aoBlurTex, _aoBlurFbo);
        _aoW = w; _aoH = h; _aoFull = full;

        void Resize(uint tex, uint fbo)
        {
            _gl.BindTexture(TextureTarget.Texture2D, tex);
            // Red is the occlusion the present multiplies by; green is the "there
            // was a surface here" mask, blue whether the normal came from the
            // geometry, and alpha how far the old depth-difference normal was from
            // it. The last three are the census's and are never drawn. See AoFs.
            _gl.TexImage2D(TextureTarget.Texture2D, 0, full ? InternalFormat.Rgba8 : InternalFormat.R8, (uint)w, (uint)h, 0,
                full ? PixelFormat.Rgba : PixelFormat.Red, PixelType.UnsignedByte, null);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, fbo);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.Texture2D, tex, 0);
        }
    }

    uint MakeAoTexture()
    {
        uint t = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, t);
        // Linear, so the present can read it at any output size without the 4x4
        // blur's own footprint showing up as blocks.
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        return t;
    }

    unsafe void EnsurePresentSize(int w, int h, bool nearest)
    {
        if (w == _presentW && h == _presentH && nearest == _presentNearest) return;
        _gl.BindTexture(TextureTarget.Texture2D, _presentTex);
        _gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)w, (uint)h, 0, PixelFormat.Rgba, PixelType.UnsignedByte, null);
        var filter = nearest ? GLEnum.Nearest : GLEnum.Linear;
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)filter);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)filter);
        _presentW = w; _presentH = h; _presentNearest = nearest;
    }

    public void Dispose()
    {
        foreach (var rt in _rts) rt?.Destroy(_gl);
        foreach (var snap in _snaps) if (snap != null) SnapDestroy(snap);
        _vram.Dispose();
        if (_aoHeightTex != 0) _gl.DeleteTexture(_aoHeightTex);
        if (_nrmVbo != 0) _gl.DeleteBuffer(_nrmVbo);
        if (_nrmVao != 0) _gl.DeleteVertexArray(_nrmVao);
        if (_vbo != 0) _gl.DeleteBuffer(_vbo);
        if (_vboLight != 0) _gl.DeleteBuffer(_vboLight);
        if (_vboTex != 0) _gl.DeleteBuffer(_vboTex);
        _mip?.Dispose();
        if (_presentVbo != 0) _gl.DeleteBuffer(_presentVbo);
        if (_aoTex != 0) _gl.DeleteTexture(_aoTex);
        if (_aoBlurTex != 0) _gl.DeleteTexture(_aoBlurTex);
        if (_aoFbo != 0) _gl.DeleteFramebuffer(_aoFbo);
        if (_aoBlurFbo != 0) _gl.DeleteFramebuffer(_aoBlurFbo);
        if (_ssrTex != 0) _gl.DeleteTexture(_ssrTex);
        if (_colorMipTex != 0) _gl.DeleteTexture(_colorMipTex);
        if (_colorMipFbo != 0) _gl.DeleteFramebuffer(_colorMipFbo);
        if (_planarMipTex != 0) _gl.DeleteTexture(_planarMipTex);
        if (_planarMipFbo != 0) _gl.DeleteFramebuffer(_planarMipFbo);
        if (_matTex != 0) _gl.DeleteTexture(_matTex);
        if (_ssrInfoTex != 0) _gl.DeleteTexture(_ssrInfoTex);
        if (_ssrFbo != 0) _gl.DeleteFramebuffer(_ssrFbo);
        if (_progSsr != 0) _gl.DeleteProgram(_progSsr);
        if (_vao != 0) _gl.DeleteVertexArray(_vao);
        if (_presentVao != 0) _gl.DeleteVertexArray(_presentVao);
        if (_progPrim != 0) _gl.DeleteProgram(_progPrim);
        if (_progPresent != 0) _gl.DeleteProgram(_progPresent);
        if (_progPresent24 != 0) _gl.DeleteProgram(_progPresent24);
        if (_presentTex != 0) _gl.DeleteTexture(_presentTex);
        if (_presentFbo != 0) _gl.DeleteFramebuffer(_presentFbo);
        if (_postProg != 0) _gl.DeleteProgram(_postProg);
        if (_postTex != 0) _gl.DeleteTexture(_postTex);
        if (_postFbo != 0) _gl.DeleteFramebuffer(_postFbo);
    }
}
