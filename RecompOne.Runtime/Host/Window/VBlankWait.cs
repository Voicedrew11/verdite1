using System.Runtime.InteropServices;

namespace RecompOne.Runtime.Host.Window;

/// <summary>
/// The display's own vertical blank on Windows, from the kernel's graphics
/// interface: a wait that returns when the monitor the window is on starts its
/// blank, whatever the GL driver's swap interval does.
/// </summary>
internal static class VBlankWait
{
    [StructLayout(LayoutKind.Sequential)]
    private struct OpenAdapterFromHdc
    {
        public nint Hdc;
        public uint Adapter;
        public uint LuidLow;
        public int LuidHigh;
        public uint VidPnSourceId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WaitForVerticalBlankEvent
    {
        public uint Adapter;
        public uint Device;
        public uint VidPnSourceId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CloseAdapter
    {
        public uint Adapter;
    }

    [DllImport("gdi32.dll")]
    private static extern int D3DKMTOpenAdapterFromHdc(ref OpenAdapterFromHdc args);

    [DllImport("gdi32.dll")]
    private static extern int D3DKMTWaitForVerticalBlankEvent(ref WaitForVerticalBlankEvent args);

    [DllImport("gdi32.dll")]
    private static extern int D3DKMTCloseAdapter(ref CloseAdapter args);

    private static uint _adapter;
    private static uint _source;
    private static bool _open;

    /// <summary>Open the adapter behind <paramref name="hdc"/>. False off Windows
    /// or when the kernel refuses, and the caller keeps the driver's vsync.</summary>
    public static bool Open(nint hdc)
    {
        Close();
        if (!OperatingSystem.IsWindows() || hdc == 0) return false;
        try
        {
            var args = new OpenAdapterFromHdc { Hdc = hdc };
            if (D3DKMTOpenAdapterFromHdc(ref args) != 0) return false;
            _adapter = args.Adapter;
            _source = args.VidPnSourceId;
            _open = true;
            return true;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] vblank wait unavailable: {e.Message}");
            return false;
        }
    }

    public static bool IsOpen => _open;

    /// <summary>Block until the next vertical blank. False if the wait failed, in
    /// which case nothing was waited for.</summary>
    public static bool Wait()
    {
        if (!_open) return false;
        var args = new WaitForVerticalBlankEvent { Adapter = _adapter, VidPnSourceId = _source };
        return D3DKMTWaitForVerticalBlankEvent(ref args) == 0;
    }

    public static void Close()
    {
        if (!_open) return;
        _open = false;
        var args = new CloseAdapter { Adapter = _adapter };
        try { D3DKMTCloseAdapter(ref args); } catch { }
    }
}
