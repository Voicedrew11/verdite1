using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

/// <summary>
/// 0085. The retained map (<see cref="RetainedScene"/>) drawn into the frame itself:
/// its opaque range, from the frame's camera, gated to the halves the frame's tile walk
/// visited, into the display target the game is drawing, with the depth test and write.
/// The table walk calls it once a frame, past slot 0 (the sky), so everything the
/// table still carries is tested against it. See "Step 1, the first slice" in docs/GPU_RENDERER.md.
/// </summary>
public sealed partial class GlCore
{
    int _uwTrueColor, _uwPlainZ, _uwSnap, _uwPerPixel, _uwDither, _uwCueFromZ;

    // The map into the normal and surface buffers (WorldNormalVs, NormalFs).
    uint _progWorldNrm;
    int _uwnR, _uwnCam, _uwnT, _uwnH, _uwnC, _uwnFb, _uwnNear, _uwnHalfGate, _uwnSnap;
    int _uwnProjH, _uwnCentre, _uwnScale, _uwnDepthCull, _uwnDepthStep;
    int _uNrmDepthCull, _uNrmDepthStep;
    const int FrameDepthUnit = 18;

    void InitMainView()
    {
        if (_progWorld == 0) return;
        InitWorldNormals();
        _uwTrueColor = _gl.GetUniformLocation(_progWorld, "uTrueColor");
        _uwPlainZ = _gl.GetUniformLocation(_progWorld, "uPlainZ");
        _uwSnap = _gl.GetUniformLocation(_progWorld, "uWorldSnap");
        _uwPerPixel = _gl.GetUniformLocation(_progWorld, "uWorldPerPixel");
        _uwDither = _gl.GetUniformLocation(_progWorld, "uWorldDither");
        _uwCueFromZ = _gl.GetUniformLocation(_progWorld, "uCueFromZ");
        int L(string n) => _gl.GetUniformLocation(_progWorld, n);
        _uwDepthBias = L("uDepthBias"); _uwDepthSlope = L("uDepthSlope"); _uwDepthOnly = L("uDepthOnly"); _uwDepthCap = L("uDepthCap"); _uwDepthCapZ = L("uDepthCapZ");
        _uwOpaqueDepth = L("uOpaqueDepth"); _uwSwellOn = L("uSwellOn"); _uwSwell = L("uSwell");
        _uwWaveOn = L("uWaveOn"); _uwWaveN = L("uWaveN"); _uwWaveRect = L("uWaveRect"); _uwWaveR = L("uWaveR");
        _uwWaveCam = L("uWaveCam"); _uwWaveT = L("uWaveT"); _uwWaveCentre = L("uWaveCentre"); _uwWaveH = L("uWaveH");
        _uwWaveTime = L("uWaveTime"); _uwWaveParams = L("uWaveParams");
        _uwBk = L("uLightBk"); _uwLcmR = L("uLcmR"); _uwLcmG = L("uLcmG"); _uwLcmB = L("uLcmB");
        _uwClipOn = L("uClipOn"); _uwClipPlane = L("uClipPlane"); _uwClipCentre = L("uClipCentre");
        _uwClipH = L("uClipH"); _uwClipLevel = L("uClipLevel"); _uwClipDq = L("uClipDq");
        _gl.UseProgram(_progWorld);
        if (_uwSwellOn >= 0) _gl.Uniform1(_uwSwellOn, 0);
        if (_uwWaveOn >= 0) _gl.Uniform1(_uwWaveOn, 0);
        _gl.UseProgram(0);
        RetainedScene.MainDrawer = DrawWorldMain;
        RetainedScene.WaterDrawer = DrawWorldWater;
        RetainedScene.ArmDrawer = DrawWorldArm;
        InitModelMeshes();
    }

    // Whether the main view drew with the mip atlas, for the arm the walk draws later.
    bool _mainMips;

    // The main view's frame, for the water slices the same walk draws after it.
    RetainedScene.Frame? _wFrame;
    GlDisplayRt? _wRt;
    int _wOffX, _wOffY;
    bool _wMips;
    // The view depth the normal pass's last water slice reached.
    float _wDone;
    int _uwDepthBias, _uwDepthSlope, _uwDepthOnly, _uwSwellOn, _uwSwell, _uwDepthCap = -1, _uwDepthCapZ = -1;
    int _uwWaveOn, _uwWaveN, _uwWaveRect, _uwWaveR, _uwWaveCam, _uwWaveT, _uwWaveCentre, _uwWaveH, _uwWaveTime, _uwWaveParams;
    int _uwnSwellOn, _uwnSwell, _uwnZSlice;
    // The world normal program is set up for this pass's frame (DrawWorldNormals).
    bool _wnReady;

    bool DrawWorldMain(int offX, int offY)
    {
        if (PlanarReflections.Capturing) return DrawWorldMirror(offX, offY);
        _wFrame = null;
        RetainedScene.WaterPending = false;
        if (!RetainedScene.MainView || _progWorld == 0) return false;
        var f = RetainedScene.Find(RetainedScene.MainSerial);
        if (f == null) return false;
        Flush(FlushReason.Target);
        var rt = ClassifyDisplay();
        if (rt == null) return false;

        uint query = BeginGpuTimer();
        long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
        UpdateShadows();
        UploadStatic();
        long t1 = System.Diagnostics.Stopwatch.GetTimestamp();
        bool mips = UpdateWorldMips(f, f.MainHalves);
        _mainMips = mips;
        int models = UploadModels(f.Models, f.Serial & (ModelRing - 1), f.Serial, mips);
        int inst = PrepareInstances(f, f.Instances, mips);
        int sky = f.Sky.Count > 0 ? PrepareInstances(f, f.Sky, mips) : -1;
        long t2 = System.Diagnostics.Stopwatch.GetTimestamp();
        var (cx, cy) = BeginWorldMain(f, rt, offX, offY, mips);
        ClearStaleDepth(rt);
        // The game culls every map face on its facing; seen unmirrored, its front
        // faces are counter-clockwise here.
        _gl.Disable(EnableCap.Blend);
        _gl.DepthMask(true);
        // The sky, linked at the table's far end, goes first.
        if (sky >= 0)
        {
            DrawSky(f, sky);
            if (RetainedScene.CullBack) _gl.Enable(EnableCap.CullFace);
        }
        long t3 = System.Diagnostics.Stopwatch.GetTimestamp();
        int drawn = DrawMapOpaque();
        // 0086. The models' pixels, for the packets the walk sends after them.
        _maskModels = RetainedScene.ModelMask || RetainedScene.ModelMaskProbe;
        if (_maskModels && RetainedScene.MainModelsShown && (models >= 0 || inst >= 0))
        {
            ClearModelMask();
            rt.ModelMaskFrame = _frame;
            RetainedScene.MaskFrames++;
        }
        if (models >= 0 && RetainedScene.MainModelsShown)
        {
            DrawWorldModels(f.Models, models);
            RetainedScene.MainModelTriangles += f.Models.Count / 3;
            RetainedScene.MainModelGroups += f.Models.Groups.Count;
        }
        if (inst >= 0 && RetainedScene.MainModelsShown)
        {
            DrawInstances(f.Instances, inst);
            RetainedScene.InstancesDrawn += f.Instances.Count;
        }
        _maskModels = false;
        EndWorldState();
        EndGpuTimer(query, GpuWork.Batch, 0, Diagnostics.GpuTimes.Pass.World);

        long t4 = System.Diagnostics.Stopwatch.GetTimestamp();
        RetainedScene.MainTicks[0] += t1 - t0; RetainedScene.MainTicks[1] += t2 - t1;
        RetainedScene.MainTicks[2] += t3 - t2; RetainedScene.MainTicks[3] += t4 - t3;
        // The normal pass draws this map first (DrawWorldNormals).
        if (AoGeometry.Active)
        {
            rt.Geo.Frame(_frame, GteDepth.Generation);
            rt.Geo.WorldSerial = RetainedScene.MainSerial;
            rt.Geo.WorldCx = cx;
            rt.Geo.WorldCy = cy;
        }
        MarkDrawn(rt);
        // The probe: the depth the retained main draw left, before water, packets, present.
        if (RetainedScene.ProbeMainDue)
        {
            ProbeDepthStage(RetainedScene.ProbeMain, rt, RetainedScene.MainSerial);
            RetainedScene.ProbeMainTaken();
        }
        // The margin latch counts packet vertices past the game's clip (V), and the
        // map and models drawn here are no packets, so a target made while this
        // draws -- an aspect changed in play -- never latched and the present
        // refused it for good: the 4:3 VRAM fallback, with every pass that needs a
        // target off. This draw is a world across the margin by construction, once
        // the clip spans the target as BeginWorldMain widens it.
        if (rt.Margin > 0 && drawn > 0 && _env.ClipX0 <= rt.X && _env.ClipX1 >= rt.X + rt.W - 1)
            rt.MarginContentFlip = GpuHle.DisplayFlip;
        RetainedScene.MainDraws++;
        RetainedScene.MainTriangles += drawn / 3;

        // The water, and the models' blended faces, drawn slice by slice as the walk goes on.
        bool water = RetainedScene.MainWater && WaterInView();
        bool blend = RetainedScene.MainWater && RetainedScene.MainBlend && inst >= 0 && f.BlendFaces.Count > 0
                     && RetainedScene.MainModelsShown;
        if (water || blend)
        {
            _wFrame = f; _wRt = rt; _wOffX = offX; _wOffY = offY; _wMips = mips;
            _wDone = float.PositiveInfinity;
            long s0 = System.Diagnostics.Stopwatch.GetTimestamp();
            _bSlot = inst;
            bool sorted = water ? SortWater(f, f.View, f.MainHalves) : ClearWater();
            bool faces = blend ? SortBlend(f.BlendFaces, f.Instances) : ClearBlend();
            RetainedScene.WaterPending = sorted || faces;
            RetainedScene.MainWaterSortTicks += System.Diagnostics.Stopwatch.GetTimestamp() - s0;
            if (water && PlanarReflections.Enabled) NoteWaterPlane(f, rt, cx, cy);
        }
        _wCx = cx; _wCy = cy;
        _wOpen = RetainedScene.WaterPending;
        if (!_wOpen) { _gl.UseProgram(_progWorld); EndWorldUniforms(); }
        return true;
    }

    int _uwClipOn = -1, _uwClipPlane = -1, _uwClipCentre = -1, _uwClipH = -1, _uwClipLevel = -1, _uwClipDq = -1;

