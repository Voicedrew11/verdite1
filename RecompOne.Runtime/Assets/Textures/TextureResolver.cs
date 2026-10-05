namespace RecompOne.Runtime.Assets.Textures;

public struct ResolvedTexture
{
    public ReplacementTexture? Texture;
    public ReplacementClut? Clut;
    public TileRect Rect;
    public bool Hit;

    /// <summary>0073. A scrolling texture's replacement: <see cref="Rect"/> is the whole
    /// dest rectangle, and a row <c>d</c> of it shows the source's row
    /// <c>(d - Scroll) mod H</c>, <see cref="Scroll"/> in texels and fractional.</summary>
    public bool Scrolls;
    public float Scroll;
}

public static class TextureResolver
{
    private sealed class Entry
    {
        public int Generation = -1;
        public ulong IndexHash;
        public ulong ClutHash;
        public TextureAsset? Texture;
        public ClutAsset? Clut;
        public TextureAsset? PageTexture;
        public TileRect Rect;
        public TileRect PageRect;
        public bool Valid;
    }

    private sealed class PageEntry
    {
        public int Generation = -1;
        public TextureAsset? Texture;
        public TileRect Rect;
    }

    private static readonly Dictionary<long, PageEntry> _pages = [];

    private static void CheckAspect(TextureAsset asset, ReplacementTexture tex, in TileRect rect, string kind)
    {
        if (asset.AspectChecked || rect.W <= 0 || rect.H <= 0 || tex.Height <= 0) return;
        asset.AspectChecked = true;

        tex.ScaleX = tex.Width / (float)rect.W;
        tex.ScaleY = tex.Height / (float)rect.H;

        var original = rect.W / (double)rect.H;
        var replacement = tex.Width / (double)tex.Height;
        var distorted = Math.Abs(replacement - original) / original > 0.02;

        Console.WriteLine($"[assets] {kind} {asset.IndexHash:x16}: {rect.W}x{rect.H} -> {tex.Width}x{tex.Height} " +
                          $"({tex.ScaleX:0.##}x, {tex.ScaleY:0.##}x)" +
                          (distorted
                              ? "  WARNING: aspect differs from the original, the image will be stretched"
                              : ""));
    }

    private static TextureAsset? ResolvePage(ushort[] vram, int tpage, int clut, int bpp, bool dirty,
        out TileRect pageRect)
    {
        var widthTexels = bpp == 4 ? 256 : 128;
        pageRect = TextureTile.Describe(tpage, clut, 0, 0, widthTexels, 256);

        var key = ((long)(tpage & 0x1FF) << 32) ^ (clut & 0x7FFF);
        var generation = VramTracker.Generation(pageRect.VramX, pageRect.VramY, pageRect.VramW, pageRect.H)
                         ^ VramTracker.Generation(pageRect.ClutX, pageRect.ClutY, pageRect.ClutCount, 1);

        PageEntry page;
        lock (_pages)
        {
            if (!_pages.TryGetValue(key, out page!))
            {
                page = new PageEntry();
                _pages[key] = page;
            }
        }

        if (VramTracker.IsGpuDirty(pageRect.VramX, pageRect.VramY, pageRect.VramW, pageRect.H))
        {
            page.Generation = -1;
            page.Texture = null;
            return null;
        }

        if (page.Generation != generation)
        {
            page.Generation = generation;
            page.Rect = pageRect;
            page.Texture = null;

            if (TextureTile.Hash(vram, pageRect, out var pageIndex, out var pageClut))
            {
                if (!dirty) page.Texture = AssetReplacerManager.Instance.ResolveTexture(pageIndex, pageClut);
                TextureRegistry.Note(vram, pageRect, pageIndex, pageClut, tpage, clut,
                    page.Texture != null, true, dirty);
            }
        }

        pageRect = page.Rect;
        return page.Texture;
    }

    private static readonly Dictionary<long, Entry> _memo = [];
    private static int _version;

    private static int _statCalls, _statNoTexture, _statRejectSize, _statRejectDirty, _statHashed, _statMemo;

    public static bool Enabled { get; set; } = true;

    /// <summary>0073. The port's texture-key census: every lookup, with the key it resolved
    /// to. Null unless something listens; while set, a lookup runs with no pack loaded.</summary>
    public delegate void LookupObserver(int tpage, int clut, in TileRect rect, ulong indexHash, ulong clutHash,
        bool valid, bool dirty, bool hit);

    public static LookupObserver? Observer;

    /// <summary>0073. A face on a texture the game scrolls by rewriting its VRAM every
    /// tick, so no hash of the VRAM holds: the port answers with the key of the source
    /// image, the dest rectangle and the phase to draw at. Null, or false, is the
    /// ordinary lookup.</summary>
    public delegate bool ScrollLookup(int tpage, int clut, int uMin, int vMin, int uMax, int vMax,
        out ulong indexHash, out ulong clutHash, out TileRect dest, out float phase);

    public static ScrollLookup? Scroll;

    private static readonly Dictionary<(ulong, ulong), (TextureAsset? Tex, bool Seen)> _scrollAssets = [];
    public static long ScrollHits, ScrollMisses;

