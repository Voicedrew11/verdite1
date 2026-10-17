// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1.Mods.Debug;

/// <summary>
/// Noclip flight: fly through walls, with the body coming along.
///
/// The collision move func_800171FC is left running and its answer overwritten:
/// the mod keeps its own position, integrates it against wall-clock time, and
/// writes it at the end of stage A, before stage B copies the player into the
/// camera. Forward is the walk's own vector, (-sin yaw, +cos yaw) (measured: yaw
/// 0x400 with Up held decreases x), with the pitch folded in.
///
/// Each write also puts the floor at y + 1500, so the game sees the eye standing,
/// and the last cell equal to the cell, so flying over an exit cell (0x40) never
/// fires it. The death routine is refused while flying: the 0x52 hazard cell
/// calls it at full HP.
/// </summary>
internal static class Noclip
{
    internal static bool Enabled;

    // Units a second at full deflection, against wall-clock time, so the flight
    // is the same at any frame rate. A cell is 2000.
    internal static float Speed = 6000f;
    internal static float FastMultiplier = 4f;
    internal static bool Fast;

    const double MaxStep = 0.1;
    static long _lastTicks;

    internal static bool InvertStrafe;

    // ---- the cinematic camera ----
    static bool _cinematic;
    internal static bool Cinematic
    {
        get => _cinematic;
        set
        {
            if (value == _cinematic) return;
            _cinematic = value;
            // Instant mouse look is off while the cinematic camera is on.
            Kf1.ViewSmoothing.LeadSuppressed = value;
            _lookPrimed = false;
        }
    }

    internal static float MoveSmoothing = 0.35f;
    internal static float LookSmoothing = 0.25f;

    static double _vx, _vy, _vz;

    static bool _lookPrimed;
    static double _targetYaw, _targetPitch, _smoothYaw, _smoothPitch;
    static int _prevYaw, _prevPitch;

    // Negative y is up in this game's world (the eye stands at -11500 over a
    // floor at -10000). Which sign of pitch looks down is not measured, so the
    // dive follows the same toggle.
    internal static bool InvertVertical;
    internal static bool InvertPitch;

    static double _x, _y, _z;

    static bool _wasEnabled;
    static (int X, int Y, int Z) _entryPosition;
    internal static (int X, int Y, int Z) EntryPosition => _entryPosition;

    static bool _wantReturn;
    static bool _wantSnap;

    internal static long DistanceFromEntry(IMemory m)
    {
        var (x, y, z) = GameState.Position(m);
        double dx = x - _entryPosition.X, dy = y - _entryPosition.Y, dz = z - _entryPosition.Z;
        return (long)Math.Sqrt(dx * dx + dy * dy + dz * dz);
    }