    /// <summary>
    /// The planar walk's mirror, drawn as the main view is: the map's opaque faces of
    /// the halves the mirrored walk left to the backend, and the models its replay took
    /// off the packets, from the mirrored camera into the planar texture the capture is
    /// drawing, clipped at the water and fogged level by PrimFs as the capture's packets
    /// are. The capture's own table walk calls it past slot 0; what the table still
    /// carries (the blended faces) is drawn and tested after it.
    /// </summary>
    bool DrawWorldMirror(int offX, int offY)
    {
        var f = RetainedScene.Find(RetainedScene.MirrorSerial);
        if (_progWorld == 0 || f == null || !f.MirrorOn || RetainedScene.StaticCount[0] == 0) return false;
        Flush(FlushReason.Target);
        var p = Classify();
        if (p is not { IsPlanar: true }) return false;
        if (!RetainedScene.MirrorShown) return true;

        uint query = BeginGpuTimer();
        UpdateShadows();
        UploadStatic();
        bool mips = UpdateWorldMips(f, f.MirrorHalves);
        int models = UploadModels(f.MirrorModels, MirrorSlot, f.Serial, mips);
        int inst = PrepareInstances(f, f.MirrorInstances, mips);
        BeginWorldMain(f, p, offX, offY, mips, mirror: true);
        ClearStaleDepth(p);
        _gl.Disable(EnableCap.Blend);
        _gl.DepthMask(true);
        int drawn = DrawMapOpaque();
        if (models >= 0) DrawWorldModels(f.MirrorModels, models);
        if (inst >= 0)
        {
            DrawInstances(f.MirrorInstances, inst);
            RetainedScene.MirrorInstancesDrawn += f.MirrorInstances.Count;
        }
        bool water = RetainedScene.MirrorWater && WaterInView();
        if (water || inst >= 0 && f.MirrorBlendFaces.Count > 0) DrawMirrorWater(f, inst, water);
        EndWorldState();
        EndWorldUniforms();
        _gl.UseProgram(0);
        EndGpuTimer(query, GpuWork.Batch, 0, Diagnostics.GpuTimes.Pass.Capture);

        p.LastDrawFrame = _frame;
        RetainedScene.MirrorDraws++;
        RetainedScene.MirrorTriangles += drawn / 3;
        RetainedScene.MirrorModelTriangles += f.MirrorModels.Count / 3;
        return true;
    }

