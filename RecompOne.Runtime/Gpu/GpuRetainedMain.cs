using RecompOne.Runtime.Hle;

namespace RecompOne.Runtime;

public sealed partial class Gpu
{
    /// <summary>0085. The retained map's opaque range into the frame, with the draw
    /// area and offset the GPU holds now; the table walk calls it once a frame, past
    /// slot 0 (the sky). False when nothing was drawn.</summary>
    public bool DrawRetainedMain()
    {
        if (!HleOn || RetainedScene.MainDrawer is not { } draw) return false;
        GpuHle.Backend!.SetDrawEnv(CurEnv());
        return draw(_drawOffsetX, _drawOffsetY);
    }

    /// <summary>0085. The first-person arm, at its slot of the table's walk.</summary>
    public bool DrawRetainedArm()
    {
        if (!HleOn || RetainedScene.ArmDrawer is not { } draw) return false;
        GpuHle.Backend!.SetDrawEnv(CurEnv());
        return draw(_drawOffsetX, _drawOffsetY);
    }

    /// <summary>0085. The map's water the table walk passed at view depth
    /// <paramref name="cut"/>, drawn if it meets the box the walk draws next (all of it
    /// by default); false once no water is left.</summary>
    public bool DrawRetainedWater(float cut, float x0 = float.MinValue, float y0 = float.MinValue,
                                  float x1 = float.MaxValue, float y1 = float.MaxValue)
    {
        if (!HleOn || RetainedScene.WaterDrawer is not { } draw) return false;
        GpuHle.Backend!.SetDrawEnv(CurEnv());
        return draw(cut, x0, y0, x1, y1);
    }
}
