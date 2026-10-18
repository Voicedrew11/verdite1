using RecompOne.Runtime;

namespace Kf1;

/// <summary>
/// 24-bit output for the GL backend: the display target becomes RGBA8 and the
/// fragment shader's <c>quant5</c> keeps eight bits a channel (the fork's `0021`).
/// Only the switch lives here. On by default; Testing ▸ Picture ▸ Shading.
///
///     KF1_TRUECOLOR=0     the console's 15-bit; 1 or unset is 24-bit
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
        Enabled = string.IsNullOrWhiteSpace(on) || on.Trim() is not ("0" or "off");
    }
}