    /// <summary>The mirror's blended faces, after its opaque ones and its models: the
    /// map's whole, and the models' (<see cref="RetainedScene.Frame.MirrorBlendFaces"/>),
    /// merged in the order the mirrored table would walk their packets; tested and not
    /// written, and neither swollen nor rippled, as a capture's blended packets are. From
    /// below the plane, level water faces away and is culled; what is left is water
    /// standing other than level.</summary>
    void DrawMirrorWater(RetainedScene.Frame f, int inst, bool water)
    {
        bool sorted = water ? SortWater(f, f.MirrorView, f.MirrorHalves) : ClearWater();
        _bSlot = inst;
        bool faces = inst >= 0 && f.MirrorBlendFaces.Count > 0 ? SortBlend(f.MirrorBlendFaces, f.MirrorInstances) : ClearBlend();
        if (sorted || faces)
        {
            for (int r = 1; r <= 4; r++)
            {
                _wAt[r] = 0;
                _wWalk[r] = _wSortCount[r] / 3;
                _wTake[r] = _wWalk[r];
                _bAt[r - 1] = 0;
                _bWalk[r - 1] = _bModeCount[r - 1];
                _bTake[r - 1] = _bWalk[r - 1];
                RetainedScene.MirrorWaterTriangles += _wTake[r];
            }
            _gl.Enable(EnableCap.CullFace);
            DrawMerged(f, f.MirrorInstances, 0f, 0f, 0f, waves: false, opaqueDepth: false, _wTake, _bTake);
            if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 0);
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
        }
        // The main view sorts its own again.
        ClearWater();
        ClearBlend();
    }

    /// <summary>
    /// The map's opaque range with 0051's tolerance, as its packets had it: true depth
    /// first with colour off, then colour against it pulled towards the camera. Two
    /// wall panels that overlap in one plane interpolate their depths differently
    /// across different triangles, and drawn in one pass under a bare GL_LEQUAL they
    /// took turns pixel by pixel; with the tolerance both pass and the later-drawn
    /// one wins at every pixel of the overlap, in a draw order that does not move with
    /// the camera. The vertices drawn.
    /// </summary>
    int DrawMapOpaque()
    {
        bool bias = GteDepth.ZBuffer && (GteDepth.DepthBias > 0f || GteDepth.DepthSlope > 0f);
        if (!bias) return DrawRange(0, null);
        DepthOnly(true);
        DrawRange(0, null);
        DepthOnly(false);
        _gl.DepthMask(false);
        if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, GteDepth.DepthBias / 65536f);
        if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, GteDepth.DepthSlope);
        ProbeTolerance(0, () => DrawRange(0, null));
        int drawn = DrawRange(0, null);
        if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
        if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
        _gl.DepthMask(true);
        RetainedScene.MainMapPrepasses++;
        return drawn;
    }

    /// <summary>The tolerance probe (<see cref="RetainedScene.ToleranceProbe"/>), in a
    /// colour pass's state, before it draws: <paramref name="draw"/> again with colour
    /// off, its samples counted with the tolerance whole and capped at each of
    /// <see cref="RetainedScene.ToleranceCaps"/>. Writes nothing.</summary>
    void ProbeTolerance(int pass, Action draw)
    {
        if (!RetainedScene.ToleranceProbe || _uwDepthCap < 0) return;
        _gl.ColorMask(false, false, false, false);
        long Count(float cap)
        {
            _gl.Uniform1(_uwDepthCap, cap / 65536f);
            uint q = _gl.GenQuery();
            _gl.BeginQuery(QueryTarget.SamplesPassed, q);
            draw();
            _gl.EndQuery(QueryTarget.SamplesPassed);
            _gl.GetQueryObject(q, QueryObjectParameterName.Result, out long n);
            _gl.DeleteQuery(q);
            return n;
        }
        long all = Count(1e9f);
        RetainedScene.ToleranceSamples[pass] += all;
        var caps = RetainedScene.ToleranceCaps;
        for (int k = 0; k < caps.Length; k++)
            RetainedScene.ToleranceBehind[pass * caps.Length + k] += all - Count(caps[k]);
        _gl.Uniform1(_uwDepthCap, 1e9f / 65536f);
        _gl.ColorMask(true, true, true, true);
    }

    // 0086. The main view's models are drawing, and mark the stencil where they land.
    bool _maskModels;

    /// <summary>0086. The target's stencil cleared to "no model here", whatever the
    /// scissor; the models then set it where their colour lands (MarkModels).</summary>
    void ClearModelMask()
    {
        bool scissor = _gl.IsEnabled(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.StencilMask(0xFF);
        _gl.ClearStencil(0);
        _gl.Clear(ClearBufferMask.StencilBufferBit);
        if (scissor) _gl.Enable(EnableCap.ScissorTest);
    }

    /// <summary>0086. Around a model colour pass of the main view: each pixel it draws
    /// is marked. Nothing is rejected by the stencil.</summary>
    void MarkModels(bool on)
    {
        if (!_maskModels) return;
        if (on)
        {
            _gl.Enable(EnableCap.StencilTest);
            _gl.StencilFunc(StencilFunction.Always, 1, 1);
            _gl.StencilOp(StencilOp.Keep, StencilOp.Keep, StencilOp.Replace);
            _gl.StencilMask(1);
        }
        else
        {
            _gl.StencilOp(StencilOp.Keep, StencilOp.Keep, StencilOp.Keep);
            _gl.StencilMask(0xFF);
            _gl.Disable(EnableCap.StencilTest);
        }
    }

    /// <summary>0086's probe, before a model list writes depth: its samples against the
    /// map's depth with the tolerance, and those that only a test pulled the map's
    /// slack towards the camera passes. Writes nothing.</summary>
    void ProbeModelsUnderMap(Action draw)
    {
        if (!_maskModels || !RetainedScene.ModelMaskProbe || _uwDepthBias < 0) return;
        _gl.ColorMask(false, false, false, false);
        _gl.DepthMask(false);
        long Count(float bias, float slope)
        {
            _gl.Uniform1(_uwDepthBias, bias / 65536f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, slope);
            uint q = _gl.GenQuery();
            _gl.BeginQuery(QueryTarget.SamplesPassed, q);
            draw();
            _gl.EndQuery(QueryTarget.SamplesPassed);
            _gl.GetQueryObject(q, QueryObjectParameterName.Result, out long n);
            _gl.DeleteQuery(q);
            return n;
        }
        long tolerant = Count(GteDepth.DepthBias, GteDepth.DepthSlope);
        long slack = 0;
        for (int i = 0; i < RetainedScene.MapSlacks.Length; i++)
        {
            slack = Count(RetainedScene.MapSlacks[i], GteDepth.DepthSlope);
            RetainedScene.ModelUnderSlack[i] += slack - tolerant;
        }
        _gl.Uniform1(_uwDepthBias, 0f);
        if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
        _gl.ColorMask(true, true, true, true);
        _gl.DepthMask(true);
        RetainedScene.ModelSamples += tolerant;
        RetainedScene.ModelUnderMap += slack - tolerant;
    }

    /// <summary>The depth half of 0051's two passes: colour masked, and PrimFs making
    /// only the decisions that say whether a fragment exists.</summary>
    void DepthOnly(bool on)
    {
        _gl.ColorMask(!on, !on, !on, !on);
        if (_uwDepthOnly >= 0) _gl.Uniform1(_uwDepthOnly, on ? 1 : 0);
    }

    void MarkDrawn(GlDisplayRt rt)
    {
        rt.Dirty = true;
        rt.LastDrawFrame = _frame;
        rt.RetainedSerial = RetainedScene.Serial;
        _lastZRt = rt;
    }

    /// <summary>The world program set up for the frame's camera in the display target,
    /// depth tested, culled, scissored to the game's clip; the centre it drew with.
    /// With <paramref name="mirror"/>, the mirrored camera into the target's planar
    /// texture, clipped at the water and fogged level as a capture's packets are.</summary>
    (float, float) BeginWorldMain(RetainedScene.Frame f, GlDisplayRt rt, int offX, int offY, bool mips, bool mirror = false)
    {
        CloseWorldMain();
        _gl.UseProgram(_progWorld);
        if (_uwMipOn >= 0) _gl.Uniform1(_uwMipOn, mips ? 1f : 0f);
        BeginWorldLights();
        _gl.ActiveTexture(TextureUnit.Texture0 + HalvesUnit);
        _gl.BindTexture(TextureTarget.Texture2D, _halvesTex);
        _gl.TexSubImage2D<byte>(TextureTarget.Texture2D, 0, 0, 0, RetainedScene.HalvesW, RetainedScene.HalvesH,
            PixelFormat.RedInteger, PixelType.UnsignedByte, mirror ? f.MirrorHalves : f.MainHalves);
        if (_uwHalfGate >= 0) _gl.Uniform1(_uwHalfGate, 1);
        if (_uwAniso >= 0) _gl.Uniform1(_uwAniso, (float)GteDepth.Anisotropy);
        if (_uwTrueColor >= 0) _gl.Uniform1(_uwTrueColor, GteDepth.TrueColor ? 1f : 0f);
        if (_uwPlainZ >= 0) _gl.Uniform1(_uwPlainZ, GteDepth.PlainDepth);
        if (_uwMirror >= 0) _gl.Uniform1(_uwMirror, 0);
        if (_uwDepthCapZ >= 0)
            _gl.Uniform1(_uwDepthCapZ, RetainedScene.DepthCapPixels > 0f ? RetainedScene.DepthCapPixels / Math.Max(1f, f.View.H) : 0f);
        if (_uwMaskOn >= 0) _gl.Uniform1(_uwMaskOn, 0);
        if (_uwAtmosSkip >= 0) _gl.Uniform1(_uwAtmosSkip, 0);
        // The frame's own settings, as its packets take them: the GTE's whole pixels
        // without sub-pixel, the corner colours without light records, and the
        // crosshatch while the draw area dithers (a map face is textured and shaded).
        if (_uwSnap >= 0) _gl.Uniform1(_uwSnap, GteDepth.Subpixel ? 0 : 1);
        if (_uwPerPixel >= 0) _gl.Uniform1(_uwPerPixel, GteLightMap.Enabled ? 1 : 0);
        if (_uwDither >= 0) _gl.Uniform1(_uwDither, _env.Dither ? 1 : 0);
        if (_uwNeighbour >= 0) _gl.Uniform1(_uwNeighbour, NeighbourBlend.Mode);
        if (_uwNbTile >= 0) _gl.Uniform1(_uwNbTile, NeighbourBlend.Tile);
        // 0089. The main view only: the mirror's walk is the game's own.
        SendFade(_uwFade, _uwFadeZ, mirror ? null : f);
        // Fogged at each pixel's own depth: a face clipped at the eye has no corner
        // whose screen-affine fog holds at the clip.
        if (_uwCueFromZ >= 0) _gl.Uniform1(_uwCueFromZ, RetainedScene.MainFogFromZ ? Math.Max(1f, f.View.H) : 0f);
        // The mirrored walk's water was never swollen (WaterSwell stands down for it).
        if (mirror) { if (_uwSwellOn >= 0) _gl.Uniform1(_uwSwellOn, 0); }
        else SendWorldSwell(f, _uwSwellOn, _uwSwell);
        SendWorldFluid();
        SendWorldAtmos();

        // The GTE's centre is in the draw area's pixels; the target's are offset
        // from them by the draw offset and the widescreen margin.
        var v = mirror ? f.MirrorView : f.View;
        Span<float> r = [v.R00, v.R01, v.R02, v.R10, v.R11, v.R12, v.R20, v.R21, v.R22];
        float cx = v.Cx + offX - rt.X + rt.Margin, cy = v.Cy + offY - rt.Y;
        // 0068's clip and level fog, as GlCore sends them for a planar batch.
        if (_uwClipOn >= 0) _gl.Uniform1(_uwClipOn, mirror ? 1 : 0);
        if (mirror && _uwClipOn >= 0)
        {
            var cp = rt.ClipPlane;
            _gl.Uniform4(_uwClipPlane, cp[0], cp[1], cp[2], cp[3]);
            _gl.Uniform2(_uwClipCentre, cx, cy);
            _gl.Uniform1(_uwClipH, Math.Max(1f, v.H));
            var la = rt.LevelAxis;
            if (_uwClipLevel >= 0) _gl.Uniform3(_uwClipLevel, la[0], la[1], la[2]);
            if (_uwClipDq >= 0) _gl.Uniform2(_uwClipDq, (float)GteDepth.ProjDqa, GteDepth.ProjDqb / 4096f);
        }
        SetWorldView(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, v.H, cx, cy, rt.Wide1x, rt.H, v.H, true, GlVram.Scale);
        if (_uwModelGteC >= 0) _gl.Uniform2(_uwModelGteC, v.Cx, v.Cy);
        if (_uwNear >= 0) _gl.Uniform1(_uwNear, RetainedScene.MainNear);
        SendWorldLights(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, cx, cy, v.H, false, 0f);
        CullChunks(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, v.H, cx, cy, rt.Wide1x, rt.H, false, 0f, v.H,
                   float.PositiveInfinity);
        BindWorldMain(rt, mips);
        return (cx, cy);
    }

    /// <summary>What a water slice sets again once the table's packets have drawn
    /// in between: the program, its textures, the target, the clip, the facing cull
    /// and the depth test. The uniforms stay the frame's.</summary>
    void BindWorldMain(GlDisplayRt rt, bool mips)
    {
        _gl.UseProgram(_progWorld);
        if (mips)
        {
            _gl.ActiveTexture(TextureUnit.Texture5);
            _gl.BindTexture(TextureTarget.Texture2D, _mip!.Texture);
        }
        BindWorldLights();
        _gl.ActiveTexture(TextureUnit.Texture0 + HalvesUnit);
        _gl.BindTexture(TextureTarget.Texture2D, _halvesTex);
        _gl.ActiveTexture(TextureUnit.Texture1);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, _vram.SampleTexture);

        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, rt.Fbo);
        _gl.Viewport(0, 0, (uint)rt.TexW, (uint)rt.TexH);
        // The game's clip, widened over the margin as FlushCore widens it.
        int s = GlVram.Scale;
        int clipX0 = _env.ClipX0 - rt.X + rt.Margin, clipY0 = _env.ClipY0 - rt.Y;
        int clipX1 = _env.ClipX1 - rt.X + rt.Margin, clipY1 = _env.ClipY1 - rt.Y;
        if (rt.Margin > 0 && _env.ClipX0 <= rt.X && _env.ClipX1 >= rt.X + rt.W - 1) { clipX0 = 0; clipX1 = rt.Wide1x - 1; }
        _gl.Enable(EnableCap.ScissorTest);
        _gl.Scissor(clipX0 * s, clipY0 * s,
            (uint)Math.Max(0, (clipX1 - clipX0 + 1) * s), (uint)Math.Max(0, (clipY1 - clipY0 + 1) * s));
        if (RetainedScene.CullBack)
        {
            _gl.Enable(EnableCap.CullFace);
            _gl.FrontFace(FrontFaceDirection.Ccw);
            _gl.CullFace(TriangleFace.Back);
        }
        _gl.ColorMask(true, true, true, true);
        _gl.Enable(EnableCap.DepthTest);
        _gl.DepthFunc(DepthFunction.Lequal);
    }

    void EndWorldState()
    {
        _gl.Disable(EnableCap.CullFace);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.DepthMask(false);
        _gl.ColorMask(true, true, true, true);
        _gl.BindVertexArray(0);
        _gl.ActiveTexture(TextureUnit.Texture0);
    }

    /// <summary>The world program's uniforms back to what the reflections and the
    /// shadow cubemaps draw with.</summary>
    void EndWorldUniforms()
    {
        if (_uwHalfGate >= 0) _gl.Uniform1(_uwHalfGate, 0);
        if (_uwSnap >= 0) _gl.Uniform1(_uwSnap, 0);
        if (_uwPerPixel >= 0) _gl.Uniform1(_uwPerPixel, 1);
        if (_uwDither >= 0) _gl.Uniform1(_uwDither, 0);
        if (_uwNeighbour >= 0) _gl.Uniform1(_uwNeighbour, 0);
        SendFade(_uwFade, _uwFadeZ, null);
        if (_uwCueFromZ >= 0) _gl.Uniform1(_uwCueFromZ, 0f);
        if (_uwSwellOn >= 0) _gl.Uniform1(_uwSwellOn, 0);
        if (_uwClipOn >= 0) _gl.Uniform1(_uwClipOn, 0);
        if (_uwDepthCapZ >= 0) _gl.Uniform1(_uwDepthCapZ, 0f);
        EndWorldLights();
    }

    // The world program still holds the main view's uniforms, for the water the
    // walk has yet to draw (DrawWorldWater).
    bool _wOpen;
    float _wCx, _wCy;

    /// <summary>Anything else that draws with the world program first puts back its
    /// uniforms; the next water slice then sets them up again.</summary>
    void CloseWorldMain()
    {
        if (!_wOpen) return;
        _wOpen = false;
        _gl.UseProgram(_progWorld);
        EndWorldUniforms();
    }

    void SendWorldSwell(RetainedScene.Frame f, int on, int waves)
    {
        if (on < 0) return;
        _gl.Uniform1(on, f.SwellOn ? 1 : 0);
        if (f.SwellOn && waves >= 0) _gl.Uniform4(waves, 3u, new ReadOnlySpan<float>(f.Swell, 0, 12));
    }

    // ---- the water ----------------------------------------------------------------

    /// <summary>Whether a visible chunk has blended faces.</summary>
    bool WaterInView()
    {
        for (int c = 0; c < RetainedScene.Chunks; c++)
            if (_chunkVis[c] && RetainedScene.ChunkCount[RetainedScene.Chunks + c] + RetainedScene.ChunkCount[2 * RetainedScene.Chunks + c]
                                + RetainedScene.ChunkCount[3 * RetainedScene.Chunks + c] + RetainedScene.ChunkCount[4 * RetainedScene.Chunks + c] > 0)
                return true;
        return false;
    }

    /// <summary>
    /// The table walk's: the water the walk has passed at view depth <paramref name="cut"/>
    /// and not drawn yet, drawn now, before the packet the walk is about to send. Whole
    /// faces in SortWater's order, cut at the key the game links each at, so a face
    /// goes where its packet would have: among 0079's held packets as well as before a
    /// barrier. False once none is left.
    /// </summary>
    bool DrawWorldWater(float cut, float bx0, float by0, float bx1, float by1)
    {
        var f = _wFrame;
        if (f == null) return false;
        int cutKey = float.IsNegativeInfinity(cut) ? int.MinValue : (int)MathF.Floor(cut * 0.25f);
        var take = _wTake;
        var btake = _bTake;
        int any = 0;
        bool left = false;
        foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
        {
            int first = _wSortStart[range] / 3, n = _wSortCount[range] / 3, k = _wWalk[range];
            for (; k < n && _wKeyAt[first + k] > cutKey; k++)
            {
                int b = (first + k) * 4;
                _pX0 = Math.Min(_pX0, _wBox[b]); _pY0 = Math.Min(_pY0, _wBox[b + 1]);
                _pX1 = Math.Max(_pX1, _wBox[b + 2]); _pY1 = Math.Max(_pY1, _wBox[b + 3]);
            }
            _wWalk[range] = k;
            take[range] = k - _wAt[range];
            any += take[range];
            left |= _wAt[range] < n;

            // The models' blended faces the walk has passed, at the slot their packets
            // would be linked: a table slot is the map's key plus the tile bias.
            int mode = range - 1, bfirst = _bModeStart[mode], bn = _bModeCount[mode], bk = _bWalk[mode];
            for (; bk < bn && _bEnt[bfirst + bk].Key - TileBias > cutKey; bk++)
            {
                ref var e = ref _bEnt[bfirst + bk];
                _pX0 = Math.Min(_pX0, e.X0); _pY0 = Math.Min(_pY0, e.Y0);
                _pX1 = Math.Max(_pX1, e.X1); _pY1 = Math.Max(_pY1, e.Y1);
            }
            _bWalk[mode] = bk;
            btake[mode] = bk - _bAt[mode];
            any += btake[mode];
            left |= _bAt[mode] < bn;
        }
        if (any == 0) { RetainedScene.MainWaterEmpty++; return left; }
        // Water that shares no pixel with what comes next may wait: the order there
        // shows nowhere.
        if (!PendingMeets(bx0, by0, bx1, by1)) { RetainedScene.MainWaterDeferred++; return true; }
        ResetPending();
        left = false;
        bool leftWater = false;
        foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
        {
            leftWater |= _wWalk[range] < _wSortCount[range] / 3;
            left |= _bWalk[range - 1] < _bModeCount[range - 1];
        }
        left |= leftWater;
        // The normal pass cuts per pixel at the same depths (DrawWorldWaterNormals).
        float lo = leftWater ? cutKey * 4f : float.NegativeInfinity, hi = _wDone;
        _wDone = lo;

        Flush(FlushReason.Target);
        var rt = ClassifyDisplay();
        if (rt != _wRt) { RetainedScene.MainMissed++; return false; }
        uint query = BeginGpuTimer();
        float cx = _wCx, cy = _wCy;
        if (_wOpen) BindWorldMain(rt, _wMips);
        else
        {
            (cx, cy) = BeginWorldMain(f, rt, _wOffX, _wOffY, _wMips);
            _wOpen = true;
        }
        DrawMerged(f, f.Instances, cx, cy, f.View.H, waves: true, opaqueDepth: true, take, btake);
        foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
        {
            _wAt[range] += take[range];
            _bAt[range - 1] += btake[range - 1];
        }
        _gl.BindVertexArray(_worldVao);
        if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 0);
        if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
        if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
        EndWorldState();
        if (!left) CloseWorldMain();
        EndGpuTimer(query, GpuWork.Batch, 0, Diagnostics.GpuTimes.Pass.World);

        // One entry for the normal pass per run of the list the water went in after.
        if (AoGeometry.Active && rt.Geo.WorldSerial == f.Serial)
        {
            var w = rt.Geo.Water;
            if (w.Count > 0 && w[^1].At == rt.Geo.Count) w[^1] = (w[^1].At, lo, w[^1].Hi);
            else w.Add((rt.Geo.Count, lo, hi));
        }
        MarkDrawn(rt);
        RetainedScene.MainWaterSlices++;
        RetainedScene.MainWaterTriangles += any;
        return left;
    }

    /// <summary>
    /// The blended streams from their heads to where the walk has reached: the water's
    /// four ranges (<see cref="_wAt"/> to <see cref="_wWalk"/>) and the models' four
    /// modes (<see cref="_bAt"/> to <see cref="_bWalk"/>), in the order the table would
    /// walk their packets, tested and not written; then, with <paramref name="opaqueDepth"/>,
    /// the depth of their texels the GPU draws opaque. The world program is bound for
    /// the view; <paramref name="take"/> and <paramref name="btake"/> are what each
    /// stream draws. Leaves the world VAO bound.
    /// </summary>
    void DrawMerged(RetainedScene.Frame f, List<RetainedScene.ModelInstance> insts, float cx, float cy, float h,
                    bool waves, bool opaqueDepth, int[] take, int[] btake)
    {
        bool models = false;
        // The world VAO and the static map's textures, or the mesh VAO and the models'.
        void EnterWater()
        {
            if (models) { EndInstances(true); models = false; }
            if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 1);
            _gl.BindVertexArray(_worldVao);
        }
        void EnterModels()
        {
            if (models) return;
            BeginInstances(_bSlot, true);
            _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, _bEbo);
            models = true;
        }
        EnterWater();
        // Blended at mode `blend`, or plain with -1 (the opaque texels' depth).
        void Draw(int range, int from, int to, int blend)
        {
            long at = _wSortStart[range] + 3L * from;
            if (blend >= 0) DrawBlended(blend, 3 * (to - from), at * 4L);
            else unsafe { _gl.DrawElements(PrimitiveType.Triangles, (uint)(3 * (to - from)), DrawElementsType.UnsignedInt, (void*)(at * 4L)); }
        }
        // A run of one instance's blended faces, entries a to b of the sorted list.
        void DrawFaces(int a, int b, int blend)
        {
            var m = insts[_bEnt[a].Inst];
            if (m.MeshGen != _meshGen || !RetainedScene.BlendShown) return;
            EnterModels();
            SendInstance(m, true);
            if (!RetainedScene.BlendDepth) _gl.Disable(EnableCap.DepthTest);
            int from = _bIdxAt[a], to = b < _bEntCount ? _bIdxAt[b] : _bIdxCount;
            if (blend >= 0) DrawBlended(blend, to - from, from * 4L);
            else unsafe { _gl.DrawElements(PrimitiveType.Triangles, (uint)(to - from), DrawElementsType.UnsignedInt, (void*)(from * 4L)); }
            if (!RetainedScene.BlendDepth) _gl.Enable(EnableCap.DepthTest);
            RetainedScene.BlendRuns++;
        }
        // Back to front, as the packets are, since an averaging blend depends on the
        // order (SortWater); tested and not written, as a blended packet is (GlCore's
        // zMode 2), with 0051's tolerance on the test.
        bool bias = GteDepth.ZBuffer && (GteDepth.DepthBias > 0f || GteDepth.DepthSlope > 0f);
        if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, bias ? GteDepth.DepthBias / 65536f : 0f);
        if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, bias ? GteDepth.DepthSlope : 0f);
        _gl.DepthMask(false);
        // The water's faces and the models', every blend mode, in one order: the order
        // the table would walk their packets. By key, far first; at one key a model
        // first, since it was built after the map, and of two models the last built.
        // Blend modes do not commute (an average and an add), so a run is one stream's
        // faces only while nothing in another stream comes before its next.
        var head = _mHead; var end = _mEnd;
        for (int r = 1; r <= 4; r++)
        {
            head[r - 1] = _wAt[r]; end[r - 1] = _wWalk[r];
            head[r + 3] = _bModeStart[r - 1] + _bAt[r - 1]; end[r + 3] = _bModeStart[r - 1] + _bWalk[r - 1];
        }
        // Whether entry i of stream a goes before entry j of stream b.
        bool Before(int a, int i, int b, int j)
        {
            int ka = a < 4 ? _wKeyAt[_wSortStart[a + 1] / 3 + i] : _bEnt[i].Key - TileBias;
            int kb = b < 4 ? _wKeyAt[_wSortStart[b + 1] / 3 + j] : _bEnt[j].Key - TileBias;
            if (ka != kb) return ka > kb;
            if (a < 4 != b < 4) return a >= 4;
            return a < 4 ? a < b : _bEnt[i].Seq > _bEnt[j].Seq;
        }
        while (true)
        {
            int a = -1;
            for (int b = 0; b < 8; b++)
                if (head[b] < end[b] && (a < 0 || Before(b, head[b], a, head[a]))) a = b;
            if (a < 0) break;
            // The first of the other streams' heads, which do not move during the run.
            int o = -1;
            for (int b = 0; b < 8; b++)
                if (b != a && head[b] < end[b] && (o < 0 || Before(b, head[b], o, head[o]))) o = b;
            int i = head[a], j = i + 1;
            if (a < 4)
            {
                while (j < end[a] && (o < 0 || Before(a, j, o, head[o]))) j++;
                int range = a + 1, mode = a;
                EnterWater();
                SendWorldWaves(waves && (mode == 0 || mode == 3), cx, cy, h);
                Draw(range, i, j, mode);
                SendWorldWaves(false, 0f, 0f, 0f);
            }
            else
            {
                int inst = _bEnt[i].Inst;
                while (j < end[a] && _bEnt[j].Inst == inst && (o < 0 || Before(a, j, o, head[o]))) j++;
                DrawFaces(i, j, a - 4);
                if (insts == f.Instances) RetainedScene.BlendDrawn += j - i;
                else RetainedScene.MirrorBlendDrawn += j - i;
            }
            RetainedScene.MainWaterRuns++;
            head[a] = j;
        }
        // The texels without the semi-transparency bit draw opaque, so they hide what
        // is behind them from the occlusion pass, as a blended packet's do: their true
        // depth, after the colour.
        if (opaqueDepth && GteDepth.SurfacesWanted && _uwOpaqueDepth >= 0)
        {
            _gl.Disable(EnableCap.Blend);
            _gl.ColorMask(false, false, false, false);
            _gl.DepthMask(true);
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
            _gl.Uniform1(_uwOpaqueDepth, 1);
            EnterWater();
            foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
                if (take[range] > 0) Draw(range, _wAt[range], _wWalk[range], -1);
            foreach (int mode in (ReadOnlySpan<int>)[0, 1, 2, 3])
            {
                if (btake[mode] == 0) continue;
                int a = _bModeStart[mode] + _bAt[mode], z = _bModeStart[mode] + _bWalk[mode];
                for (int i = a; i < z;)
                {
                    int j = i + 1, inst = _bEnt[i].Inst;
                    while (j < z && _bEnt[j].Inst == inst) j++;
                    // A solid model hides what is behind it with every texel.
                    _gl.Uniform1(_uwOpaqueDepth, insts[inst].Solid ? 2 : 1);
                    DrawFaces(i, j, -1);
                    i = j;
                }
            }
            _gl.Uniform1(_uwOpaqueDepth, 0);
        }
        if (models) EndInstances(true);
        _gl.BindVertexArray(_worldVao);
    }

    // The models' blended faces of the main view, sorted by blend mode, then far to
    // near by table slot, then the last built first. An element buffer of their corners
    // in the mesh store, in that order, one run per face.
    RetainedScene.BlendFace[] _bEnt = [];
    int _bEntCount, _bIdxCount, _bSlot;
    int[] _bIdxAt = [];
    uint[] _bIdx = [];
    uint _bEbo;
    readonly int[] _bModeStart = new int[4], _bModeCount = new int[4], _bAt = new int[4], _bWalk = new int[4], _bTake = new int[4];
    // The merge's streams: the water's four ranges, then the models' four modes.
    readonly int[] _mHead = new int[8], _mEnd = new int[8];
    /// <summary>A tile is linked at its mean depth plus this many slots (PolyAssembler).</summary>
    const int TileBias = 0xF0;

    bool ClearBlend()
    {
        Array.Clear(_bModeCount);
        Array.Clear(_bModeStart);
        Array.Clear(_bAt);
        Array.Clear(_bWalk);
        _bEntCount = 0;
        return false;
    }

    /// <summary>The water state of a frame that has none.</summary>
    bool ClearWater()
    {
        Array.Clear(_wSortCount);
        Array.Clear(_wSortStart);
        Array.Clear(_wAt);
        Array.Clear(_wWalk);
        ResetPending();
        return false;
    }

    /// <summary>
    /// The frame's blended model faces, sorted as the table would walk their packets:
    /// per blend mode (the order the water is drawn in), far to near by slot, and within
    /// a slot the last built first. Their corners go to an element buffer on the mesh
    /// VAO. False when none is left to draw.
    /// </summary>
    unsafe bool SortBlend(List<RetainedScene.BlendFace> src, List<RetainedScene.ModelInstance> insts)
    {
        ClearBlend();
        int n = src.Count;
        if (n == 0 || !InstancesReady || _bSlot < 0) return false;
        if (_bEnt.Length < n) { _bEnt = new RetainedScene.BlendFace[n * 2]; _bIdxAt = new int[n * 2]; }
        int m = 0;
        for (int i = 0; i < n; i++)
        {
            var e = src[i];
            if (insts[e.Inst].MeshGen != _meshGen || RetainedScene.BlendOnly >= 0 && e.Inst != RetainedScene.BlendOnly) continue;
            _bEnt[m++] = e;
        }
        if (m == 0) return false;
        Array.Sort(_bEnt, 0, m, Comparer<RetainedScene.BlendFace>.Create((a, b) =>
            a.Mode != b.Mode ? a.Mode.CompareTo(b.Mode) : a.Key != b.Key ? b.Key.CompareTo(a.Key) : b.Seq.CompareTo(a.Seq)));
        // Instances stay together where their faces are adjacent in the sorted order; the
        // draws split a run wherever the instance changes.
        int corners = 0;
        for (int i = 0; i < m; i++) corners += _bEnt[i].Corners;
        if (_bIdx.Length < corners) _bIdx = new uint[corners * 2];
        int at = 0;
        for (int i = 0; i < m; i++)
        {
            _bIdxAt[i] = at;
            for (int j = 0; j < _bEnt[i].Corners; j++) _bIdx[at++] = (uint)(_bEnt[i].Corner + j);
            _bModeCount[_bEnt[i].Mode]++;
        }
        for (int mode = 1; mode < 4; mode++) _bModeStart[mode] = _bModeStart[mode - 1] + _bModeCount[mode - 1];
        _bEntCount = m;
        _bIdxCount = at;
        if (_bEbo == 0) _bEbo = _gl.GenBuffer();
        _gl.BindVertexArray(_meshVao);
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, _bEbo);
        fixed (uint* p = _bIdx)
            _gl.BufferData(BufferTargetARB.ElementArrayBuffer, (nuint)(at * 4), p, BufferUsageARB.StreamDraw);
        _gl.BindVertexArray(0);
        RetainedScene.BlendSorted += m;
        return true;
    }

    // The frame's visible blended triangles, far to near, as vertex indices into the
    // static map: an element buffer on the world VAO, a run per range.
    uint _wEbo;
    uint[] _wIdx = [];
    int[] _wTri = [], _wKey = [];
    readonly int[] _wBucket = new int[SortBuckets + 1];
    readonly int[] _wSortStart = new int[5], _wSortCount = new int[5];
    // Each sorted triangle's key and screen box; per range how many are drawn, and
    // how many the walk has passed; the passed and undrawn ones' box.
    int[] _wKeyAt = [];
    float[] _wTriBox = [], _wBox = [];
    readonly int[] _wAt = new int[5], _wWalk = new int[5], _wTake = new int[5];
    float _pX0, _pY0, _pX1, _pY1;

    void ResetPending() { _pX0 = _pY0 = float.MaxValue; _pX1 = _pY1 = float.MinValue; }

    /// <summary>Whether a box meets the walked and undrawn water: their union first,
    /// then each triangle, up to a bound past which it counts as met.</summary>
    bool PendingMeets(float x0, float y0, float x1, float y1)
    {
        if (_pX1 < x0 || x1 < _pX0 || _pY1 < y0 || y1 < _pY0) return false;
        int budget = 256;
        foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
        {
            int first = _wSortStart[range] / 3;
            for (int k = _wAt[range]; k < _wWalk[range]; k++)
            {
                if (--budget < 0) return true;
                int b = (first + k) * 4;
                if (!(_wBox[b + 2] < x0 || x1 < _wBox[b] || _wBox[b + 3] < y0 || y1 < _wBox[b + 1])) return true;
            }
            int mode = range - 1;
            for (int k = _bModeStart[mode] + _bAt[mode]; k < _bModeStart[mode] + _bWalk[mode]; k++)
            {
                if (--budget < 0) return true;
                ref var e = ref _bEnt[k];
                if (!(e.X1 < x0 || x1 < e.X0 || e.Y1 < y0 || y1 < e.Y0)) return true;
            }
        }
        return false;
    }

    /// <summary>A static triangle's box on the screen, in the GTE's pixels, grown by
    /// the swell's reach; everything when a corner is near the eye.</summary>
    static void ScreenBox(in RetainedScene.View v, ReadOnlySpan<RetainedScene.Vertex> st, int i, float reach, Span<float> box)
    {
        float x0 = float.MaxValue, y0 = float.MaxValue, x1 = float.MinValue, y1 = float.MinValue, zmin = float.MaxValue;
        for (int j = 0; j < 3; j++)
        {
            ref readonly var p = ref st[i + j];
            double dx = p.X - v.CamX, dy = p.Y - v.CamY, dz = p.Z - v.CamZ;
            float z = (float)(v.R20 * dx + v.R21 * dy + v.R22 * dz) + v.Tz;
            if (z < RetainedScene.MainNear + reach + 1f)
            {
                box[0] = box[1] = float.MinValue; box[2] = box[3] = float.MaxValue;
                return;
            }
            float x = v.Cx + v.H * ((float)(v.R00 * dx + v.R01 * dy + v.R02 * dz) + v.Tx) / z;
            float y = v.Cy + v.H * ((float)(v.R10 * dx + v.R11 * dy + v.R12 * dz) + v.Ty) / z;
            x0 = Math.Min(x0, x); x1 = Math.Max(x1, x); y0 = Math.Min(y0, y); y1 = Math.Max(y1, y);
            zmin = Math.Min(zmin, z);
        }
        float m = 2f + reach * v.H / (zmin - reach);
        box[0] = x0 - m; box[1] = y0 - m; box[2] = x1 + m; box[3] = y1 + m;
    }
    float[] _wCentre = [];
    int _wCentreGen = -1;
    const int SortBuckets = 0x2000;

    /// <summary>
    /// The blended triangles of the frame's visible chunks and halves, sorted far to
    /// near by the key the game links a map face into its table with (its corners'
    /// mean view depth over four, 0x2000 entries; a quad's two triangles one face),
    /// and uploaded. Within a key the last built goes first, as the table draws the
    /// last packet linked into a slot first. A counting sort on each face's centre,
    /// which is kept per static triangle. False when none is left.
    /// </summary>
    unsafe bool SortWater(RetainedScene.Frame f, in RetainedScene.View v, byte[] halves)
    {
        var st = RetainedScene.Static;
        if (_wCentreGen != RetainedScene.StaticGeneration)
        {
            _wCentreGen = RetainedScene.StaticGeneration;
            if (_wCentre.Length < st.Length) _wCentre = new float[st.Length];
            for (int i = RetainedScene.StaticStart[1]; i + 2 < st.Length; i += 3)
            {
                _wCentre[i] = (st[i].X + st[i + 1].X + st[i + 2].X) / 3f;
                _wCentre[i + 1] = (st[i].Y + st[i + 1].Y + st[i + 2].Y) / 3f;
                _wCentre[i + 2] = (st[i].Z + st[i + 1].Z + st[i + 2].Z) / 3f;
                // A quad (0,1,2 then 1,3,2): both its triangles take its four corners' mean.
                if ((st[i].Flags & RetainedScene.FlagQuadTail) != 0 && i >= 3
                    && (st[i - 3].Flags & (RetainedScene.FlagQuadTail | (RetainedScene.HalfBits << RetainedScene.HalfShift)))
                       == (st[i].Flags & (RetainedScene.HalfBits << RetainedScene.HalfShift)))
                {
                    ref readonly var c3 = ref st[i + 1];
                    for (int a = 0; a < 3; a++)
                    {
                        float q = (3f * _wCentre[i - 3 + a] + (a == 0 ? c3.X : a == 1 ? c3.Y : c3.Z)) / 4f;
                        _wCentre[i - 3 + a] = q;
                        _wCentre[i + a] = q;
                    }
                }
            }
        }
        int total = 0;
        if (_wIdx.Length < st.Length) _wIdx = new uint[st.Length];
        if (_wTri.Length < st.Length / 3)
        {
            _wTri = new int[st.Length / 3]; _wKey = new int[st.Length / 3]; _wKeyAt = new int[st.Length / 3];
            _wTriBox = new float[st.Length / 3 * 4]; _wBox = new float[st.Length / 3 * 4];
        }
        Array.Clear(_wAt);
        Array.Clear(_wWalk);
        ResetPending();
        float reach = 0f;
        if (f.SwellOn)
            for (int w = 0; w < 3; w++) reach += Math.Abs(f.Swell[w * 4 + 2]);
        foreach (int range in (ReadOnlySpan<int>)[1, 2, 3, 4])
        {
            _wSortStart[range] = total;
            _wSortCount[range] = 0;
            int n = 0;
            for (int c = 0; c < RetainedScene.Chunks; c++)
            {
                int k = range * RetainedScene.Chunks + c;
                if (!_chunkVis[c] || RetainedScene.ChunkCount[k] == 0) continue;
                int end = RetainedScene.ChunkStart[k] + RetainedScene.ChunkCount[k];
                for (int i = RetainedScene.ChunkStart[k]; i + 2 < end; i += 3)
                {
                    uint hid = (st[i].Flags >> RetainedScene.HalfShift) & RetainedScene.HalfBits;
                    if (hid == 0 || halves[hid - 1] == 0) continue;
                    float z = (float)(v.R20 * (_wCentre[i] - v.CamX) + v.R21 * (_wCentre[i + 1] - v.CamY)
                                    + v.R22 * (_wCentre[i + 2] - v.CamZ)) + v.Tz;
                    _wTri[n] = i;
                    _wKey[n] = Math.Clamp((int)(z * 0.25f), 0, SortBuckets - 1);
                    ScreenBox(v, st, i, reach, _wTriBox.AsSpan(n * 4, 4));
                    n++;
                }
            }
            if (n == 0) continue;
            // Far first: count by key, then place from the far end.
            Array.Clear(_wBucket);
            for (int t = 0; t < n; t++) _wBucket[SortBuckets - 1 - _wKey[t]]++;
            int at = 0;
            for (int b = 0; b < SortBuckets; b++) { int c = _wBucket[b]; _wBucket[b] = at; at += c; }
            for (int t = n - 1; t >= 0; t--)
            {
                int dst = total + 3 * _wBucket[SortBuckets - 1 - _wKey[t]]++;
                _wKeyAt[dst / 3] = _wKey[t];
                _wTriBox.AsSpan(t * 4, 4).CopyTo(_wBox.AsSpan(dst / 3 * 4, 4));
                uint i = (uint)_wTri[t];
                _wIdx[dst] = i; _wIdx[dst + 1] = i + 1; _wIdx[dst + 2] = i + 2;
            }
            _wSortCount[range] = 3 * n;
            total += 3 * n;
        }
        if (total == 0) return false;
        if (_wEbo == 0) _wEbo = _gl.GenBuffer();
        _gl.BindVertexArray(_worldVao);
        _gl.BindBuffer(BufferTargetARB.ElementArrayBuffer, _wEbo);
        fixed (uint* p = _wIdx)
            _gl.BufferData(BufferTargetARB.ElementArrayBuffer, (nuint)(total * 4), p, BufferUsageARB.StreamDraw);
        _gl.BindVertexArray(0);
        RetainedScene.MainWaterSorted += total / 3;
        return true;
    }

    /// <summary>0078's ripples for a range in water's blend, with the frame's camera
    /// and clock, as GlCore sends them for a batch of water packets.</summary>
    void SendWorldWaves(bool on, float cx, float cy, float h)
    {
        if (_uwWaveOn < 0) return;
        on &= WaterWaves.Active;
        _gl.Uniform1(_uwWaveOn, on ? 1 : 0);
        if (!on) return;
        _gl.Uniform1(_uwWaveN, WaterWaves.RectN);
        _gl.Uniform4(_uwWaveRect, (uint)WaterWaves.RectN, new ReadOnlySpan<float>(WaterWaves.Rects, 0, WaterWaves.RectN * 4));
        _gl.UniformMatrix3(_uwWaveR, 1, true, WaterWaves.R);
        _gl.Uniform3(_uwWaveCam, WaterWaves.CamX, WaterWaves.CamY, WaterWaves.CamZ);
        _gl.Uniform3(_uwWaveT, WaterWaves.Tx, WaterWaves.Ty, WaterWaves.Tz);
        _gl.Uniform1(_uwWaveTime, WaterWaves.Time);
        _gl.Uniform4(_uwWaveParams, WaterWaves.Distort, Math.Max(16f, WaterWaves.Scale), WaterWaves.Shade, 0f);
        _gl.Uniform2(_uwWaveCentre, cx, cy);
        _gl.Uniform1(_uwWaveH, Math.Max(1f, h));
        WaterWaves.Batches++;
    }

    /// <summary>
    /// 0068's plane finder is fed by the water the frame draws, and the frame's water
    /// is not packets now: its visible triangles, at rest, cut at the near plane and
    /// handed over as DrawTri hands a water packet's.
    /// </summary>
    void NoteWaterPlane(RetainedScene.Frame f, GlDisplayRt rt, float cx, float cy)
    {
        var v = f.View;
        var st = RetainedScene.Static;
        float h = Math.Max(1f, v.H);
        float left = -cx, top = -cy, right = rt.Wide1x - cx, bottom = rt.H - cy;
        Span<float> px = stackalloc float[3], py = stackalloc float[3], pz = stackalloc float[3];
        Span<float> ox = stackalloc float[4], oy = stackalloc float[4], oz = stackalloc float[4];
        const float near = 16f;
        foreach (int range in (ReadOnlySpan<int>)[1, 4])
            for (int c = 0; c < RetainedScene.Chunks; c++)
            {
                int k = range * RetainedScene.Chunks + c;
                if (!_chunkVis[c] || RetainedScene.ChunkCount[k] == 0) continue;
                int end = RetainedScene.ChunkStart[k] + RetainedScene.ChunkCount[k];
                for (int i = RetainedScene.ChunkStart[k]; i + 2 < end; i += 3)
                {
                    uint flags = st[i].Flags;
                    if ((flags & RetainedScene.FlagWater) == 0) continue;
                    uint hid = (flags >> RetainedScene.HalfShift) & RetainedScene.HalfBits;
                    if (hid == 0 || f.MainHalves[hid - 1] == 0) continue;
                    int behind = 0;
                    for (int j = 0; j < 3; j++)
                    {
                        ref readonly var p = ref st[i + j];
                        double dx = p.X - v.CamX, dy = p.Y - v.CamY, dz = p.Z - v.CamZ;
                        px[j] = (float)(v.R00 * dx + v.R01 * dy + v.R02 * dz) + v.Tx;
                        py[j] = (float)(v.R10 * dx + v.R11 * dy + v.R12 * dz) + v.Ty;
                        pz[j] = (float)(v.R20 * dx + v.R21 * dy + v.R22 * dz) + v.Tz;
                        if (pz[j] < near) behind++;
                    }
                    if (behind == 3) continue;
                    // The triangle in front of the near plane, as a fan.
                    int n = 0;
                    for (int j = 0; j < 3; j++)
                    {
                        int q = (j + 1) % 3;
                        bool inJ = pz[j] >= near, inQ = pz[q] >= near;
                        if (inJ) { ox[n] = px[j]; oy[n] = py[j]; oz[n] = pz[j]; n++; }
                        if (inJ != inQ && n < 4)
                        {
                            float t = (near - pz[j]) / (pz[q] - pz[j]);
                            ox[n] = px[j] + t * (px[q] - px[j]); oy[n] = py[j] + t * (py[q] - py[j]); oz[n] = near; n++;
                        }
                    }
                    for (int j = 1; j + 1 < n; j++)
                    {
                        PlanarReflections.NoteWater(h * ox[0] / oz[0], h * oy[0] / oz[0], oz[0],
                            h * ox[j] / oz[j], h * oy[j] / oz[j], oz[j],
                            h * ox[j + 1] / oz[j + 1], h * oy[j + 1] / oz[j + 1], oz[j + 1],
                            left, top, right, bottom);
                        RetainedScene.MainWaterNoted++;
                    }
                }
            }
    }

    // ---- the models ---------------------------------------------------------------

    // The frames' models, one buffer per frame in the ring, so the normal pass at
    // present still finds the models of the frame the presented target holds.
    // The mirror's are drawn at once, into one buffer past the ring.
    const int ModelRing = 4, MirrorSlot = ModelRing;
    readonly uint[] _mdlVbo = new uint[ModelRing + 1], _mdlMipVbo = new uint[ModelRing + 1], _mdlVao = new uint[ModelRing + 1];
    readonly int[] _mdlSerial = new int[ModelRing + 1], _mdlCap = new int[ModelRing + 1];
    uint[] _mdlMip = [];
    int _uwBk, _uwLcmR, _uwLcmG, _uwLcmB;

    /// <summary>The frame's models into their buffer, with their mip-atlas entries
    /// looked up and decoded; the buffer's slot, or -1 for none. Before the target is
    /// bound, since the decode draws.</summary>
    unsafe int UploadModels(RetainedScene.ModelRuns runs, int slot, int serial, bool mips)
    {
        int n = runs.Count;
        if (n == 0) return -1;
        if (_mdlVao[slot] == 0)
        {
            _mdlVbo[slot] = _gl.GenBuffer();
            _mdlMipVbo[slot] = _gl.GenBuffer();
            _mdlVao[slot] = MakeWorldVao(_mdlVbo[slot], _mdlMipVbo[slot]);
        }
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _mdlVbo[slot]);
        if (n > _mdlCap[slot])
        {
            _mdlCap[slot] = Math.Max(n, _mdlCap[slot] * 2);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_mdlCap[slot] * sizeof(RetainedScene.Vertex)), null, BufferUsageARB.StreamDraw);
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _mdlMipVbo[slot]);
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(_mdlCap[slot] * 4), null, BufferUsageARB.StreamDraw);
            _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _mdlVbo[slot]);
        }
        _gl.BufferSubData<RetainedScene.Vertex>(BufferTargetARB.ArrayBuffer, 0, new ReadOnlySpan<RetainedScene.Vertex>(runs.Tris, 0, n));
        if (_mdlMip.Length < n) _mdlMip = new uint[Math.Max(n, _mdlMip.Length * 2)];
        for (int i = 0; i + 2 < n; i += 3)
        {
            uint e = mips ? DynMip(runs.Tris[i]) : 0u;
            _mdlMip[i] = _mdlMip[i + 1] = _mdlMip[i + 2] = e;
        }
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _mdlMipVbo[slot]);
        _gl.BufferSubData<uint>(BufferTargetARB.ArrayBuffer, 0, new ReadOnlySpan<uint>(_mdlMip, 0, n));
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);
        if (mips && _mip!.HasPending) _mip.Process(_vram.SampleTexture);
        _mdlSerial[slot] = serial;
        return slot;
    }

    /// <summary>
    /// The frame's models after the map, in the state the map left: tested and written,
    /// with 0051's tolerance as their packets had it (true depth first with colour off,
    /// then colour against it, pulled towards the camera), so a model flush with the
    /// floor wins as the later table entry did. Not culled: the port kept only the faces
    /// the game's own facing test kept. A run per BK and LCM, which light the dots.
    /// </summary>
    void DrawWorldModels(RetainedScene.ModelRuns f, int slot)
    {
        _gl.Disable(EnableCap.CullFace);
        _gl.BindVertexArray(_mdlVao[slot]);
        if (_uwMipIndirect >= 0) _gl.Uniform1(_uwMipIndirect, 0);
        bool bias = GteDepth.ZBuffer && (GteDepth.DepthBias > 0f || GteDepth.DepthSlope > 0f);
        ProbeModelsUnderMap(() => DrawModelRuns(f));
        if (bias)
        {
            DepthOnly(true);
            DrawModelRuns(f);
            DepthOnly(false);
            _gl.DepthMask(false);
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, GteDepth.DepthBias / 65536f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, GteDepth.DepthSlope);
        }
        if (bias) ProbeTolerance(1, () => DrawModelRuns(f));
        MarkModels(true);
        foreach (var g in f.Groups)
        {
            if (_uwBk >= 0) _gl.Uniform3(_uwBk, g.Bk0, g.Bk1, g.Bk2);
            if (_uwLcmR >= 0) _gl.Uniform3(_uwLcmR, g.L0, g.L1, g.L2);
            if (_uwLcmG >= 0) _gl.Uniform3(_uwLcmG, g.L3, g.L4, g.L5);
            if (_uwLcmB >= 0) _gl.Uniform3(_uwLcmB, g.L6, g.L7, g.L8);
            if (g.Cull) _gl.Enable(EnableCap.CullFace);
            _gl.DrawArrays(PrimitiveType.Triangles, g.Start, (uint)g.Count);
            if (g.Cull) _gl.Disable(EnableCap.CullFace);
        }
        MarkModels(false);
        if (bias)
        {
            if (_uwDepthBias >= 0) _gl.Uniform1(_uwDepthBias, 0f);
            if (_uwDepthSlope >= 0) _gl.Uniform1(_uwDepthSlope, 0f);
            _gl.DepthMask(true);
        }
    }

    /// <summary>Every run of the frame's models through the bound program, culled
    /// where the port could not cull (the facing set up as the map's).</summary>
    void DrawModelRuns(RetainedScene.ModelRuns f)
    {
        foreach (var g in f.Groups)
        {
            if (g.Cull) _gl.Enable(EnableCap.CullFace);
            _gl.DrawArrays(PrimitiveType.Triangles, g.Start, (uint)g.Count);
            if (g.Cull) _gl.Disable(EnableCap.CullFace);
        }
    }

    // ---- the normal and surface buffers ------------------------------------------

    void InitWorldNormals()
    {
        _progWorldNrm = GlShaders.Build(_gl, GlShaders.WorldNormalVs, GlShaders.NormalFs, "worldnormal");
        if (_progNormal != 0)
        {
            _uNrmDepthCull = _gl.GetUniformLocation(_progNormal, "uDepthCull");
            _uNrmDepthStep = _gl.GetUniformLocation(_progNormal, "uDepthStep");
            _gl.UseProgram(_progNormal);
            int u = _gl.GetUniformLocation(_progNormal, "uFrameDepth");
            if (u >= 0) _gl.Uniform1(u, FrameDepthUnit);
            if (_uNrmDepthCull >= 0) _gl.Uniform1(_uNrmDepthCull, 0);
        }
        if (_progWorldNrm == 0) return;
        int L(string n) => _gl.GetUniformLocation(_progWorldNrm, n);
        _uwnR = L("uR"); _uwnCam = L("uCam"); _uwnT = L("uT"); _uwnH = L("uH"); _uwnC = L("uC"); _uwnFb = L("uFb");
        _uwnNear = L("uNear"); _uwnHalfGate = L("uHalfGate"); _uwnSnap = L("uWorldSnap");
        _uwnProjH = L("uProjH"); _uwnCentre = L("uCentre"); _uwnScale = L("uScale");
        _uwnDepthCull = L("uDepthCull"); _uwnDepthStep = L("uDepthStep");
        _uwnSwellOn = L("uSwellOn"); _uwnSwell = L("uSwell"); _uwnZSlice = L("uZSlice");
        _uwnFade = L("uFade"); _uwnFadeZ = L("uFadeZ");
        _gl.UseProgram(_progWorldNrm);
        void Unit(string n, int v) { int l = L(n); if (l >= 0) _gl.Uniform1(l, v); }
        Unit("uVram", 0); Unit("uHalves", HalvesUnit); Unit("uFrameDepth", FrameDepthUnit); Unit("uVeilPass", 0);
        Unit("uSwellOn", 0); Unit("uModelBlend", 0);
        _gl.UseProgram(0);
    }

    /// <summary>
    /// Before the normal pass draws the target's triangles: bind the frame's depth
    /// and, when the map was drawn on the GPU into this target, draw it first. The
    /// triangles after it then drop what lies behind the depth, since the order they
    /// are drawn in no longer says which surface is in front. False with no map, and
    /// the pass is the one it was.
    /// </summary>
    bool DrawWorldNormals(GlDisplayRt src, int scale)
    {
        var geo = src.Geo;
        if (geo.WorldSerial == 0 || src.Depth == 0 || !RetainedScene.MainSurfaces) return false;
        _gl.ActiveTexture(TextureUnit.Texture0 + FrameDepthUnit);
        _gl.BindTexture(TextureTarget.Texture2D, src.Depth);
        _gl.ActiveTexture(TextureUnit.Texture0);
        float stepX = (float)src.TexW / Math.Max(1, src.NormalW), stepY = (float)src.TexH / Math.Max(1, src.NormalH);

        var f = RetainedScene.Find(geo.WorldSerial);
        _wnReady = false;
        _wnFrame = f;
        if (_progWorldNrm != 0 && f != null)
        {
            _wnReady = true;
            var v = f.View;
            Span<float> r = [v.R00, v.R01, v.R02, v.R10, v.R11, v.R12, v.R20, v.R21, v.R22];
            CullChunks(r, v.CamX, v.CamY, v.CamZ, v.Tx, v.Ty, v.Tz, v.H, geo.WorldCx, geo.WorldCy, src.Wide1x, src.H,
                       false, 0f, v.H, float.PositiveInfinity);
            _gl.ActiveTexture(TextureUnit.Texture0 + HalvesUnit);
            _gl.BindTexture(TextureTarget.Texture2D, _halvesTex);
            _gl.TexSubImage2D<byte>(TextureTarget.Texture2D, 0, 0, 0, RetainedScene.HalvesW, RetainedScene.HalvesH,
                PixelFormat.RedInteger, PixelType.UnsignedByte, f.MainHalves);
            _gl.ActiveTexture(TextureUnit.Texture0);

            _gl.UseProgram(_progWorldNrm);
            if (_uwnR >= 0) _gl.UniformMatrix3(_uwnR, 1, true, r);
            if (_uwnCam >= 0) _gl.Uniform3(_uwnCam, (float)v.CamX, (float)v.CamY, (float)v.CamZ);
            if (_uwnT >= 0) _gl.Uniform3(_uwnT, (float)v.Tx, v.Ty, v.Tz);
            if (_uwnH >= 0) _gl.Uniform1(_uwnH, v.H);
            if (_uwnC >= 0) _gl.Uniform2(_uwnC, geo.WorldCx, geo.WorldCy);
            if (_uwnModelGteC >= 0) _gl.Uniform2(_uwnModelGteC, v.Cx, v.Cy);
            if (_uwnFb >= 0) _gl.Uniform2(_uwnFb, (float)src.Wide1x, src.H);
            if (_uwnNear >= 0) _gl.Uniform1(_uwnNear, RetainedScene.MainNear);
            if (_uwnHalfGate >= 0) _gl.Uniform1(_uwnHalfGate, 1);
            if (_uwnSnap >= 0) _gl.Uniform1(_uwnSnap, GteDepth.Subpixel ? 0 : 1);
            if (_uwnProjH >= 0) _gl.Uniform1(_uwnProjH, Math.Max(1f, v.H));
            if (_uwnCentre >= 0) _gl.Uniform2(_uwnCentre, geo.WorldCx, geo.WorldCy);
            if (_uwnScale >= 0) _gl.Uniform1(_uwnScale, (float)scale);
            if (_uwnDepthCull >= 0) _gl.Uniform1(_uwnDepthCull, 1);
            if (_uwnDepthStep >= 0) _gl.Uniform2(_uwnDepthStep, stepX, stepY);
            if (_uwnZSlice >= 0) _gl.Uniform2(_uwnZSlice, 0f, 0f);
            SendFade(_uwnFade, _uwnFadeZ, f);
            SendWorldSwell(f, _uwnSwellOn, _uwnSwell);
            if (RetainedScene.CullBack)
            {
                _gl.Enable(EnableCap.CullFace);
                _gl.FrontFace(FrontFaceDirection.Ccw);
                _gl.CullFace(TriangleFace.Back);
            }
            RetainedScene.MainNormalTriangles += DrawStaticChunks(0) / 3;
            _gl.Disable(EnableCap.CullFace);
            int ms = f.Serial & (ModelRing - 1);
            if (f.Models.Count > 0 && _mdlSerial[ms] == f.Serial && RetainedScene.MainModelsShown)
            {
                _gl.BindVertexArray(_mdlVao[ms]);
                DrawModelRuns(f.Models);
                RetainedScene.MainModelNormalTriangles += f.Models.Count / 3;
            }
            if (RetainedScene.MainModelsShown) DrawInstanceNormals(f);
            _gl.BindVertexArray(0);
        }

        _gl.UseProgram(_progNormal);
        if (_uNrmDepthCull >= 0) _gl.Uniform1(_uNrmDepthCull, 1);
        if (_uNrmDepthStep >= 0) _gl.Uniform2(_uNrmDepthStep, stepX, stepY);
        return true;
    }

    /// <summary>The map's water into the normal and surface buffers, one slice of view
    /// depth, where the list says the colour pass drew it; and the models' blended faces
    /// that are a surface (<see cref="RetainedScene.ModelInstance.BlendSurfaces"/>) in the
    /// same slice. Only those: WorldNormalVs drops every other blended face. The world
    /// normal program keeps what <see cref="DrawWorldNormals"/> set.</summary>
    void DrawWorldWaterNormals(float lo, float hi)
    {
        DrawBlendNormals(lo, hi);
        _gl.UseProgram(_progWorldNrm);
        if (_uwnZSlice >= 0)
            _gl.Uniform2(_uwnZSlice, float.IsNegativeInfinity(lo) ? -1f : lo, float.IsPositiveInfinity(hi) ? 1e30f : hi);
        if (RetainedScene.CullBack)
        {
            _gl.Enable(EnableCap.CullFace);
            _gl.FrontFace(FrontFaceDirection.Ccw);
            _gl.CullFace(TriangleFace.Back);
        }
        foreach (int range in (ReadOnlySpan<int>)[1, 4])
            if (RetainedScene.StaticCount[range] > 0) RetainedScene.MainNormalTriangles += DrawStaticChunks(range) / 3;
        _gl.Disable(EnableCap.CullFace);
        if (_uwnZSlice >= 0) _gl.Uniform2(_uwnZSlice, 0f, 0f);
    }

    RetainedScene.Frame? _wnFrame;
    int _uwnModelBlend = -2, _uwnModelSolid, _uwnModelTwin;

    /// <summary>The frame's instances' blended faces that are a surface, in a slice of view
    /// depth; each pixel's own depth decides, as for the water.</summary>
    void DrawBlendNormals(float lo, float hi)
    {
        var f = _wnFrame;
        int slot = f == null ? 0 : f.Serial & (ModelRing - 1);
        if (f == null || _uwnModel < 0 || !RetainedScene.MainModelsShown || !RetainedScene.BlendShown
            || _mvSerial[slot] != f.Serial || _mvGen[slot] != _meshGen) return;
        bool any = false;
        foreach (var m in f.Instances) any |= m.BlendSurfaces && m.MeshGen == _meshGen;
        if (!any) return;
        if (_uwnModelBlend == -2)
        {
            _uwnModelBlend = _gl.GetUniformLocation(_progWorldNrm, "uModelBlend");
            _uwnModelSolid = _gl.GetUniformLocation(_progWorldNrm, "uModelSolid");
            _uwnModelTwin = _gl.GetUniformLocation(_progWorldNrm, "uModelTwin");
        }
        if (_uwnModelBlend < 0) return;
        _gl.UseProgram(_progWorldNrm);
        if (_uwnZSlice >= 0)
            _gl.Uniform2(_uwnZSlice, float.IsNegativeInfinity(lo) ? -1f : lo, float.IsPositiveInfinity(hi) ? 1e30f : hi);
        BeginInstances(slot, false);
        _gl.Uniform1(_uwnModelBlend, 1);
        foreach (var m in f.Instances)
        {
            if (!m.BlendSurfaces || m.MeshGen != _meshGen) continue;
            SendInstance(m, false);
            _gl.Uniform1(_uwnModelSolid, m.Solid ? 1 : 0);
            _gl.Uniform1(_uwnModelTwin, m.TwinMode - 1);
            _gl.DrawArrays(PrimitiveType.Triangles, m.MeshStart + m.MeshCount, (uint)(m.MeshAll - m.MeshCount));
            RetainedScene.BlendNormalInstances++;
        }
        _gl.Uniform1(_uwnModelBlend, 0);
        EndInstances(false);
        if (_uwnZSlice >= 0) _gl.Uniform2(_uwnZSlice, 0f, 0f);
    }

    float[]? _chkSurf, _chkDepth;

    /// <summary>A readback's saved state: the read framebuffer, that framebuffer's read
    /// buffer and the pack alignment, put back by <see cref="Restore"/>.</summary>
    readonly struct ReadState
    {
        readonly int _fbo, _buf, _align;
        public ReadState(GL gl)
        {
            _fbo = gl.GetInteger(GLEnum.ReadFramebufferBinding);
            _buf = gl.GetInteger(GLEnum.ReadBuffer);
            _align = gl.GetInteger(GLEnum.PackAlignment);
            gl.PixelStore(PixelStoreParameter.PackAlignment, 4);
        }
        public void Restore(GL gl)
        {
            gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, (uint)_fbo);
            gl.ReadBuffer((ReadBufferMode)_buf);
            gl.PixelStore(PixelStoreParameter.PackAlignment, _align);
        }
    }

    /// <summary>The probe's: the surface buffer just drawn, read back against the
    /// target's depth on a 4-pixel grid. Waits on the GPU; the probe only.</summary>
    unsafe void CheckSurfaces(GlDisplayRt src)
    {
        RetainedScene.SurfaceCheck = false;
        int sw = src.NormalW, sh = src.NormalH, dw = src.TexW, dh = src.TexH;
        if (_chkSurf == null || _chkSurf.Length < sw * sh * 4) _chkSurf = new float[sw * sh * 4];
        if (_chkDepth == null || _chkDepth.Length < dw * dh) _chkDepth = new float[dw * dh];
        var state = new ReadState(_gl);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, src.NormalFbo);
        int normalBuf = _gl.GetInteger(GLEnum.ReadBuffer);
        _gl.ReadBuffer(ReadBufferMode.ColorAttachment1);
        fixed (float* p = _chkSurf) _gl.ReadPixels(0, 0, (uint)sw, (uint)sh, PixelFormat.Rgba, PixelType.Float, p);
        _gl.ReadBuffer((ReadBufferMode)normalBuf);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, src.Fbo);
        fixed (float* p = _chkDepth) _gl.ReadPixels(0, 0, (uint)dw, (uint)dh, PixelFormat.DepthComponent, PixelType.Float, p);
        state.Restore(_gl);
        for (int y = 0; y < sh; y += 4)
        for (int x = 0; x < sw; x += 4)
        {
            float d = _chkDepth[Math.Min(dh - 1, y * dh / sh) * dw + Math.Min(dw - 1, x * dw / sw)];
            if (d >= 0.99999f) continue;
            RetainedScene.SurfaceDepthPixels++;
            int i = (y * sw + x) * 4;
            float z = _chkSurf[i + 2] * 65536f, id = _chkSurf[i + 3], dz = d * 65536f;
            RetainedScene.SurfaceIds[id < 0.5f ? 0 : id < 1.5f ? 1 : id < 2.5f ? 2 : id < 3.5f ? 3 : id < 255.5f ? 4 : 5]++;
            if (id < 0.5f || z <= 0f) { if (id < 2.5f || id > 3.5f) RetainedScene.SurfaceMissing++; continue; }
            if (z > dz + 64f + dz / 64f) RetainedScene.SurfaceBehind++;
        }
        RetainedScene.SurfaceChecks++;
    }

    float[]? _probeDepth, _probeSurf;

    /// <summary>0085's probe at one stage: the target's depth on a 4-pixel grid, and at
    /// present the surface buffer, only where a pass drew it this frame. No image, and
    /// no state left changed.</summary>
    unsafe void ProbeDepthStage(int stage, GlDisplayRt? rt, int serial, bool surfaceFresh = false)
    {
        int dw = rt?.TexW ?? 0, dh = rt?.TexH ?? 0;
        RetainedScene.ProbeRuns[stage]++;
        RetainedScene.ProbeTargetFbo[stage] = rt?.Fbo ?? 0;
        RetainedScene.ProbeFrame[stage] = _frame;
        RetainedScene.ProbeTargetSerial[stage] = serial;
        RetainedScene.ProbeTargetW[stage] = dw;
        RetainedScene.ProbeTargetH[stage] = dh;
        bool absent = rt == null || rt.Fbo == 0 || dw <= 0 || dh <= 0;
        bool depth = !absent && rt!.Depth != 0;
        int sw = rt?.NormalW ?? 0, sh = rt?.NormalH ?? 0;
        bool hasSurface = !absent && rt!.NormalFbo != 0 && rt.Surface != 0 && sw > 0 && sh > 0;
        // Ids come only from a fresh pass: an attachment left from an older frame is stale.
        bool surface = hasSurface && stage == RetainedScene.ProbePresent && surfaceFresh;
        if (absent) RetainedScene.ProbeTargetAbsent[stage]++;
        else if (!depth) RetainedScene.ProbeDepthAbsent[stage]++;

        if (depth || hasSurface)
        {
            var state = new ReadState(_gl);
            if (depth)
            {
                if (_probeDepth == null || _probeDepth.Length < (long)dw * dh) _probeDepth = new float[(long)dw * dh];
                _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, rt!.Fbo);
                fixed (float* p = _probeDepth)
                    _gl.ReadPixels(0, 0, (uint)dw, (uint)dh, PixelFormat.DepthComponent, PixelType.Float, p);
            }
            if (surface)
            {
                if (_probeSurf == null || _probeSurf.Length < (long)sw * sh * 4) _probeSurf = new float[(long)sw * sh * 4];
                _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, rt!.NormalFbo);
                int normalBuf = _gl.GetInteger(GLEnum.ReadBuffer);
                _gl.ReadBuffer(ReadBufferMode.ColorAttachment1);
                fixed (float* p = _probeSurf)
                    _gl.ReadPixels(0, 0, (uint)sw, (uint)sh, PixelFormat.Rgba, PixelType.Float, p);
                _gl.ReadBuffer((ReadBufferMode)normalBuf);
            }
            state.Restore(_gl);

            for (int y = 0; y < dh; y += 4)
            for (int x = 0; x < dw; x += 4)
            {
                RetainedScene.ProbeSamples[stage]++;
                if (depth && _probeDepth![(long)y * dw + x] < 0.99999f) RetainedScene.ProbeDepth[stage]++;
                if (!hasSurface) { RetainedScene.ProbeSurfaceAbsent[stage]++; continue; }
                if (!surface) { RetainedScene.ProbeSurfaceStale[stage]++; continue; }
                int sx = Math.Min(sw - 1, (int)((long)x * sw / dw)), sy = Math.Min(sh - 1, (int)((long)y * sh / dh));
                float id = _probeSurf![((long)sy * sw + sx) * 4 + 3];
                RetainedScene.ProbeSurfaceIds[stage * 6 + (id < 0.5f ? 0 : id < 1.5f ? 1 : id < 2.5f ? 2 : id < 3.5f ? 3 : id < 255.5f ? 4 : 5)]++;
            }
        }
        if (stage == RetainedScene.ProbePresent) ProbeDepthPair();
    }

    /// <summary>0085's probe: whether the present reading's target and serial were the
    /// after-main reading's. A pair that did not still counts its own two stages.</summary>
    void ProbeDepthPair()
    {
        RetainedScene.ProbePairRuns++;
        if (RetainedScene.ProbeTargetFbo[RetainedScene.ProbePresent] == RetainedScene.ProbeTargetFbo[RetainedScene.ProbeMain])
            RetainedScene.ProbePairFboMatch++;
        else RetainedScene.ProbePairFboMismatch++;
        if (RetainedScene.ProbeTargetSerial[RetainedScene.ProbePresent] == RetainedScene.ProbeTargetSerial[RetainedScene.ProbeMain])
            RetainedScene.ProbePairSerialMatch++;
        else RetainedScene.ProbePairSerialMismatch++;
    }
}
