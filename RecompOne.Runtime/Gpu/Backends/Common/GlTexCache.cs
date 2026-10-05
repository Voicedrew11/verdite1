using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

/// <summary>
/// 0060. Decoded textures with a mip chain each, for <see cref="GteDepth.Mipmaps"/>.
///
/// VRAM cannot be mipmapped: it is one sheet of pages, CLUTs and indices, and a lower
/// level would average a texture into its neighbours and an index into the next one.
/// So a texture a polygon is minified on is decoded through its CLUT into a block of
/// its own in an RGBA8 atlas, premultiplied by solidity (a transparent texel is
/// black), and its levels are built by a 2x2 box inside the block. A block is a
/// power of two on a boundary of its size, so every level of it is aligned too and
/// no level reads another block. Past the texture's own width the block repeats its
/// edge, so a bilinear tap at the edge reads the edge.
///
/// An entry is keyed on the page, the mode, the CLUT and the texel rectangle, and is
/// decoded again when <c>VramTracker</c> says the texels or the CLUT changed. Decodes
/// are queued as polygons ask and run at the start of the batch that draws them, so a
/// decode sees VRAM as those polygons did: anything that changes VRAM flushes first.
/// </summary>
sealed class GlTexCache
{
    public const int AtlasSize = 2048;
    const int MinLog = 3, MaxLog = 8, AtlasLog = 11;
    const int Levels = MaxLog + 1;

    [StructLayout(LayoutKind.Sequential)]
    struct QuadVert
    {
        public float X, Y;
        public int U0, V0, W, H;
        public int PageX, PageY, ClutX, ClutY;
        public int Mode, BlockX, BlockY, Pad;
    }

    sealed class Entry
    {
        public ulong Key;
        public int X, Y, Log;
        public int U0, V0, W, H, Mode, PageX, PageY, ClutX, ClutY;
        public int Gen;
        public int CheckedClock = -1;
        public long LastFrame;
        public bool Queued;
    }

    readonly GL _gl;
    uint _tex, _vao, _vbo, _progDecode, _progDown;
    readonly uint[] _fbo = new uint[Levels];
    public bool Ready { get; private set; }

    readonly Dictionary<ulong, Entry> _entries = [];
    readonly List<Entry> _queue = [];
    QuadVert[] _quads = new QuadVert[6 * 64];

    // Buddy allocator: free blocks per size, keyed by block index at that size.
    readonly HashSet<int>[] _free = new HashSet<int>[AtlasLog + 1];

    public GlTexCache(GL gl) => _gl = gl;

    public uint Texture => _tex;

