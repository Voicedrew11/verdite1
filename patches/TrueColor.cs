using RecompOne.Runtime;

namespace Kf1;

/// <summary>
/// 24-bit output for the GL backend: the display target becomes RGBA8 and the
/// fragment shader's <c>quant5</c> keeps eight bits a channel (the fork's `0021`).
/// Only the switch lives here. Off until judged; Testing ▸ Picture ▸ Shading.
///
///     KF1_TRUECOLOR=1     24-bit; 0 or unset is the console's 15-bit
///
/// See "Unit 1" in docs/PICTURE.md.
/// </summary>
public static class TrueColor
{
    public static bool Enabled
    {
        get => GteDepth.TrueColor;
        set => GteDepth.TrueColor = value;
    }

    public static void Configure(string? on)
    {
        if (!string.IsNullOrWhiteSpace(on)) Enabled = on.Trim() is not ("0" or "off");
    }
}
