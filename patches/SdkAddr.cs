namespace Kf2;

/// <summary>
/// The PSY-Q entry points the patches hook, per overlay, for King's Field (JP,
/// SLPS-00017). One table, so a patch never carries its own copy of an address
/// that belongs to the game rather than to the patch -- which is how the King's
/// Field II port ended up with fourteen copies of its DrawOTag. The same
/// addresses are bound to the runtime's HLE in config/kf1.json; see "The SDK
/// entry points" in docs/KF1.md for how each was found.
/// </summary>
public static class SdkAddr
{
    public static readonly (string Overlay, uint Addr)[] DrawOTag =
        [("open", 0x800309D4), ("game", 0x80050CF0)];

    public static readonly (string Overlay, uint Addr)[] DrawSync =
        [("open", 0x80030578), ("game", 0x80050894)];

    public static readonly (string Overlay, uint Addr)[] PutDrawEnv =
        [("open", 0x80030A2C), ("game", 0x80050D48)];

    public static readonly (string Overlay, uint Addr)[] PutDispEnv =
        [("open", 0x80030B4C), ("game", 0x80050E68)];

    public static readonly (string Overlay, uint Addr)[] VSync =
        [("open", 0x800352C4), ("game", 0x800555E0)];
}