    public unsafe void Init()
    {
        _progDecode = GlShaders.Build(_gl, GlShaders.MipVs, GlShaders.MipDecodeFs, "mipdecode",
            [(0, "aPos"), (1, "aRect"), (2, "aSrc"), (3, "aInfo")]);
        _progDown = GlShaders.Build(_gl, GlShaders.MipVs, GlShaders.MipDownFs, "mipdown",
            [(0, "aPos"), (1, "aRect"), (2, "aSrc"), (3, "aInfo")]);
        if (_progDecode == 0 || _progDown == 0) return;

        _gl.UseProgram(_progDecode);
        _gl.Uniform1(_gl.GetUniformLocation(_progDecode, "uVram"), 0);
        _gl.UseProgram(_progDown);
        _gl.Uniform1(_gl.GetUniformLocation(_progDown, "uAtlas"), 0);

        _gl.ActiveTexture(TextureUnit.Texture7);
        _tex = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, _tex);
        for (int l = 0; l < Levels; l++)
        {
            uint sz = (uint)(AtlasSize >> l);
            _gl.TexImage2D(TextureTarget.Texture2D, l, InternalFormat.Rgba8, sz, sz, 0,
                PixelFormat.Rgba, PixelType.UnsignedByte, null);
        }
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureBaseLevel, 0);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMaxLevel, MaxLog);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.LinearMipmapNearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Linear);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.ActiveTexture(TextureUnit.Texture0);

        for (int l = 0; l < Levels; l++)
        {
            _fbo[l] = _gl.GenFramebuffer();
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo[l]);
            _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
                TextureTarget.Texture2D, _tex, l);
        }
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);

        _vao = _gl.GenVertexArray();
        _vbo = _gl.GenBuffer();
        _gl.BindVertexArray(_vao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vbo);
        uint st = (uint)sizeof(QuadVert);
        _gl.EnableVertexAttribArray(0); _gl.VertexAttribPointer(0, 2, VertexAttribPointerType.Float, false, st, (void*)0);
        _gl.EnableVertexAttribArray(1); _gl.VertexAttribIPointer(1, 4, VertexAttribIType.Int, st, (void*)8);
        _gl.EnableVertexAttribArray(2); _gl.VertexAttribIPointer(2, 4, VertexAttribIType.Int, st, (void*)24);
        _gl.EnableVertexAttribArray(3); _gl.VertexAttribIPointer(3, 4, VertexAttribIType.Int, st, (void*)40);
        _gl.BindVertexArray(0);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, 0);

        Reset();
        Ready = true;
    }

    void Reset()
    {
        _entries.Clear();
        _queue.Clear();
        for (int i = 0; i <= AtlasLog; i++) _free[i] = [];
        _free[AtlasLog].Add(0);
        GteDepth.MipEntries = 0;
    }

    /// <summary>The atlas entry for a texture, as the flags the prim shader reads
    /// (block x/8, y/8, log2 size, bit 30), or 0 when there is none.
    /// <paramref name="tpage"/> and <paramref name="clut"/> are the primitive's.</summary>
    public uint Lookup(int tpage, int clut, uint rect, long frame)
    {
        int mode = (tpage >> 7) & 3;
        if (mode == 3) mode = 2;
        int u0 = (int)(rect & 0xFF), v0 = (int)((rect >> 8) & 0xFF);
        int u1 = (int)((rect >> 16) & 0xFF), v1 = (int)(rect >> 24);
        int w = u1 - u0 + 1, h = v1 - v0 + 1;
        if (w <= 0 || h <= 0) return 0;

        ulong key = rect | (ulong)(tpage & 0x1F) << 32 | (ulong)mode << 37
                  | (mode == 2 ? 0ul : (ulong)(clut & 0x7FFF) << 40);
        if (!_entries.TryGetValue(key, out var e))
        {
            int log = MinLog;
            while ((1 << log) < Math.Max(w, h)) log++;
            if (log > MaxLog) return 0;
            if (!Allocate(log, frame, out int bx, out int by)) { GteDepth.MipFull++; return 0; }
            e = new Entry
            {
                Key = key, X = bx, Y = by, Log = log, U0 = u0, V0 = v0, W = w, H = h, Mode = mode,
                PageX = (tpage & 0xF) * 64, PageY = ((tpage >> 4) & 1) * 256,
                ClutX = (clut & 0x3F) * 16, ClutY = (clut >> 6) & 0x1FF,
            };
            _entries[key] = e;
            GteDepth.MipEntries = _entries.Count;
        }

        e.LastFrame = frame;
        int clock = Assets.Textures.VramTracker.Clock;
        if (e.CheckedClock != clock)
        {
            e.CheckedClock = clock;
            int gen = VramGen(e);
            if (gen != e.Gen)
            {
                e.Gen = gen;
                if (!e.Queued) { e.Queued = true; _queue.Add(e); }
            }
        }
        return (uint)(e.X >> 3) | (uint)(e.Y >> 3) << 8 | (uint)e.Log << 16 | 0x40000000u;
    }

    static int VramGen(Entry e)
    {
        int div = e.Mode == 0 ? 4 : e.Mode == 1 ? 2 : 1;
        int x0 = e.PageX + e.U0 / div, x1 = e.PageX + (e.U0 + e.W - 1) / div;
        int g = Assets.Textures.VramTracker.Generation(x0, e.PageY + e.V0, x1 - x0 + 1, e.H);
        if (e.Mode != 2)
            g = g * 31 + Assets.Textures.VramTracker.Generation(e.ClutX, e.ClutY, e.Mode == 0 ? 16 : 256, 1);
        return g | 1;
    }

    public bool HasPending => _queue.Count > 0;

    /// <summary>Decode what the batch about to be drawn asked for and build its levels.
    /// Leaves the framebuffer, program, VAO, viewport and texture unit 0 changed;
    /// the caller binds its own after.</summary>
    public unsafe void Process(uint vramTex)
    {
        if (_queue.Count == 0) return;
        int n = _queue.Count;
        if (_quads.Length < n * 6) _quads = new QuadVert[n * 6 * 2];

        _gl.Disable(EnableCap.ScissorTest);
        _gl.Disable(EnableCap.DepthTest);
        _gl.Disable(EnableCap.Blend);
        _gl.DepthMask(false);
        _gl.BindVertexArray(_vao);
        _gl.BindBuffer(BufferTargetARB.ArrayBuffer, _vbo);

        // Level 0: decode.
        for (int i = 0; i < n; i++) Quad(i, _queue[i], 0);
        Upload(n);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo[0]);
        _gl.Viewport(0, 0, AtlasSize, AtlasSize);
        _gl.UseProgram(_progDecode);
        _gl.ActiveTexture(TextureUnit.Texture0);
        _gl.BindTexture(TextureTarget.Texture2D, vramTex);
        _gl.DrawArrays(PrimitiveType.Triangles, 0, (uint)(n * 6));

        // Levels 1..: each from the one above, sampling that level alone so the level
        // being written is not in the texture's sampled range.
        _gl.UseProgram(_progDown);
        _gl.BindTexture(TextureTarget.Texture2D, _tex);
        for (int l = 1; l <= MaxLog; l++)
        {
            int m = 0;
            for (int i = 0; i < n; i++)
                if (_queue[i].Log >= l) Quad(m++, _queue[i], l);
            if (m == 0) break;
            Upload(m);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureBaseLevel, l - 1);
            _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMaxLevel, l - 1);
            _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo[l]);
            _gl.Viewport(0, 0, (uint)(AtlasSize >> l), (uint)(AtlasSize >> l));
            _gl.DrawArrays(PrimitiveType.Triangles, 0, (uint)(m * 6));
        }
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureBaseLevel, 0);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMaxLevel, MaxLog);

        if (GteDepth.MipVerify > 0) Verify(_queue[n - 1]);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        _gl.BindVertexArray(0);
        foreach (var e in _queue) e.Queued = false;
        GteDepth.MipDecodes += n;
        _queue.Clear();
    }

    unsafe void Verify(Entry e)
    {
        GteDepth.MipVerify--;
        int size = 1 << e.Log;
        var px = new byte[size * size * 4];
        var top = new byte[4];
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _fbo[0]);
        fixed (byte* p = px) _gl.ReadPixels(e.X, e.Y, (uint)size, (uint)size, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _fbo[e.Log]);
        fixed (byte* p = top) _gl.ReadPixels(e.X >> e.Log, e.Y >> e.Log, 1, 1, PixelFormat.Rgba, PixelType.UnsignedByte, p);
        double r = 0, g = 0, b = 0, a = 0;
        int distinct = 0, last = -1;
        for (int i = 0; i < size * size; i++)
        {
            r += px[i * 4]; g += px[i * 4 + 1]; b += px[i * 4 + 2]; a += px[i * 4 + 3];
            int v = px[i * 4] | px[i * 4 + 1] << 8 | px[i * 4 + 2] << 16;
            if (v != last) { distinct++; last = v; }
        }
        int n = size * size;
        Console.WriteLine($"[KF2] mip verify: mode {e.Mode} page {e.PageX},{e.PageY} rect {e.U0},{e.V0} {e.W}x{e.H} " +
                          $"block {size}: level 0 mean {r / n:F1},{g / n:F1},{b / n:F1} solid {a / n / 255:P0}, " +
                          $"{distinct} colour runs; level {e.Log} {top[0]},{top[1]},{top[2]},{top[3]}");
    }

    unsafe void Upload(int quads)
    {
        fixed (QuadVert* p = _quads)
            _gl.BufferData(BufferTargetARB.ArrayBuffer, (nuint)(quads * 6 * sizeof(QuadVert)), p, BufferUsageARB.StreamDraw);
    }

    void Quad(int slot, Entry e, int level)
    {
        float size = AtlasSize >> level;
        float x0 = (e.X >> level) / size * 2f - 1f, y0 = (e.Y >> level) / size * 2f - 1f;
        float x1 = ((e.X >> level) + (1 << (e.Log - level))) / size * 2f - 1f;
        float y1 = ((e.Y >> level) + (1 << (e.Log - level))) / size * 2f - 1f;
        var q = new QuadVert
        {
            U0 = e.U0, V0 = e.V0, W = e.W, H = e.H,
            PageX = e.PageX, PageY = e.PageY, ClutX = e.ClutX, ClutY = e.ClutY,
            Mode = e.Mode, BlockX = e.X >> level, BlockY = e.Y >> level,
        };
        int b = slot * 6;
        _quads[b] = q; _quads[b].X = x0; _quads[b].Y = y0;
        _quads[b + 1] = q; _quads[b + 1].X = x1; _quads[b + 1].Y = y0;
        _quads[b + 2] = q; _quads[b + 2].X = x0; _quads[b + 2].Y = y1;
        _quads[b + 3] = q; _quads[b + 3].X = x1; _quads[b + 3].Y = y0;
        _quads[b + 4] = q; _quads[b + 4].X = x1; _quads[b + 4].Y = y1;
        _quads[b + 5] = q; _quads[b + 5].X = x0; _quads[b + 5].Y = y1;
    }

    // ---- The buddy allocator --------------------------------------------------

    bool Allocate(int log, long frame, out int x, out int y)
    {
        if (TryAllocate(log, out x, out y)) return true;
        // Evict what no frame since the last one has drawn, oldest first. The current
        // and previous frame's entries may still be in a batch not yet drawn.
        var old = new List<Entry>();
        foreach (var e in _entries.Values)
            if (e.LastFrame < frame - 1 && !e.Queued) old.Add(e);
        old.Sort((a, b) => a.LastFrame.CompareTo(b.LastFrame));
        foreach (var e in old)
        {
            _entries.Remove(e.Key);
            Release(e.X, e.Y, e.Log);
            GteDepth.MipEvictions++;
            if (TryAllocate(log, out x, out y)) { GteDepth.MipEntries = _entries.Count; return true; }
        }
        GteDepth.MipEntries = _entries.Count;
        return false;
    }

    bool TryAllocate(int log, out int x, out int y)
    {
        x = y = 0;
        if (log > AtlasLog) return false;
        int per = AtlasSize >> log;
        foreach (int i in _free[log])
        {
            _free[log].Remove(i);
            x = (i % per) << log; y = (i / per) << log;
            return true;
        }
        if (!TryAllocate(log + 1, out int px, out int py)) return false;
        int s = 1 << log;
        _free[log].Add(Index(px + s, py, log));
        _free[log].Add(Index(px, py + s, log));
        _free[log].Add(Index(px + s, py + s, log));
        x = px; y = py;
        return true;
    }

    void Release(int x, int y, int log)
    {
        while (log < AtlasLog)
        {
            int s = 1 << log;
            int px = x & ~(2 * s - 1), py = y & ~(2 * s - 1);
            int a = Index(px, py, log), b = Index(px + s, py, log), c = Index(px, py + s, log), d = Index(px + s, py + s, log);
            int self = Index(x, y, log);
            var f = _free[log];
            bool all = (a == self || f.Contains(a)) && (b == self || f.Contains(b))
                    && (c == self || f.Contains(c)) && (d == self || f.Contains(d));
            if (!all) { f.Add(self); return; }
            f.Remove(a); f.Remove(b); f.Remove(c); f.Remove(d);
            x = px; y = py; log++;
        }
        _free[AtlasLog].Add(0);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static int Index(int x, int y, int log) => (y >> log) * (AtlasSize >> log) + (x >> log);

    public void Dispose()
    {
        if (!Ready) return;
        _gl.DeleteTexture(_tex);
        foreach (var f in _fbo) _gl.DeleteFramebuffer(f);
        _gl.DeleteBuffer(_vbo);
        _gl.DeleteVertexArray(_vao);
        _gl.DeleteProgram(_progDecode);
        _gl.DeleteProgram(_progDown);
        Ready = false;
    }
}