    /// <summary>0073. Look a triangle up by its face's texture rectangle when the
    /// assembler recorded one; false keys each triangle on its own UVs, as upstream does.</summary>
    public static bool KeyOnFaceRect = true;

    /// <summary>0073. Key a rectangle on the image the game uploaded it as, when it lies
    /// inside one to within <see cref="UploadSlop"/> texels: this game's faces read
    /// a texel past their texture's edge, so one 64x64 tile had three keys.</summary>
    public static bool KeyOnUpload = true;

    public const int UploadSlop = 2;

    /// <summary>The rectangle (<paramref name="u0"/>, <paramref name="v0"/>, <paramref name="w"/>,
    /// <paramref name="h"/>, in the page's texels) widened to the upload under its centre,
    /// when it lies inside it to within <see cref="UploadSlop"/>; false leaves it alone.
    /// The port keys its texture materials on the same rectangle.</summary>
    public static bool ToUpload(int tpage, ref int u0, ref int v0, ref int w, ref int h)
    {
        var depth = (tpage >> 7) & 3;
        var per = depth switch { 0 => 4, 1 => 2, _ => 1 };
        int pageX = (tpage & 0xF) * 64, pageY = ((tpage >> 4) & 1) * 256;
        if (!VramTracker.UploadAt(pageX + (u0 + w / 2) / per, pageY + v0 + h / 2,
                out var ux, out var uy, out var uw, out var uh))
            return false;
        int nu0 = Math.Max(0, (ux - pageX) * per), nv0 = Math.Max(0, uy - pageY);
        int nu1 = Math.Min(256, (ux + uw - pageX) * per), nv1 = Math.Min(256, uy + uh - pageY);
        if (nu1 <= nu0 || nv1 <= nv0) return false;
        if (u0 < nu0 - UploadSlop || v0 < nv0 - UploadSlop
            || u0 + w > nu1 + UploadSlop || v0 + h > nv1 + UploadSlop) return false;
        u0 = nu0; v0 = nv0; w = nu1 - nu0; h = nv1 - nv0;
        return true;
    }

    public static void ResetStats()
    {
        Volatile.Write(ref _statCalls, 0);
        Volatile.Write(ref _statNoTexture, 0);
        Volatile.Write(ref _statRejectSize, 0);
        Volatile.Write(ref _statRejectDirty, 0);
        Volatile.Write(ref _statHashed, 0);
        Volatile.Write(ref _statMemo, 0);
    }

    public static string StatsLine()
    {
        return $"calls={Volatile.Read(ref _statCalls)} untextured={Volatile.Read(ref _statNoTexture)} " +
               $"rejected-size={Volatile.Read(ref _statRejectSize)} rejected-gpudirty={Volatile.Read(ref _statRejectDirty)} " +
               $"hashed={Volatile.Read(ref _statHashed)} memo-hits={Volatile.Read(ref _statMemo)} tiles={CachedTiles}";
    }

    public static void Invalidate()
    {
        lock (_memo)
        {
            _memo.Clear();
            _version++;
        }

        lock (_pages)
        {
            _pages.Clear();
        }

        lock (_scrollAssets)
        {
            _scrollAssets.Clear();
        }
    }

    public static int CachedTiles
    {
        get
        {
            lock (_memo)
            {
                return _memo.Count;
            }
        }
    }

    private static bool ResolveScroll(AssetReplacerManager mgr, int tpage, int clut, ulong index, ulong clutHash,
        in TileRect dest, float phase, LookupObserver? observer, ref ResolvedTexture result)
    {
        (TextureAsset? Tex, bool Seen) e;
        lock (_scrollAssets)
        {
            if (!_scrollAssets.TryGetValue((index, clutHash), out e))
            {
                e = (mgr.ResolveTexture(index, clutHash), true);
                _scrollAssets[(index, clutHash)] = e;
            }
        }

        var tex = e.Tex != null ? mgr.LoadTexture(e.Tex) : null;
        observer?.Invoke(tpage, clut, dest, index, clutHash, true, false, tex != null);
        if (tex == null)
        {
            ScrollMisses++;
            return false;
        }

        CheckAspect(e.Tex!, tex, dest, "scrolling");
        ScrollHits++;
        result.Rect = dest;
        result.Texture = tex;
        result.Clut = null;
        result.Scrolls = true;
        result.Scroll = phase;
        result.Hit = true;
        return true;
    }

