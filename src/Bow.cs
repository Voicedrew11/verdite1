using RecompOne.Runtime.Memory;

namespace Verdite.Core;

/// <summary>
/// The bow as a port reads it once a tick: what <see cref="Rumble"/> and
/// <see cref="GyroAim"/> both gate on. Each port hands one in, as its
/// <c>Bow.Reads</c>: the guest memory and the time of the routine that runs once a
/// tick (KF2's func_800271D0, KF3's look routine), the equipped-weapon byte, the
/// swing clock and the clip byte, and the two weapon ids that are bows.
///
/// **Drawn** is a bow in hand with the clip byte at 0 and the swing clock running:
/// from the press that nocks the arrow, through the draw, to the release, which sets
/// the clip to 1 and the clock back at 0. The tension is the clock over
/// <see cref="FullDraw"/>, read off the game rather than timed here.
/// </summary>
public sealed class BowReads
{
    public const int FullDraw = 0x0FFF;

    /// <summary>The guest memory, from the last tick; null before the first.</summary>
    public required Func<IMemory?> Memory { get; init; }

    /// <summary>Environment.TickCount64 when the port's tick routine last ran.</summary>
    public required Func<long> TickMs { get; init; }

    /// <summary>u8, the equipped weapon id, 0xFF none.</summary>
    public required uint WeaponSlot { get; init; }

    /// <summary>s16, -1 idle; the draw, then the loose.</summary>
    public required uint SwingClock { get; init; }

    /// <summary>u8, for a bow: 0 drawn, 1 loosed.</summary>
    public required uint ClipByte { get; init; }

    /// <summary>The weapon ids that are bows (KF2 16 and 17, KF3 27 and 28).</summary>
    public required (int First, int Second) Weapons { get; init; }

    /// <summary>Whether attack is held this tick; the gyro probe's only read.</summary>
    public required Func<IMemory, bool> AttackHeld { get; init; }

    /// <summary>Another reader holds the mouse, so the gyro stands down (KF3's item turn).</summary>
    public Func<bool>? Busy { get; init; }

    /// <summary>Another reader wants the pad's gyroscope streamed too (KF3's item turn).</summary>
    public Func<bool>? GyroWanted { get; init; }

    /// <summary>The port's hook on its tick routine, run by each reader's Install.</summary>
    public Action? Attach { get; init; }

    /// <summary>A bow in hand.</summary>
    public bool InHand(IMemory m)
    {
        int weapon = m.ReadU8(WeaponSlot);
        return weapon == Weapons.First || weapon == Weapons.Second;
    }

    /// <summary>A bow in hand and being drawn: from the nock, through the draw, to the release.</summary>
    public bool Drawn(IMemory m, out int clock)
    {
        clock = (short)m.ReadU16(SwingClock);
        return InHand(m) && m.ReadU8(ClipByte) == 0 && clock >= 0;
    }
}
