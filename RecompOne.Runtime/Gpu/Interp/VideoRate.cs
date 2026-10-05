namespace RecompOne.Runtime.Interp;

//vidrat
public static class VideoRate
{
    private const int History = 8;

    
    private static readonly int[] _factors = new int[History];
    private static int _cursor;
    
    public static int Rate { get; private set; }
    
    public static void Push(int factor)
    {
        if (factor <= 0) factor = 1;
        
        _factors[_cursor] = factor;
        _cursor = (_cursor + 1) % History;
        
        Rate = Settled();
    }
    
    public static void Reset()
    {
        Array.Clear(_factors);
        _cursor = 0;
        Rate = 0;
    }
    
    private const int Agreement = 6;
    
    private static int Settled()
    {
        var best = 0;
        var seen = 0;
        
        foreach (var candidate in _factors)
        {
            if (candidate <= 0) continue;
            
            var count = 0;
            foreach (var factor in _factors)
                if (factor == candidate) count++;
            
            if (count <= seen) continue;
            
            seen = count;
            best = candidate;
        }
        
        if (best <= 0 || seen < Agreement) return 0;
        
        return (Runtime.Gpu?.Pal == true ? 50 : 60) / best;
    }
}