    public static bool Resolve(int tpage, int clut, int uMin, int vMin, int uMax, int vMax,
        int twAndX, int twAndY, int twOrX, int twOrY, out ResolvedTexture result)
    {
        result = default;
        if (!Enabled) return false;

        var mgr = AssetReplacerManager.Instance;
        var dumping = TextureDumper.Enabled;
        var observer = Observer;
        var observing = dumping || TextureRegistry.Enabled || observer != null;
        if (!observing && !mgr.HasTextures) return false;

        var gpu = Runtime.Gpu;
        if (gpu == null) return false;

        Interlocked.Increment(ref _statCalls);

        int u0, v0, w, h;
        if (twAndX != 0xFF || twOrX != 0)
        {
            u0 = twOrX;
            w = (~twAndX & 0xFF) + 1;
        }
        else
        {
            u0 = uMin;
            w = uMax - uMin + 1;
        }

        if (twAndY != 0xFF || twOrY != 0)
        {
            v0 = twOrY;
            h = (~twAndY & 0xFF) + 1;
        }
        else
        {
            v0 = vMin;
            h = vMax - vMin + 1;
        }

        bool windowed = twAndX != 0xFF || twOrX != 0 || twAndY != 0xFF || twOrY != 0;
        if (!windowed && Scroll is { } scroll
            && scroll(tpage, clut, uMin, vMin, uMax, vMax, out var sIndex, out var sClut, out var dest, out var phase))
            return ResolveScroll(mgr, tpage, clut, sIndex, sClut, dest, phase, observer, ref result);

        if (KeyOnUpload && !windowed)
            ToUpload(tpage, ref u0, ref v0, ref w, ref h);

        if (w <= 0 || h <= 0 || w > 256 || h > 256)
        {
            Interlocked.Increment(ref _statRejectSize);
            return false;
        }

        var rect = TextureTile.Describe(tpage, clut, u0, v0, w, h);

        if (mgr.HasRules && mgr.MatchRule(tpage, rect.Bpp, w, h) is { } ruled)
        {
            var ruledTex = mgr.LoadTexture(ruled);
            if (ruledTex != null)
            {
                result.Rect = rect;
                result.Texture = ruledTex;
                result.Clut = null;
                result.Hit = true;
                return true;
            }
        }

        var dirty = VramTracker.IsGpuDirty(rect.VramX, rect.VramY, rect.VramW, rect.H);
        if (dirty)
        {
            Interlocked.Increment(ref _statRejectDirty);
            if (!observing) return false;
        }

        var key = (long)(tpage & 0x1FF)
                  | ((long)(clut & 0x7FFF) << 9)
                  | ((long)(u0 & 0xFF) << 24)
                  | ((long)(v0 & 0xFF) << 32)
                  | ((long)(w & 0x1FF) << 40)
                  | ((long)(h & 0x1FF) << 49);

        var generation = VramTracker.Generation(rect.VramX, rect.VramY, rect.VramW, rect.H)
                         ^ VramTracker.Generation(rect.ClutX, rect.ClutY, rect.ClutCount, 1);

        Entry entry;
        lock (_memo)
        {
            if (!_memo.TryGetValue(key, out entry!))
            {
                entry = new Entry();
                _memo[key] = entry;
            }
        }

        if (entry.Generation == generation)
        {
            Interlocked.Increment(ref _statMemo);
        }
        else
        {
            Interlocked.Increment(ref _statHashed);
            entry.Generation = generation;
            entry.Rect = rect;
            entry.Valid = TextureTile.Hash(gpu.Vram, rect, out entry.IndexHash, out entry.ClutHash);

            if (entry.Valid)
            {
                if (dumping) TextureDumper.Offer(gpu.Vram, rect, entry.IndexHash, entry.ClutHash, tpage, clut);

                entry.Texture = null;
                entry.Clut = null;
                entry.PageTexture = null;

                if (!dirty)
                {
                    entry.Texture = mgr.ResolveTexture(entry.IndexHash, entry.ClutHash);
                    entry.Clut = mgr.ResolveClut(entry.ClutHash);
                }

                if (entry.Texture == null && rect.Bpp != 16)
                    entry.PageTexture = ResolvePage(gpu.Vram, tpage, clut, rect.Bpp, dirty, out entry.PageRect);

                if (entry.Texture != null || entry.Clut != null || entry.PageTexture != null) mgr.Stats.TextureHits++;
                else mgr.Stats.TextureMisses++;

                TextureRegistry.Note(gpu.Vram, rect, entry.IndexHash, entry.ClutHash, tpage, clut,
                    entry.Texture != null || entry.Clut != null || entry.PageTexture != null, false, dirty);
            }
        }

        observer?.Invoke(tpage, clut, rect, entry.IndexHash, entry.ClutHash, entry.Valid, dirty,
            entry.Valid && (entry.Texture != null || entry.Clut != null || entry.PageTexture != null));

        if (!entry.Valid || (entry.Texture == null && entry.Clut == null && entry.PageTexture == null)) return false;

        if (entry.Texture == null && entry.PageTexture != null)
        {
            result.Rect = entry.PageRect;
            result.Texture = mgr.LoadTexture(entry.PageTexture);
            result.Clut = null;
            result.Hit = result.Texture != null;
            if (result.Hit)
            {
                CheckAspect(entry.PageTexture, result.Texture!, entry.PageRect, "page");
                return true;
            }
        }

        result.Rect = entry.Rect;
        result.Texture = entry.Texture != null ? mgr.LoadTexture(entry.Texture) : null;
        if (result.Texture != null) CheckAspect(entry.Texture!, result.Texture, entry.Rect, "tile");
        result.Clut = entry.Clut != null ? mgr.LoadClut(entry.Clut) : null;
        result.Hit = result.Texture != null || result.Clut != null;
        return result.Hit;
    }
}