    internal static bool ReturnToEntry()
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) return false;
        _wantReturn = true;
        return true;
    }

    static void Sync(IMemory m)
    {
        var (x, y, z) = GameState.Position(m);
        _x = x; _y = y; _z = z;
        _vx = _vy = _vz = 0;
        _lookPrimed = false;
    }

    static bool Flying(IMemory m) => Enabled && GameState.IsInGame(m);

    [PreHook("game", Address = 0x80015164)]
    static bool BeforeDeath(CpuContext c, IMemory m) => !Flying(m);

    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m)
    {
        if (_wantReturn || _wantSnap)
        {
            if (GameState.IsInGame(m))
            {
                if (_wantReturn) { _wantReturn = false; DoReturnToEntry(m); }
                if (_wantSnap)   { _wantSnap = false;   DoSnapToFloor(m); }
            }
            else _wantReturn = _wantSnap = false;
        }

        if (!Enabled)
        {
            if (_wasEnabled) Leave(m);
            return;
        }

        if (!Flying(m)) return;
        if (!_wasEnabled) Enter(m);
        Fly(m);
    }

    static void Enter(IMemory m)
    {
        _wasEnabled = true;
        _lastTicks = System.Diagnostics.Stopwatch.GetTimestamp();
        _entryPosition = GameState.Position(m);
        Sync(m);
        Console.WriteLine($"[kf1debug] noclip on at {Format(_entryPosition)}");
    }

    static void Leave(IMemory m)
    {
        _wasEnabled = false;
        if (!GameState.IsInGame(m)) return;
        // The game takes the floor from the cell again on its next move; until
        // then the eye would hang where it stopped, so it is put on the floor.
        var (x, y, z) = GameState.Position(m);
        Console.WriteLine($"[kf1debug] noclip off at {Format((x, y, z))}");
        Place(m, x, GameState.StandingY(m, x, z), z);
    }

    static void Fly(IMemory m)
    {
        double dt = Elapsed();

        if (_cinematic) SmoothLook(m, dt);

        int yaw   = GameState.ReadAngle12(m, GameState.Yaw);
        int pitch = GameState.ReadS16(m, GameState.Pitch);

        var (sx, sy) = Shape(Controller.LeftX, Controller.LeftY);
        var (dfwd, dstrafe) = Digital();

        float forward = Math.Clamp(-sy + dfwd, -1f, 1f);
        float strafe  = Math.Clamp(sx + dstrafe, -1f, 1f);
        if (InvertStrafe) strafe = -strafe;
        float vertical = Vertical();

        double tvx = 0, tvy = 0, tvz = 0;
        if (forward != 0f || strafe != 0f || vertical != 0f)
        {
            double rate = Speed * (Fast || Hotkeys.FlyFast() || Held(Controller.R3) ? FastMultiplier : 1f);

            // Strafe right is yaw - 0x400: yaw increases turning left.
            float fwdAngle    = GameState.AngleToRadians(yaw);
            float strafeAngle = GameState.AngleToRadians(yaw - 0x400);
            float pitchRad    = GameState.AngleToRadians(pitch) * (InvertPitch ? -1f : 1f);
            float level = MathF.Cos(pitchRad);
            float dive  = MathF.Sin(pitchRad);
            float up = InvertVertical ? 1f : -1f;

            tvx = (-MathF.Sin(fwdAngle) * forward * level + -MathF.Sin(strafeAngle) * strafe) * rate;
            tvz = ( MathF.Cos(fwdAngle) * forward * level +  MathF.Cos(strafeAngle) * strafe) * rate;
            tvy = (vertical * up - forward * dive * up) * rate;
        }

        double k = _cinematic ? Lag(MoveSmoothing, dt) : 1.0;
        _vx += (tvx - _vx) * k;
        _vy += (tvy - _vy) * k;
        _vz += (tvz - _vz) * k;

        _x += _vx * dt;
        _y += _vy * dt;
        _z += _vz * dt;

        // The cell bytes index a 100x100 grid; outside it there is nothing, so
        // the flight is held to the map.
        _x = Math.Clamp(_x, 0, GameState.Grid * GameState.CellSize - 1);
        _z = Math.Clamp(_z, 0, GameState.Grid * GameState.CellSize - 1);

        GameState.SetPosition(m, (int)_x, (int)_y, (int)_z);
        // The walk and strafe only; turning and looking keep their own easing.
        m.WriteU16(GameState.StrafeVel, 0);
        m.WriteU16(GameState.FwdVel, 0);
        m.WriteU16(GameState.WalkMag, 0);
    }

    static double Lag(double tau, double dt) =>
        tau <= 1e-4 ? 1.0 : 1.0 - Math.Exp(-dt / tau);

    /// <summary>
    /// The camera, trailing the input: the turn stage A just applied is recovered
    /// as a difference from the last write, accumulated into a target, and the
    /// smoothed value written back. Yaw is masked to 12 bits; pitch is a plain s16.
    /// </summary>
    static void SmoothLook(IMemory m, double dt)
    {
        int baseYaw   = GameState.ReadAngle12(m, GameState.Yaw);
        int basePitch = GameState.ReadS16(m, GameState.Pitch);

        if (!_lookPrimed)
        {
            _targetYaw = _smoothYaw = baseYaw;
            _targetPitch = _smoothPitch = basePitch;
            _prevYaw = baseYaw;
            _prevPitch = basePitch;
            _lookPrimed = true;
            return;
        }

        int dYaw = (baseYaw - _prevYaw) & GameState.AngleMask;
        if (dYaw > GameState.AngleFull / 2) dYaw -= GameState.AngleFull;
        _targetYaw += dYaw;
        _targetPitch = Math.Clamp(_targetPitch + (basePitch - _prevPitch),
                                  -GameState.PitchLimit, GameState.PitchLimit);

        double k = Lag(LookSmoothing, dt);
        _smoothYaw   += (_targetYaw - _smoothYaw) * k;
        _smoothPitch += (_targetPitch - _smoothPitch) * k;

        if (_targetYaw > GameState.AngleFull * 64 || _targetYaw < -GameState.AngleFull * 64)
        {
            double turns = Math.Truncate(_targetYaw / GameState.AngleFull) * GameState.AngleFull;
            _targetYaw -= turns;
            _smoothYaw -= turns;
        }

        int yaw   = ((int)Math.Round(_smoothYaw)) & GameState.AngleMask;
        int pitch = (int)Math.Round(_smoothPitch);

        GameState.WriteS16(m, GameState.Yaw, yaw);
        GameState.WriteS16(m, GameState.Pitch, pitch);

        _prevYaw = yaw > GameState.AngleFull / 2 ? yaw - GameState.AngleFull : yaw;
        _prevPitch = pitch;
    }

    /// <summary>
    /// Ask for the player to be put on the floor of the cell they are over. The
    /// way out of a flight that ended inside a wall.
    /// </summary>
    internal static bool SnapToFloor()
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) return false;
        _wantSnap = true;
        return true;
    }

    static void DoSnapToFloor(IMemory m)
    {
        var (x, _, z) = GameState.Position(m);
        int y = GameState.StandingY(m, x, z);
        Place(m, x, y, z);
        Console.WriteLine($"[kf1debug] snapped to floor, y {y}");
    }

    static void DoReturnToEntry(IMemory m)
    {
        Place(m, _entryPosition.X, _entryPosition.Y, _entryPosition.Z);
        Console.WriteLine($"[kf1debug] returned to {Format(_entryPosition)}");
    }

    static void Place(IMemory m, int x, int y, int z)
    {
        GameState.SetPosition(m, x, y, z);
        GameState.StopMotion(m);
        Sync(m);
    }

    /// <summary>
    /// Re-read the game's position. Anything that moves the player from outside
    /// this file calls it, or the next flight frame drags them back.
    /// </summary>
    internal static void Resync()
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem != null) Sync(mem);
    }

    static double Elapsed()
    {
        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        double dt = (now - _lastTicks) / (double)System.Diagnostics.Stopwatch.Frequency;
        _lastTicks = now;
        return dt <= 0 ? 0 : Math.Min(dt, MaxStep);
    }

    /// <summary>
    /// Forward and strafe off the runtime's pad word (active low), which carries
    /// the keyboard's bindings too: W/S on Up/Down and A/D on L1/R1 in the
    /// shipped layout.
    /// </summary>
    static (float Forward, float Strafe) Digital()
    {
        float f = 0f, s = 0f;
        if (Held(Controller.Up)) f += 1f;
        if (Held(Controller.Down)) f -= 1f;
        if (Held(Controller.R1)) s += 1f;
        if (Held(Controller.L1)) s -= 1f;
        return (f, s);
    }

    /// <summary>Page Up/Down, and the pad's R2/L2 (which also pitch, as in the game).</summary>
    static float Vertical()
    {
        float v = Hotkeys.FlyVertical();
        if (Held(Controller.R2)) v += 1f;
        if (Held(Controller.L2)) v -= 1f;
        return v;
    }

    static bool Held(ushort bit) => (Controller.State & bit) == 0;

    static (float X, float Y) Shape(byte bx, byte by)
    {
        const float deadzone = 0.15f;
        float x = (bx - 128) / 127f;
        float y = (by - 128) / 127f;
        float mag = MathF.Sqrt(x * x + y * y);
        if (mag <= deadzone || mag <= 0f) return (0f, 0f);
        float unit = Math.Clamp((mag - deadzone) / (1f - deadzone), 0f, 1f);
        float scaled = unit / mag;
        return (x * scaled, y * scaled);
    }

    internal static string Format((int X, int Y, int Z) p) => $"({p.X}, {p.Y}, {p.Z})";

    internal static void Reset()
    {
        Enabled = false;
        Fast = false;
        _wasEnabled = false;
        _wantReturn = _wantSnap = false;
        _vx = _vy = _vz = 0;
        _cinematic = false;
        Kf1.ViewSmoothing.LeadSuppressed = false;
        _lookPrimed = false;
    }
}
