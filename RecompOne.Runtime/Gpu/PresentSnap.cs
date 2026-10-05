namespace RecompOne.Runtime;

/// <summary>
/// 0069. The presented picture read back once, on request: what the window shows,
/// after the occlusion, the reflections and any post shader, at the render scale.
/// VRAM cannot answer that -- the passes composite at present and write nothing back
/// -- so this is the only way to compare two pictures of a change that lives there.
///
/// <para>The request is taken by the next present that composites a picture, which
/// reads it with <c>glReadPixels</c> (a stall, once) and hands over RGBA8 rows,
/// top row first, on the thread that presents. GL core and 2.1 both; the software
/// rasterizer never presents through here.</para>
/// </summary>
public static class PresentSnap
{
    /// <summary>RGBA8, top row first; width; height; the VRAM origin of the display
    /// buffer it was presented from.</summary>
    public delegate void Taken(byte[] rgba, int w, int h, int dispX, int dispY);

    /// <summary>How many presents a request waits for its buffer before it takes
    /// whichever is presented.</summary>
    const int BufferPatience = 8;

    static Taken? _pending;
    static int _skip, _dispY = -1, _patience;

    /// <summary>Read the picture presented <paramref name="after"/> presents from
    /// now (0 is the next one), and from then on the first one whose display buffer
    /// starts at VRAM row <paramref name="dispY"/> (-1, any). Two display buffers
    /// can differ by a pixel's rounding, so a comparison names one. A second request
    /// replaces the first.</summary>
    public static void Request(Taken taken, int after = 0, int dispY = -1)
    {
        _skip = Math.Max(0, after);
        _dispY = dispY;
        _patience = BufferPatience;
        _pending = taken;
    }

    public static bool Pending => _pending != null;

    /// <summary>The presenting backend asks once per present; true when this one
    /// is to be read.</summary>
    internal static bool Due(int dispY)
    {
        if (_pending == null) return false;
        if (_skip > 0) { _skip--; return false; }
        if (_dispY >= 0 && dispY != _dispY && --_patience > 0) return false;
        return true;
    }

    internal static void Deliver(byte[] rgba, int w, int h, int dispX, int dispY)
    {
        var t = _pending;
        _pending = null;
        t?.Invoke(rgba, w, h, dispX, dispY);
    }
}
