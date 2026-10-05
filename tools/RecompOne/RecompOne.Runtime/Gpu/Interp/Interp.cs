using RecompOne.Runtime.Config;

namespace RecompOne.Runtime.Interp;

public static class Interp
{
    public const string KeyFps = "display.fps";
    
    public static bool Shown = true;
    
    public const int Native = 0;
    
    public static readonly int[] Steps =
    [
        Native, 5, 10, 15, 20, 24, 25, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240 //roughly based on https://en.wikipedia.org/wiki/Refresh_rate with some extra ones
    ];
    
    private static bool _loaded;
    
    internal static InterpBackend? Backend { get; set; }
    
    public static int TargetFps { get; private set; }
    
    public static bool Requested => TargetFps != Native;
    
    public static bool Available => Pgxp.Pgxp.Enabled && Pgxp.Pgxp.MemoryTracking;
    
    public static bool Enabled => Requested && Available;
    
    public static int RefreshRate { get; set; }
    
    public static bool VSync { get; set; }
    
    public static int Resolve(int target, int refreshRate, bool vsync)
    {
        if (target <= 0) return 0;
        
        return vsync && refreshRate > 0 ? Math.Min(target, refreshRate) : target;
    }
    
    public static int EffectiveTarget => Available ? Resolve(TargetFps, RefreshRate, VSync) : Native;
    
    public static void Load()
    {
        var fps = ConfigManager.View.GetInt(KeyFps, Native);
        TargetFps = Array.IndexOf(Steps, fps) >= 0 ? fps : Native;
        
        _loaded = true;
    }
    
    public static void EnsureLoaded()
    {
        if (!_loaded) Load();
    }
}
