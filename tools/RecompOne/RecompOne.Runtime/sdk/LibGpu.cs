using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Sdk;

public static class LibGpu
{
    private static readonly DrawEnvEvent _drawEnvEvent = new();
    private static readonly DispEnvEvent _dispEnvEvent = new();

    public static void DrawOTag(CpuContext c, IMemory m)
    {
        //0045.
        var profile = Profiler.Begin(Profiler.DrawOTag);
        DrawOTagCore(c, m);
        Profiler.End(profile);
    }

    private static void DrawOTagCore(CpuContext c, IMemory m)
    {
        if (Runtime.Gpu == null) return;
        if (Log.SdkOn) Log.Sdk($"DrawOTag ot=0x{c.A0:X8}");
        WalkOTag(m, c.A0, null);
    }

    /// <summary>
    /// The ordering table's walk, back to front, for <see cref="DrawOTag"/> and for a
    /// port that replaces it. <paramref name="onEntry"/>, if given, is told the entry
    /// (counted from the head) before each packet is sent, which is the only way to
    /// know where a primitive came from. 0079: while <see cref="BlendOrder.Active"/>,
    /// a blended packet the depth buffer tests is sent after the opaque tested ones
    /// that follow it, and is reported with its own entry when it is.
    /// </summary>
    public static void WalkOTag(IMemory m, uint head, Action<int>? onEntry)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null) return;

        var addr = head & Runtime.RamWordMask;
        var custom = GpuPrims.Any && GpuPrims.OtLength > 0;
        var otBase = GpuPrims.OtBase & Runtime.RamWordMask;
        var otEnd = otBase + (uint)GpuPrims.OtLength * 4u;
        // An asset pack's own primitives are emitted by entry, so nothing may move.
        var reorder = BlendOrder.Active && !custom;
        var probe = BlendOrder.Probe && !custom && GtePacketDepth.Active;
        if (probe) BlendOrder.ProbeBegin();
        // A walk cut short by an exception must not hand the next one its packets.
        BlendOrder.Clear();
        BlendOrder.Walks++;

        var slot = -1;
        // 0085. The map's water, drawn by the backend where its packets would be sent.
        var water = false;
        // 0085. The first-person arm, drawn by the backend face by face where the walk
        // would have sent its packets: the runs it has passed go in together before the
        // next packet the walk sends that draws where the arm does, since nothing between
        // them shares a pixel with it.
        var arm = RetainedScene.ArmSerial > 0 && RetainedScene.ArmSerial == RetainedScene.MainSerial
                  && !custom && !PlanarReflections.Capturing;
        var armDue = false;
        // 0096. The last slot the game linked to be drawn before the world; -1 until a
        // main view has drawn.
        var underTo = -1;
        for (var guard = 0; guard < 0x100000; guard++)
        {
            // Where in the table this primitive was linked, counted from the head —
            // which is the far end, since the walk goes back to front. It is the
            // game's own opinion of the primitive's depth, and the only thing that
            // can contradict a recovered SZ. See GteDepth.OtEntry.
            GteDepth.OtEntry = guard;

            if (custom && addr >= otBase && addr < otEnd)
                gpu.EmitCustomOrder((int)((addr - otBase) >> 2));

            var header = m.ReadU32(addr);
            var count = (int)(header >> 24);
            if (count == 0)
            {
                slot++;
                // 0085. The map, past the sky and ahead of everything tested against it;
                // in a planar capture, the mirror's.
                if (slot == 1 && !custom && PlanarReflections.Capturing && RetainedScene.MirrorSerial > 0)
                {
                    if (!gpu.DrawRetainedMain()) RetainedScene.MirrorMissed++;
                    else underTo = slot + RetainedScene.UnderSlots;
                    RetainedScene.MirrorSerial = 0;
                }
                else if (slot == 1 && RetainedScene.MainSerial > 0 && !custom && !PlanarReflections.Capturing)
                {
                    if (!gpu.DrawRetainedMain()) RetainedScene.MainMissed++;
                    else
                    {
                        water = RetainedScene.WaterPending && reorder;
                        underTo = slot + RetainedScene.UnderSlots;
                    }
                    RetainedScene.MainSerial = 0;
                }
                RetainedScene.UnderWorld = slot > 1 && slot <= underTo;
                armDue |= arm && slot >= RetainedScene.ArmSlot && slot >= 1;
            }
            GteDepth.OtSlot = slot;

            if (count > 0)
            {
                // The arm is a barrier, as its packets were: what the walk holds goes first.
                if (armDue && Draws(m, addr) && ArmMeets(m, addr, count))
                {
                    if (BlendOrder.Queued > 0) SendHeld(gpu, m, onEntry, probe, guard, slot, ref water);
                    if (water) water = gpu.DrawRetainedWater(WaterCut(slot));
                    arm = DrawArm(gpu, slot);
                    armDue = false;
                }
                // Slot 0 (the skybox) is never depth-tested, whatever it recorded.
                var kind = (reorder || probe) && slot != 0 ? BlendOrder.Classify(m, addr, count, out _) : BlendOrder.Kind.Barrier;
                if (reorder && kind == BlendOrder.Kind.Deferred)
                    BlendOrder.Defer(addr, count, guard, slot);
                else
                {
                    if (BlendOrder.Queued > 0)
                    {
                        if (kind == BlendOrder.Kind.Opaque) BlendOrder.Passed++;
                        else SendHeld(gpu, m, onEntry, probe, guard, slot, ref water);
                    }
                    // The water walked before a barrier that draws goes before it, as
                    // the held packets do; not before one under the world (0096),
                    // which the water is drawn over.
                    if (water && kind == BlendOrder.Kind.Barrier && !RetainedScene.UnderWorld && Draws(m, addr))
                        water = gpu.DrawRetainedWater(WaterCut(slot));
                    onEntry?.Invoke(guard);
                    if (probe) BlendOrder.ProbePacket(m, addr, count);
                    SendPacket(gpu, m, addr, count);
                }
            }

            var next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = next & Runtime.RamWordMask;
        }
        if (BlendOrder.Queued > 0) SendHeld(gpu, m, onEntry, probe, GteDepth.OtEntry, GteDepth.OtSlot, ref water);
        if (water) gpu.DrawRetainedWater(float.NegativeInfinity);
        if (arm) DrawArm(gpu, int.MaxValue);

        // The length is only known once the walk ends, so it is published for the
        // next one. An entry is readable as an OTZ against it: the walk starts at
        // the far end, so otz = length - 1 - entry.
        if (GteDepth.OtEntry >= 0) GteDepth.OtLength = GteDepth.OtEntry + 1;
        GteDepth.OtEntry = -1;
        GteDepth.OtSlot = -1;
        RetainedScene.UnderWorld = false;
        if (custom) GpuPrims.Clear();
    }

    /// <summary>0085. The arm's runs the walk has reached at <paramref name="slot"/>;
    /// false once none is left.</summary>
    private static bool DrawArm(Gpu gpu, int slot)
    {
        RetainedScene.ArmCut = slot;
        if (gpu.DrawRetainedArm()) return true;
        RetainedScene.ArmSerial = 0;
        return false;
    }

    private static bool ArmMeets(IMemory m, uint addr, int count)
    {
        var (x0, y0, x1, y1) = PacketBox(m, addr, count);
        return RetainedScene.ArmMeets(x0, y0, x1, y1);
    }

    /// <summary>0085. Whether a packet draws: a polygon, a line or a rectangle.</summary>
    private static bool Draws(IMemory m, uint addr)
    {
        uint op = m.ReadU32(addr + 4u) >> 24;
        return op >= 0x20u && op < 0x80u;
    }

    /// <summary>0085. The screen box of a packet of polygons, in their own
    /// coordinates; anything else covers everything.</summary>
    private static (float, float, float, float) PacketBox(IMemory m, uint addr, int count)
    {
        const float Lo = float.MinValue, Hi = float.MaxValue;
        float x0 = Hi, y0 = Hi, x1 = Lo, y1 = Lo;
        for (int at = 0; at < count;)
        {
            uint cmd = m.ReadU32(addr + 4u + 4u * (uint)at) >> 24;
            if ((cmd & 0xE0u) != 0x20u) return (Lo, Lo, Hi, Hi);
            bool gouraud = (cmd & 0x10u) != 0;
            int n = (cmd & 8u) != 0 ? 4 : 3, stride = 1 + ((cmd & 4u) != 0 ? 1 : 0) + (gouraud ? 1 : 0);
            int size = n * stride + (gouraud ? 0 : 1);
            if (at + size > count) return (Lo, Lo, Hi, Hi);
            for (int i = 0; i < n; i++)
            {
                uint w = m.ReadU32(addr + 4u + 4u * (uint)(at + 1 + i * stride));
                float x = (short)w, y = (short)(w >> 16);
                x0 = Math.Min(x0, x); x1 = Math.Max(x1, x); y0 = Math.Min(y0, y); y1 = Math.Max(y1, y);
            }
            at += size;
        }
        return x0 <= x1 ? (x0, y0, x1, y1) : (Lo, Lo, Hi, Hi);
    }

    /// <summary>0085. The view depth a map face is walked at a slot for: the table's
    /// 0x2000 entries walked from the far end, and a tile linked at its mean SZ over
    /// four plus 0xF0 (PolyAssembler). Water linked deeper was walked already.</summary>
    private static float WaterCut(int slot) => 4f * (0x1FFF - slot - 0xF0);

    /// <summary>0079. The held packets, each under its own entry and slot, then the walk's put back.
    /// 0085: the map's water the walk passed before each goes in ahead of it.</summary>
    private static void SendHeld(Gpu gpu, IMemory m, Action<int>? onEntry, bool probe, int entry, int slot, ref bool water)
    {
        foreach (var e in BlendOrder.Take())
        {
            if (water)
            {
                var (x0, y0, x1, y1) = PacketBox(m, e.Addr, e.Count);
                water = gpu.DrawRetainedWater(WaterCut(e.OtSlot), x0, y0, x1, y1);
            }
            GteDepth.OtEntry = e.OtEntry;
            GteDepth.OtSlot = e.OtSlot;
            onEntry?.Invoke(e.OtEntry);
            if (probe) BlendOrder.ProbePacket(m, e.Addr, e.Count);
            SendPacket(gpu, m, e.Addr, e.Count);
        }
        GteDepth.OtEntry = entry;
        GteDepth.OtSlot = slot;
    }

    private static void SendPacket(Gpu gpu, IMemory m, uint addr, int count)
    {
        if (m is PSMemory ram && ram.TryWords(addr + 4u, count, out var words))
        {
            gpu.WriteGp0Packet(words, addr + 4u);
            return;
        }
        // 0012. The slow path has to carry the source address too, or
        // every vertex in a packet that took it misses the map.
        for (var i = 0; i < count; i++)
        {
            var src = addr + 4u + (uint)i * 4u;
            gpu.WriteGp0(m.ReadU32(src), src);
        }
    }

    public static void DrawSync(CpuContext c, IMemory m)
    {
        if (Log.SdkOn) Log.Sdk($"DrawSync({(int)c.A0})");
        c.V0 = 0;
    }

    public static void PutDrawEnv(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = c.A0;
            return;
        }

        var env = c.A0;
        short clipX = S16(m, env + 0x00), clipY = S16(m, env + 0x02);
        short clipW = S16(m, env + 0x04), clipH = S16(m, env + 0x06);
        short ofsX = S16(m, env + 0x08), ofsY = S16(m, env + 0x0A);
        short twX = S16(m, env + 0x0C), twY = S16(m, env + 0x0E);
        short twW = S16(m, env + 0x10), twH = S16(m, env + 0x12);
        var tpage = m.ReadU16(env + 0x14);
        var dtd = m.ReadU8(env + 0x16);
        var dfe = m.ReadU8(env + 0x17);
        var isbg = m.ReadU8(env + 0x18);
        byte r0 = m.ReadU8(env + 0x19), g0 = m.ReadU8(env + 0x1A), b0 = m.ReadU8(env + 0x1B);

        if (Log.SdkOn)
            Log.Sdk($"PutDrawEnv env=0x{env:X8} clip=({clipX},{clipY})-{clipW}x{clipH} " +
                    $"ofs=({ofsX},{ofsY}) tpage=0x{tpage:X4} isbg={isbg}");

        _curCs = GetCs(clipX, clipY);
        _curCe = GetCe((short)(clipX + clipW - 1), (short)(clipY + clipH - 1));
        _curOfs = GetOfs(ofsX, ofsY);
        gpu.WriteGp0(_curCs);
        gpu.WriteGp0(_curCe);
        gpu.WriteGp0(_curOfs);
        gpu.WriteGp0(GetMode(dfe, dtd, tpage));
        gpu.WriteGp0(GetTw(twX, twY, twW, twH));
        gpu.WriteGp0(0xE6000000u);

        if (isbg != 0)
        {
            // 0022-0024. The background clear is the only thing that paints the
            // widescreen margin every frame. GlCore writes back and re-syncs a
            // target's *middle* columns only, so the margin columns live nowhere
            // but in the render target and are otherwise touched only by geometry
            // that happens to spill past the game's own 320-wide clip -- which
            // means that without this widening they accumulate every primitive
            // ever drawn out there and never lose one. That reads as ghosting
            // that persists while standing still, gains new content as you move,
            // and keeps a damage flash's red for good. Upstream has no margin, so
            // the merge to 0409bc2 took its narrower clear; this is the port's.
            var margin = GpuHle.WideMargin(clipW);
            var w = Math.Clamp(clipW + margin * 2, 0, VramShadow.Width - 1);
            var h = Math.Clamp((int)clipH, 0, VramShadow.Height - 1);
            int x = clipX - margin - ofsX, y = clipY - ofsY;
            gpu.WriteGp0(0x60000000u | ((uint)b0 << 16) | ((uint)g0 << 8) | r0);
            gpu.WriteGp0(((uint)(ushort)y << 16) | (ushort)x);
            gpu.WriteGp0(((uint)(ushort)h << 16) | (ushort)w);
        }

        if (Event.HasAnyListeners<DrawEnvEvent>())
        {
            var e = _drawEnvEvent;
            e.Context = c;
            e.Memory = m;
            e.ClipX = clipX;
            e.ClipY = clipY;
            e.ClipW = clipW;
            e.ClipH = clipH;
            e.OfsX = ofsX;
            e.OfsY = ofsY;
            e.IsBackground = isbg != 0;
            Event.Dispatch(e);
        }

        c.V0 = c.A0;
    }
    
    private static int _videoMode = -1;
    
    internal static bool Pal
    {
        get
        {
            if (_videoMode < 0)
            {
                if (Runtime.Cd == null) return false;
                _videoMode = European() ? 1 : 0;
            }

            return _videoMode == 1;
        }
    }
    
    private static bool European()
    {
        var id = Assets.AssetApi.GameId;
        return id.StartsWith("SCES", StringComparison.Ordinal) 
               || id.StartsWith("SLES", StringComparison.Ordinal)
               || id.StartsWith("SCED", StringComparison.Ordinal) 
               || id.StartsWith("SLED", StringComparison.Ordinal);
    }
    
    public static void SetVideoMode(CpuContext c, IMemory m)
    {
        c.V0 = Pal ? 1u : 0u;
        _videoMode = c.A0 != 0 ? 1 : 0;
    }
    
    public static void GetVideoMode(CpuContext c, IMemory m)
    {
        c.V0 = Pal ? 1u : 0u;
    }
    
    public static void PutDispEnv(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = c.A0;
            return;
        }

        var env = c.A0;
        short dispX = S16(m, env + 0x00), dispY = S16(m, env + 0x02);
        short dispW = S16(m, env + 0x04), dispH = S16(m, env + 0x06);
        short scrX = S16(m, env + 0x08), scrY = S16(m, env + 0x0A);
        short scrW = S16(m, env + 0x0C), scrH = S16(m, env + 0x0E);
        var isinter = m.ReadU8(env + 0x10);
        var isrgb24 = m.ReadU8(env + 0x11);
        var pal = Pal;

        if (Log.SdkOn)
            Log.Sdk($"PutDispEnv env=0x{env:X8} disp=({dispX},{dispY})-{dispW}x{dispH} " +
                    $"screen=({scrX},{scrY})-{scrW}x{scrH} inter={isinter} rgb24={isrgb24}");

        gpu.WriteGp1(0x05000000u | (((uint)dispY & 0x3FF) << 10) | ((uint)dispX & 0x3FF));

        var hStart = scrX * 10 + 0x260;
        var vStart = scrY + (pal ? 0x13 : 0x10);
        var hEnd = hStart + (scrW != 0 ? scrW * 10 : 2560);
        var vEnd = vStart + (scrH != 0 ? scrH : 240);
        hStart = Math.Clamp(hStart, 500, 3290);
        hEnd = Math.Clamp(hEnd, hStart + 0x50, 3290);
        vStart = Math.Clamp(vStart, 0x10, pal ? 310 : 256);
        vEnd = Math.Clamp(vEnd, vStart + 2, pal ? 312 : 258);
        gpu.WriteGp1(0x06000000u | (((uint)hEnd & 0xFFF) << 12) | ((uint)hStart & 0xFFF));
        gpu.WriteGp1(0x07000000u | (((uint)vEnd & 0x3FF) << 10) | ((uint)vStart & 0x3FF));

        var mode = 0x08000000u;
        if (pal) mode |= 0x8;
        if (isrgb24 != 0) mode |= 0x10;
        if (isinter != 0) mode |= 0x20;
        if (dispW <= 280)
        {
        }
        else if (dispW <= 352)
        {
            mode |= 1;
        }
        else if (dispW <= 400)
        {
            mode |= 0x40;
        }
        else if (dispW <= 560)
        {
            mode |= 2;
        }
        else
        {
            mode |= 3;
        }

        if (dispH > (pal ? 288 : 256)) mode |= 0x24;
        gpu.WriteGp1(mode);

        GpuHle.NotifyDisplay(dispX, dispY, dispW, dispH);

        FlipFrame(dispX, dispY);

        if (Event.HasAnyListeners<DispEnvEvent>())
        {
            var e = _dispEnvEvent;
            e.Context = c;
            e.Memory = m;
            e.X = dispX;
            e.Y = dispY;
            e.W = dispW;
            e.H = dispH;
            Event.Dispatch(e);
        }

        c.V0 = c.A0;
    }

    private static short S16(IMemory m, uint addr)
    {
        return (short)m.ReadU16(addr);
    }

    private static uint GetCs(short x, short y)
    {
        x = short.Clamp(x, 0, VramShadow.Width - 1);
        y = short.Clamp(y, 0, VramShadow.Height - 1);
        return 0xE3000000u | (((uint)y & 0x3FF) << 10) | ((uint)x & 0x3FF);
    }

    private static uint GetCe(short x, short y)
    {
        x = short.Clamp(x, 0, VramShadow.Width - 1);
        y = short.Clamp(y, 0, VramShadow.Height - 1);
        return 0xE4000000u | (((uint)y & 0x3FF) << 10) | ((uint)x & 0x3FF);
    }

    private static int _flipX = -1, _flipY = -1;
    private static double _autoMark;

    //0042. Presents forced from a display flip; see LibEtc.VSyncCalls.
    public static long AutoPresents;

    //this is not the best method probably, but some games get stuck on this and i havent found a better way
    private const double FlipGrace = 100.0;

    private static void FlipFrame(int x, int y)
    {
        if (x == _flipX && y == _flipY) return;

        _flipX = x;
        _flipY = y;
        AutoPresent();
    }

    private static void AutoPresent()
    {
        var now = Interrupts.ClockMs;
        if (now - LibEtc.LastWaitMs < FlipGrace) return;
        if (now - _autoMark < Host.FrameClock.FrameMs * 0.5) return;

        _autoMark = now;
        AutoPresents++; //0042
        Runtime.PresentFrame();
    }

    private static uint _curCs = 0xE3000000u, _curCe = 0xE4000000u, _curOfs = 0xE5000000u;

    private static (short X, short Y, short W, short H) ReadRect(IMemory m, uint p)
    {
        return (S16(m, p), S16(m, p + 2), S16(m, p + 4), S16(m, p + 6));
    }


    private static short Clamp(short v, int max)
    {
        return (short)Math.Clamp((int)v, 0, max);
    }

    private const int VramW = 1024;
    private const int VramH = 512;

    private static uint Pack(short lo, short hi)
    {
        return ((uint)(ushort)hi << 16) | (ushort)lo;
    }

    public static void LoadImage(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        var r = ReadRect(m, c.A0);
        var src = c.A1;
        short w = Clamp(r.W, VramW), h = Clamp(r.H, VramH);
        var words = (w * h + 1) / 2;
        if (words <= 0)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        gpu.WriteGp0(0x01000000u);
        gpu.WriteGp0(0xA0000000u);
        gpu.WriteGp0(Pack(r.X, r.Y));
        gpu.WriteGp0(Pack(w, h));
        for (var i = 0; i < words; i++)
            gpu.WriteGp0(m.ReadU32(src + (uint)i * 4u));

        c.V0 = 0u;
    }

    public static void StoreImage(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        var r = ReadRect(m, c.A0);
        var dst = c.A1;
        short w = Clamp(r.W, VramW), h = Clamp(r.H, VramH);
        var words = (w * h + 1) / 2;
        if (words <= 0)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        gpu.WriteGp0(0x01000000u);
        gpu.WriteGp0(0xC0000000u);
        gpu.WriteGp0(Pack(r.X, r.Y));
        gpu.WriteGp0(Pack(w, h));
        for (var i = 0; i < words; i++)
            m.WriteU32(dst + (uint)i * 4u, gpu.ReadData());

        c.V0 = 0u;
    }

    public static void MoveImage(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        var r = ReadRect(m, c.A0);
        if (r.W == 0 || r.H == 0)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        gpu.WriteGp0(0x80000000u);
        gpu.WriteGp0(Pack(r.X, r.Y));
        gpu.WriteGp0(Pack((short)c.A1, (short)c.A2));
        gpu.WriteGp0(Pack(r.W, r.H));

        c.V0 = 0u;
    }

    public static void ClearImage(CpuContext c, IMemory m)
    {
        var gpu = Runtime.Gpu;
        if (gpu == null)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        var r = ReadRect(m, c.A0);
        short w = Clamp(r.W, VramW - 1), h = Clamp(r.H, VramH - 1);
        var color = ((c.A3 & 0xFFu) << 16) | ((c.A2 & 0xFFu) << 8) | (c.A1 & 0xFFu);

        if ((r.X & 0x3F) != 0 || (w & 0x3F) != 0)
        {
            gpu.WriteGp0(0xE3000000u);
            gpu.WriteGp0(0xE4FFFFFFu);
            gpu.WriteGp0(0xE5000000u);
            gpu.WriteGp0(0xE6000000u);
            gpu.WriteGp0(0x60000000u | color);
            gpu.WriteGp0(Pack(r.X, r.Y));
            gpu.WriteGp0(Pack(w, h));
            gpu.WriteGp0(_curCs);
            gpu.WriteGp0(_curCe);
            gpu.WriteGp0(_curOfs);
        }
        else
        {
            gpu.WriteGp0(0xE6000000u);
            gpu.WriteGp0(0x02000000u | color);
            gpu.WriteGp0(Pack(r.X, r.Y));
            gpu.WriteGp0(Pack(w, h));
        }

        c.V0 = 0u;
    }

    private static uint GetOfs(short x, short y)
    {
        return 0xE5000000u | (((uint)y & 0x7FF) << 11) | ((uint)x & 0x7FF);
    }

    private static uint GetMode(int dfe, int dtd, ushort tpage)
    {
        return (dtd != 0 ? 0xE1000200u : 0xE1000000u) | (dfe != 0 ? 0x400u : 0u) | ((uint)tpage & 0x9FF);
    }

    private static uint GetTw(short x, short y, short w, short h)
    {
        var c0 = ((uint)x & 0xFF) >> 3;
        var c1 = ((uint)y & 0xFF) >> 3;
        var c2 = ((uint)-w & 0xFF) >> 3;
        var c3 = ((uint)-h & 0xFF) >> 3;
        return 0xE2000000u | (c1 << 15) | (c0 << 10) | (c3 << 5) | c2;
    }
}