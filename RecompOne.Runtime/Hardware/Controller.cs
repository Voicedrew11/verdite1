namespace RecompOne.Runtime.Hardware;

public static class Controller
{
    public const ushort Select = 1 << 0;
    public const ushort L3 = 1 << 1;
    public const ushort R3 = 1 << 2;
    public const ushort Start = 1 << 3;
    public const ushort Up = 1 << 4;
    public const ushort Right = 1 << 5;
    public const ushort Down = 1 << 6;
    public const ushort Left = 1 << 7;
    public const ushort L2 = 1 << 8;
    public const ushort R2 = 1 << 9;
    public const ushort L1 = 1 << 10;
    public const ushort R1 = 1 << 11;
    public const ushort Triangle = 1 << 12;
    public const ushort Circle = 1 << 13;
    public const ushort Cross = 1 << 14;
    public const ushort Square = 1 << 15;

    public static bool Analog;
    public static bool Analog2;

    public static ushort State = 0xFFFF;
    public static byte RightX = 0x80;
    public static byte RightY = 0x80;
    public static byte LeftX = 0x80;
    public static byte LeftY = 0x80;

    /// <summary>Set by a port that reads the gyroscope; the host switches pad 1's
    /// on while this is true and off again when it is not (0102).</summary>
    public static bool WantGyro;

    /// <summary>Whether pad 1's gyroscope is on and reporting.</summary>
    public static bool Gyro;

    /// <summary>Pad 1's angular rate in radians a second, on SDL's axes: X
    /// across the pad (pitch), Y up out of it (yaw), Z toward the player (roll).
    /// Zero while <see cref="Gyro"/> is false.</summary>
    public static float GyroX, GyroY, GyroZ;

    /// <summary>One moment of rumble (0104): a low band and a high band, each a
    /// frequency in hertz and an amplitude 0..1. A two-motor pad takes the
    /// amplitudes alone, the low band on the large motor and the high on the small;
    /// a Switch pad sent HD rumble plays both bands at the frequencies asked.</summary>
    public sealed record RumbleWave(float LowHz, float Low, float HighHz, float High);

    /// <summary>What a port asks pad 1's motors for, or null for nothing. A new
    /// record is a new moment: the host sends it as soon as it sees it and resends
    /// it while it stands, so a port replaces the reference rather than editing one
    /// (0104).</summary>
    public static RumbleWave? Rumble;

    /// <summary>Set by a port that wants HD rumble: the host then writes a Switch
    /// pad's rumble reports itself, with both bands' frequencies (0104), or plays the
    /// wave into a DualSense's actuators over USB (0105), instead of going through
    /// SDL's two motors.</summary>
    public static bool WantHdRumble;

    /// <summary>Whether pad 1 is taking HD rumble: a Switch Pro Controller or a
    /// single Joy-Con opened for its reports, or a DualSense's sound card open for
    /// its actuators.</summary>
    public static bool HdRumble;

    public static ushort State2 = 0xFFFF;
    public static bool Connected2;
    public static byte RightX2 = 0x80;
    public static byte RightY2 = 0x80;
    public static byte LeftX2 = 0x80;
    public static byte LeftY2 = 0x80;
}