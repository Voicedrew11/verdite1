namespace RecompOne.Runtime.Interp;

internal sealed class InterpClock
{
    public const int MaxDisplayFrames = 8;
    
    private readonly float[] _weights = new float[MaxDisplayFrames];
    
    private long _logicalTicks;
    private long _displayTicks;
    private int _tickSource;
    private int _tickTarget;
    
    public ReadOnlySpan<float> Weights(int count)
    {
        return _weights.AsSpan(0, count);
    }
    
    public void Reset()
    {
        _logicalTicks = 0;
        _displayTicks = 0;
        _tickSource = 0;
        _tickTarget = 0;
    }
    
    public int Advance(int sourceRate, int targetRate)
    {
        if (sourceRate <= 0 || targetRate <= 0)
        {
            Reset();
            _weights[0] = 1f;
            return 1;
        }
        
        if (_tickSource != sourceRate || _tickTarget != targetRate)
        {
            _logicalTicks = 0;
            _displayTicks = 0;
            _tickSource = sourceRate;
            _tickTarget = targetRate;
        }
        
        _logicalTicks += targetRate;
        
        var frames = (int)((_logicalTicks - _displayTicks) / sourceRate);
        frames = Math.Min(frames, MaxDisplayFrames);
        
        for (var i = 0; i < frames; i++)
        {
            _displayTicks += sourceRate;
            _weights[i] = Math.Clamp((targetRate + _displayTicks - _logicalTicks) / (float)targetRate, 0f, 1f);
        }
        
        return frames;
    }
}
