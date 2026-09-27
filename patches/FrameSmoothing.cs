using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Carries the view between logic ticks, so that a picture drawn more often than
/// the world advances actually looks like it.
///
///     KF2_SMOOTH=0        off; the camera then moves at the tick rate only
///     KF2_SMOOTH_POS=0    leave the position at the tick, carry the angles
///     KF2_SMOOTH_PROBE=1  what is being carried, per second
///
/// Both are settings, under Video beside the frame rate. **Both default to on**:
/// the shipped picture is 60 fps against a 20 Hz world, and without this that is
/// a faster picture of a camera not moving.
///
/// This is the other half of <see cref="FramePacing"/> rather than an option
/// beside it. Above the tick rate the port ticks the game's own stages on their
/// own clock and draws in between, which keeps every counter in the game right --
/// and leaves the camera standing still on the frames that did not tick. At 120
/// fps against a 20 Hz world that is the same motion presented six times, which is
/// not merely no better than 20 fps, it is worse: the picture updates, the view
/// does not, and the eye reads the mismatch as stutter. The default tick rate
/// moving from 30 to 20 makes this **more** load-bearing, not less: a tick is now
/// 50 ms rather than 33, so the camera stands still for half again as long.
///
/// ## Why the hook point is exactly one function
///
/// Main-loop **stage 8, `func_80025A1C`**, is the whole of "build the render
/// camera from the player state", and it is nothing else. Twenty-eight
/// instructions, no branches:
///
///     a0[0] = 0x801994EC (X)        a1[0] = 0x80199504 (composed pitch)
///     a0[8] = 0x801994F4 (Z)        a1[2] = 0x80199506 (composed yaw)
///     a0[4] = 0x801994F0 (Y) + s16 0x80199548 + s16 0x8019954C - 0x640
///                                   a1[4] = 0x80199508 (composed roll)
///
/// It runs after stage 3 and before stage 13, and the two stack blocks it fills
/// are what the renderer -- and stage 9, the 3D sound listener -- are handed. So
/// it is the *only* thing between the authoritative player state and the picture.
/// A pre-hook that writes those globals and a post-hook that puts them back means
/// the carried view exists for exactly one function call: it cannot accumulate,
/// it cannot reach the collision code, and it cannot reach a save. That isolation
/// is the whole reason this is safe, and it is why the hook is not on stage 2 or
/// stage 13 -- both of which do other things, repeatedly.
///
/// **The isolation argument is about the angles, and only about the angles.** Two
/// of stage 13's own callees read the player *position* triple directly, after
/// stage 8 has run and after <see cref="After"/> has put the raw values back:
/// `func_80032400` (the first-person arm) reads `0x801994EC` and `0x801994F4`, and
/// `func_800331B4` (the world and object walks) reads all three --
/// docs/GAME_INTERNALS.md, "Stage 8 is the render camera, and it is the only copy".
/// So with <see cref="Position"/> on, the eye is carried and the origin those two
/// place their geometry against is not: on a non-tick frame the arm, the torches
/// and the creatures shear against the architecture by exactly the distance the
/// carry moved the eye, worst just before a tick lands. That is knowingly
/// inconsistent rather than fixed -- it is why the position half is a separate
/// switch (on by default), and closing it means carrying those two readers
/// too, inside the stage <see cref="ObjectSmoothing"/> already brackets.
///
/// ## Interpolate, not extrapolate
///
/// **This carried the view *forward* at first -- current angle + this tick's
/// velocity x frac -- and that is what made it bounce.** Forward extrapolation is
/// smooth only while the velocity holds. King's Field damps a turn and stops dead
/// at a wall, so the next tick's real angle is routinely *less* than the one that
/// was predicted, and the view snapped back to it -- "the camera bounces back to a
/// position it would have travelled in 20 Hz", every time a turn eased off.
///
/// So it interpolates instead: it keeps the view the game produced at the previous
/// tick and at the current one, and draws `lerp(prev, cur, frac)`. That can never
/// reach a position the game did not produce, so nothing overshoots and nothing
/// snaps. The cost is a tick of latency -- the picture trails input by up to 50 ms
/// at 20 Hz -- which for a game whose input is already sampled at the tick rate is
/// a delay of the *display*, not of the response, and is the price the smoothness
/// is worth.
///
/// The mouse is the exception, since the game applies its displacement exactly:
/// the view shows it before the tick spends it. See <see cref="MouseLead"/>.
///
/// What is carried:
/// * **Yaw and pitch**, the composed view angles at `0x80199504`/`0x80199506` --
///   the values stage 3 (`func_80028DB8`) has already folded this tick's turn into,
///   sampled fresh on every frame the world advanced. Yaw is 12-bit and wraps, so
///   the interpolation takes the shortest way round.
/// * **Position**, optionally, from `0x801994EC`/`F0`/`F4` -- the player position
///   `func_80028080` writes after the collision test, so walking a wall interpolates
///   between two positions that already slid along it, with no overshoot into it. A
///   step past <see cref="TeleportUnits"/> on an axis is a warp rather than a walk
///   and is left alone, the way <see cref="ObjectSmoothing"/> guards a placement.
/// * **The head bob and the landing offset** at `0x80199548`/`0x8019954C`, which
///   stage 8 adds to the eye height. Nothing after stage 8 reads them, so they
///   are isolated like the angles.
///
/// Both agree on time with <see cref="ObjectSmoothing"/>, which interpolates too:
/// both draw the world at `t - 1 + frac`, so nothing slides against anything else
/// *in the angles*. The position carries the caveat above.
///
/// <see cref="FramePacing.LogicPhase"/> is the fraction used, and it is continuous
/// across a tick boundary -- it does not reset to zero on the frames where the
/// world did advance -- so the view does not jump when the world catches up.
///
/// ## The compass reads the view, so it is carried with it
///
/// The compass needle is the HUD's reading of the view's heading: a damped spring
/// in stage 13's own body chases the camera's yaw, and <see cref="Stage13"/> steps
/// it on the tick. So between ticks it stood still while the view it reads turned.
/// It is carried here, by the same rule and on the same switch as the view's yaw
/// (<see cref="TickPair"/>): sampled when the spring stepped it, drawn at
/// lerp(prev, cur, phase), written into record 0 for exactly the one function that
/// reads it -- the HUD builder <c>func_80031D5C</c> -- and put back after.
/// <c>KF2_SMOOTH_COMPASS=0</c> leaves it on the tick. See "The compass is carried
/// with the view" in docs/PATCHES_AND_MODS.md.
///
/// The HP and MP gauges' lengths are the same kind of value -- a HUD record stage
/// 13 derives on the tick and only the builder reads -- so they are carried by the
/// same <see cref="HudReading"/>, sampled on <see cref="Stage13.HudTicks"/>.
/// <c>KF2_SMOOTH_GAUGES=0</c> leaves them on the tick. See "The gauges are carried
/// like the needle" in docs/PATCHES_AND_MODS.md.
/// </summary>
public static class FrameSmoothing
{
    // Stage 8: the render camera copy. The one hook.
    const uint CameraCopy = 0x80025A1C;

    // The composed view the renderer is handed: base + the three delta vectors.
    const uint ComposedPitch = 0x80199504;   // u16, wrapped to 12 bits
    const uint ComposedYaw   = 0x80199506;

    // The position triple, read as signed; a normal Y is negative.
    const uint PosX = 0x801994EC;            // u32
    const uint PosY = 0x801994F0;
    const uint PosZ = 0x801994F4;

    // Stage 8 adds these to the camera Y; only it reads them on the render side.
    const uint Bob = 0x80199548;             // s16, func_80028560's head bob
    const uint Landing = 0x8019954C;         // s16, the landing offset

    /// <summary>Units on one axis between two ticks past which the position is a
    /// warp rather than a walk, and is left where the game put it -- lerping across
    /// it would sweep the camera the width of the map. The player, the fastest
    /// thing in the game, covers about 45 units a tick, so 1024 clears any walk by
    /// twenty times. Matches <see cref="ObjectSmoothing"/>'s placement guard.</summary>
    const int TeleportUnits = 1024;

    public const string OnKey  = "kf2.smoothing.on";
    public const string PosKey = "kf2.smoothing.pos";

    /// <summary>Carry the view angles between ticks. **On by default**: the
    /// shipped picture is 60 fps against a 20 Hz world, and without this that is
    /// a faster picture of a camera not moving.</summary>
    public static bool Enabled { get; private set; } = true;

    /// <summary>Carry the position too. On with the rest of the smoothing tick;
    /// unlike the angles it is not isolated to stage 8, since two of stage 13's
    /// callees read the same triple afterwards. See the class summary.</summary>
    public static bool Position { get; private set; } = true;

    /// <summary>Carry the compass needle with the view. Part of the view's switch;
    /// <c>KF2_SMOOTH_COMPASS=0</c> separates it, for comparison.</summary>
    public static bool Compass { get; private set; } = true;

    /// <summary>Carry the HP and MP gauges' lengths. Part of the view's switch;
    /// <c>KF2_SMOOTH_GAUGES=0</c> separates it, for comparison.</summary>
    public static bool Gauges { get; private set; } = true;

    static bool _onFromEnv, _posFromEnv;

    // Last tick's and this tick's composed view, sampled on a frame the world
    // advanced on. `_cur` is what the game most recently produced, `_prev` what it
    // produced the tick before; the frame is drawn at lerp(prev, cur, phase).
    static TickPair _viewYaw = new(wraps: true);
    static ushort _prevPitch, _curPitch;
    static int _prevX, _curX, _prevY, _curY, _prevZ, _curZ;
    static short _prevBob, _curBob, _prevLand, _curLand;

    // False until a first sample exists, then until a second does. Carrying needs
    // both prev and cur to be real, exactly as ObjectSmoothing's `_live` does.
    static bool _primed, _carriable;


    // What the pre-hook overwrote, and whether it overwrote anything. Restored by
    // the post-hook; there is exactly one call in flight at a time, on one thread.
    static bool _applied;
    static ushort _pitch, _yaw;
    static uint _x, _y, _z;
    static ushort _bob, _land;

    // ---- the probe ------------------------------------------------------------

    static bool _probe;
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _reportedAt;
    static long _carried, _skipped;

    /// <summary>Frames entered and frames actually carried, counted whether or not
    /// the probe is on, so another patch can ask per frame whether this one
    /// smoothed it. <see cref="BlackProbe"/> reads the pair.</summary>
    public static long Frames { get; private set; }

    public static long Carries { get; private set; }

    /// <summary>Why the last frame was or was not carried: 0 off or not
    /// extrapolating, 1 unprimed (no two ticks to carry between yet), 2 nothing
    /// moved, 3 carried. A patch watching a transition needs the reason, because
    /// "declined, nothing moved" and "declined, unprimed" look identical from
    /// outside and only one of them is a stutter.</summary>
    public static int LastVerdict { get; private set; }

    // Why a frame was skipped, each reason in its own bucket. One number for all
    // of them cannot say the thing the probe exists to say: "nothing moving" reads
    // very differently from "the logic clock is broken", and both read differently
    // again from "the switch is off / the rate is at the tick rate". _skipOff and
    // _skipUnprimed used not to be counted at all, so the configuration the class
    // doc says this probe once diagnosed -- a boundary that never delivers a tick,
    // leaving _carriable false forever -- made Report print nothing whatever.
    static long _skipStill, _skipOff, _skipUnprimed;

    // KF2_SMOOTH_PROBE=2: every frame for 400 ms after an overlay load, what this
    // class decided and what it wrote. The event lands a frame after the game has
    // placed the player, so the frame that matters is the line before the load.
    static bool _trace;
    static double _traceUntil;
    const double TraceMs = 400.0;

    /// <summary>
    /// Frames carried since <see cref="TakeHealth"/> last asked. Counted whether
    /// or not <c>KF2_SMOOTH_PROBE</c> is on, because the reader is
    /// <see cref="FramePacing"/>'s own probe and that one has to work on its own:
    /// a session reported as having no smoothing in it is exactly the session
    /// nobody thought to turn three extra probes on for.
    /// </summary>
    static long _carries;
    static double _yawSum, _pitchSum, _posSum, _fracSum;
    static long _bobFrames, _bobOffTick;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.framesmoothing",
        Name = "Frame smoothing",
        Version = "1.0",
        Description = "Carries the view between the game's logic ticks.",
    };

    public static void Configure(string? on, string? position, string? compass, string? gauges, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) { Enabled = on != "0"; _onFromEnv = true; }
        if (!string.IsNullOrWhiteSpace(position)) { Position = position != "0"; _posFromEnv = true; }
        if (!string.IsNullOrWhiteSpace(compass)) Compass = compass != "0";
        if (!string.IsNullOrWhiteSpace(gauges)) Gauges = gauges != "0";
        _probe = probe is "1" or "2";
        _trace = probe == "2";
    }

    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            var view = RecompOne.Runtime.Runtime.View;
            if (!_onFromEnv) Enabled = view.GetBool(OnKey, Enabled);
            if (!_posFromEnv) Position = view.GetBool(PosKey, Position);
        });

        // A load is not what moves the player -- the placement is, and Before
        // sees that directly. The event only opens the trace window.
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (_trace) _traceUntil = Environment.TickCount64 + TraceMs;
        });

        HookAttach.OnOverlayLoad("smoothing", Attach);
    }

    /// <summary>Whether the pair is installed. The checkbox cannot talk
    /// <see cref="Attach"/>'s safety disable back out again: with a pre and no
    /// post nothing runs <see cref="After"/>, so the lerped camera words stay in
    /// game memory for the next tick's collision query and for the save.</summary>
    static bool _paired;

    /// <summary>Turn the carry on or off from the Video pane. Re-enabling primes
    /// again rather than lerping from whatever the last enabled frame sampled:
    /// `_prev`/`_cur` are only refreshed on a tick frame while enabled, so a spell
    /// disabled leaves them describing a heading and a position from wherever the
    /// player was then, and the first frame back would swing the camera to it --
    /// up to 180 degrees, since Delta12 takes the short way round.
    /// <see cref="AnimSmoothing.SetCarry"/> clears its slots for the same
    /// reason.</summary>
    public static void SetEnabled(bool on)
    {
        if (on && !Enabled) { _primed = false; _carriable = false; foreach (var r in _hud) r.Unprime(); }
        Enabled = on && _paired;
    }

    public static void SetPosition(bool on) => Position = on;

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, CameraCopy);
        if (target == null)
        {
            // Nothing is installed, so nothing runs -- but the checkbox and the
            // summary line would both go on claiming a feature that is not there,
            // which is the failure mode the rest of this file works to make loud.
            Enabled = false;
            Console.Error.WriteLine($"[KF2] smoothing: no game function at 0x{CameraCopy:X8}; " +
                                    "the view will step at the logic rate above it.");
            return false;
        }

        var self = typeof(FrameSmoothing);
        int n = 0;
        if (HookManager.AddPre(_self, target,
                self.GetMethod(nameof(Before), BindingFlags.Public | BindingFlags.Static)!)) n++;
        if (HookManager.AddPost(_self, target,
                self.GetMethod(nameof(After), BindingFlags.Public | BindingFlags.Static)!)) n++;

        // Committed here rather than left to FramePacing, which listens for the same
        // event and therefore attaches *first* -- Program.cs installs it first, and
        // listeners run in registration order, so its Commit has already happened by
        // the time this runs. Commit is idempotent; every other patch calls it too.
        HookManager.Commit();

        // Both halves *and* the detour: an Add* only queues, and n == 2 with a
        // commit that refused the function is a patch that claims to be smoothing
        // and never runs.
        _paired = n == 2 && HookAttach.Installed(target);

        if (!_paired)
        {
            Enabled = false;
            Console.Error.WriteLine("[KF2] smoothing: the pair did not attach" +
                                    (n == 2 ? " (queued, but the detour did not install)" : " (only half of it)") +
                                    "; the interpolation is disabled rather than left applied.");
            return false;
        }

        _hudPaired = AttachHud();
        Console.WriteLine($"[KF2] smoothing: {(Enabled ? "on" : "off")}" +
                          $"{(Position ? ", carrying position" : "")}" +
                          $"{(_hudPaired ? $", carrying the HUD{(Compass ? "" : " but the compass")}{(Gauges ? "" : " but the gauges")}" : "")}, hooked stage 8 at 0x{CameraCopy:X8}");
        return true;
    }

    static bool _hudQueued;

    /// <summary>The HUD's pair on the HUD builder, judged like the view's: both
    /// halves and the detour, or the needle and the gauges are left on the tick. The
    /// view carries either way.</summary>
    static bool AttachHud()
    {
        var target = SymbolRegistry.Resolve("game", null, HudBuilder);
        if (target == null) return false;
        if (!_hudQueued)
        {
            var self = typeof(FrameSmoothing);
            const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
            _hudQueued = HookManager.AddPre(_self, target, self.GetMethod(nameof(BeforeHud), flags)!)
                            && HookManager.AddPost(_self, target, self.GetMethod(nameof(AfterHud), flags)!);
            if (!_hudQueued) return false;
        }
        HookManager.Commit();
        bool ok = HookAttach.Installed(target);
        if (!ok) Console.Error.WriteLine("[KF2] smoothing: the HUD pair did not attach; the needle and the gauges stay on the tick.");
        return ok;
    }

    /// <summary>
    /// Draw the view at lerp(prev tick, this tick, phase), just before the one
    /// function that reads it. On a frame the world advanced on, this tick's value
    /// is re-sampled first; on every other frame the two samples stand and only the
    /// phase moves.
    /// </summary>
    public static void Before(CpuContext c, IMemory m)
    {
        _applied = false;
        Frames++;
        LastVerdict = 0;
        // Extrapolating, not Gating: Gating is true at the tick rate too, where
        // every frame ticks and LogicPhase is always ~0. Carrying there is not
        // merely pointless -- lerp(prev, cur, 0) is `prev`, so the renderer would
        // be handed last tick's camera on every frame, a permanent tick of latency
        // bought with no smoothing at all. The checkbox greys itself out on the
        // same test; this is the half that has to agree with it.
        if (!FramePacing.Extrapolating) { if (_probe) { _skipped++; _skipOff++; } return; }

        // Off, the mouse still leads: the view is the tick's plus what the mouse
        // has moved since, and nothing is interpolated.
        if (!Enabled)
        {
            if (_probe) { _skipped++; _skipOff++; }
            MouseLead(m, 1.0);
            if (_aheadYaw != 0 || _aheadPitch != 0) LeadOnly(m);
            return;
        }

        // Roll forward on a tick, then re-read the values the game just produced.
        // Stage 3 (func_80028DB8 for the angles, func_80028080 for the position)
        // runs before stage 8, so on a tick frame these already hold the new tick.
        if (FramePacing.TickedThisFrame)
        {
            int stepX = _curX - _prevX, stepY = _curY - _prevY, stepZ = _curZ - _prevZ;
            _prevPitch = _curPitch;
            _prevX = _curX; _prevY = _curY; _prevZ = _curZ;
            _prevBob = _curBob; _prevLand = _curLand;

            _viewYaw.Roll(m.ReadU16(ComposedYaw));
            _curPitch = m.ReadU16(ComposedPitch);
            _curX = (int)m.ReadU32(PosX);
            _curY = (int)m.ReadU32(PosY);
            _curZ = (int)m.ReadU32(PosZ);
            _curBob = (short)m.ReadU16(Bob);
            _curLand = (short)m.ReadU16(Landing);

            _carriable = _primed;   // both prev and cur are real only after two samples
            _primed = true;

            // A placement landed in this tick: the pair straddles it. Put `prev`
            // a walk step behind `cur`, the last tick's step, so the view keeps
            // its tick of lag and its speed instead of dropping both. Resetting
            // prev = cur here lurched the view forward and then held it a tick.
            if (Math.Abs(_curX - _prevX) > TeleportUnits || Math.Abs(_curY - _prevY) > TeleportUnits ||
                Math.Abs(_curZ - _prevZ) > TeleportUnits)
            {
                _prevX = _curX - stepX; _prevY = _curY - stepY; _prevZ = _curZ - stepZ;
                OffTickMoves++;
            }
        }

        // Off a tick the live view must still be `cur`; if the game moved it, it
        // placed the player. Carrying the old pair over it drew the new area from
        // the old camera (the crossing's black frame); resetting the pair onto it
        // dropped the tick of lag at once (a lurch forward). Shift the whole pair
        // by the placement instead, so the view carries straight through it. See
        // "What the crossing frame actually is" in docs/PATCHES_AND_MODS.md.
        else if (_primed && Moved(m))
        {
            int x = (int)m.ReadU32(PosX), y = (int)m.ReadU32(PosY), z = (int)m.ReadU32(PosZ);
            ushort yaw = m.ReadU16(ComposedYaw), pitch = m.ReadU16(ComposedPitch);
            _prevX += x - _curX; _prevY += y - _curY; _prevZ += z - _curZ;
            _viewYaw.Shift(yaw);
            _prevPitch = (ushort)(_prevPitch + (pitch - _curPitch));
            _curX = x; _curY = y; _curZ = z; _curPitch = pitch;
            OffTickMoves++;
        }

        double frac = FramePacing.LogicPhase;
        var (yawLead, pitchLead) = MouseLead(m, frac);
        bool leading = _aheadYaw != 0 || _aheadPitch != 0;

        // Not gated on a small phase: interpolation must overwrite the live globals
        // even at frac ~= 0, because on a tick frame they hold `cur` (the new tick)
        // and the frame is meant to draw `prev`. Skipping there would leave the new
        // value on screen and put a snap back the other way.
        if (!_carriable)
        {
            LastVerdict = 1;
            if (_probe) { _skipped++; _skipUnprimed++; }
            if (leading) LeadOnly(m);
            return;
        }

        int yawD = _viewYaw.Delta;
        int pitchD = S12(_curPitch) - S12(_prevPitch);

        int dx = _curX - _prevX, dy = _curY - _prevY, dz = _curZ - _prevZ;

        bool posLive = Position &&
                       Math.Abs(dx) <= TeleportUnits &&
                       Math.Abs(dy) <= TeleportUnits &&
                       Math.Abs(dz) <= TeleportUnits &&
                       (dx != 0 || dy != 0 || dz != 0);

        int bobD = _curBob - _prevBob, landD = _curLand - _prevLand;
        bool eyeLive = bobD != 0 || landD != 0;

        if (yawD == 0 && pitchD == 0 && !posLive && !eyeLive)
        {
            LastVerdict = 2;
            if (_probe) { _skipped++; _skipStill++; }
            if (leading) LeadOnly(m);
            return;
        }

        _pitch = m.ReadU16(ComposedPitch);
        _yaw   = m.ReadU16(ComposedYaw);
        _x = m.ReadU32(PosX);
        _y = m.ReadU32(PosY);
        _z = m.ReadU32(PosZ);
        _bob = m.ReadU16(Bob);
        _land = m.ReadU16(Landing);
        _applied = true;
        _carries++;

        // Rounded with the mouse's lead rather than apart, or the two roundings
        // wobble the view by a unit as the phase moves.
        int yawStep = (int)Math.Round(yawD * frac + yawLead);
        int pitchStep = (int)Math.Round(pitchD * frac + pitchLead);

        // Stepped from the game's own previous-tick word, in the game's own 16-bit
        // domain, rather than rebuilt out of the 12-bit angle. The composer
        // (func_8002A550) sums four zero-extended halfwords and stores the low
        // sixteen bits, so the upper four are whatever that arithmetic left there
        // -- for a negative pitch, the s16 sign extension (-700 is 0xFD44). Masking
        // to 0xFFF wrote 0x0D44 back, which every consumer that indexes a sin/cos
        // table with `& 0xFFF` agrees with and any consumer that reads the field as
        // the s16 it is typed as (docs/GAME_INTERNALS.md, "Stage 8 is the render
        // camera") sees as +3396. Adding the step to the raw word is bit-exact at
        // phase 0 and agrees with `cur` mod 4096 at phase 1.
        m.WriteU16(ComposedYaw, (ushort)(_viewYaw.Prev + yawStep));
        m.WriteU16(ComposedPitch, (ushort)(_prevPitch + pitchStep));

        if (posLive)
        {
            m.WriteU32(PosX, (uint)(_prevX + (int)Math.Round(dx * frac)));
            m.WriteU32(PosY, (uint)(_prevY + (int)Math.Round(dy * frac)));
            m.WriteU32(PosZ, (uint)(_prevZ + (int)Math.Round(dz * frac)));
        }

        // The bob is a stepped |sin| sampled per tick; carried like the angles,
        // since nothing after stage 8 reads it. See "The head bob" in
        // docs/PATCHES_AND_MODS.md.
        if (eyeLive)
        {
            m.WriteU16(Bob, (ushort)(short)(_prevBob + (int)Math.Round(bobD * frac)));
            m.WriteU16(Landing, (ushort)(short)(_prevLand + (int)Math.Round(landD * frac)));
        }

        Carries++;
        LastVerdict = 3;

        if (_probe)
        {
            _carried++;
            _yawSum += Math.Abs(yawD * frac);
            _pitchSum += Math.Abs(pitchD * frac);
            if (posLive) _posSum += Math.Abs(dx * frac) + Math.Abs(dz * frac);
            _fracSum += frac;
            if (eyeLive)
            {
                _bobFrames++;
                if (Math.Round(bobD * frac) != 0 && Math.Round(bobD * frac) != bobD) _bobOffTick++;
            }
        }
    }

    /// <summary>
    /// Put the state back the moment stage 8 has read it. Everything downstream --
    /// stage 13, the next tick's collision, a save -- then sees exactly what the
    /// game wrote.
    /// </summary>
    public static void After(CpuContext c, IMemory m)
    {
        if (_trace && Environment.TickCount64 < _traceUntil)
            Console.WriteLine($"[smooth] verdict {LastVerdict} " +
                              $"tick {(FramePacing.TickedThisFrame ? 1 : 0)} " +
                              $"frac {FramePacing.LogicPhase:0.00} " +
                              $"applied {(_applied ? 1 : 0)} " +
                              $"yaw prev {_viewYaw.Prev:X4} cur {_viewYaw.Cur:X4} " +
                              $"live {m.ReadU16(ComposedYaw):X4} " +
                              $"pitch live {m.ReadU16(ComposedPitch):X4} " +
                              $"pos ({(int)m.ReadU32(PosX)},{(int)m.ReadU32(PosZ)}) " +
                              $"prev ({_prevX},{_prevZ}) cur ({_curX},{_curZ}) offtick {OffTickMoves}");

        if (_applied)
        {
            m.WriteU16(ComposedPitch, _pitch);
            m.WriteU16(ComposedYaw, _yaw);
            m.WriteU32(PosX, _x);
            m.WriteU32(PosY, _y);
            m.WriteU32(PosZ, _z);
            m.WriteU16(Bob, _bob);
            m.WriteU16(Landing, _land);
            _applied = false;
        }

        if (_probe) Report();
    }

    // ---- the HUD's readings -------------------------------------------------

    /// <summary>The HUD builder: the one function that reads the records below.</summary>
    const uint HudBuilder = 0x80031D5C;

    /// <summary>
    /// A halfword of a HUD record that stage 13 steps on the tick and only the HUD
    /// builder reads: the needle's yaw, and the two gauges' lengths. Sampled when
    /// <c>steps</c> moves, drawn at lerp(prev, cur, phase) for the builder and put
    /// back after it. Stands down, and unprimes, whenever <c>onTick</c> says the
    /// value is not stepped on the tick. A menu's pass
    /// (<see cref="Stage13.DrawScene"/>) draws the world as it stands, with the view
    /// and the objects uncarried, so the reading is left where it stands too.
    /// </summary>
    sealed class HudReading(string name, uint address, bool wraps, Func<bool> onTick, Func<long> steps)
    {
        public readonly string Name = name;
        TickPair _pair = new(wraps);
        long _seen = -1;
        bool _primed, _carriable, _applied;
        ushort _live, _drawn;
        public long Carried, Frames, Moves;

        public void Unprime() { _primed = _carriable = false; _seen = -1; }

        public void Before(IMemory m, bool enabled)
        {
            _applied = false;
            bool carrying = enabled && FramePacing.Extrapolating && onTick();
            if (!carrying) Unprime();
            if (!Stage13.InFrame) return;

            ushort live = m.ReadU16(address);
            ushort drawn = live;
            if (carrying)
            {
                long n = steps();
                if (n != _seen)
                {
                    _seen = n;
                    _pair.Roll(live);
                    _carriable = _primed;
                    _primed = true;
                }
                // Moved by something other than its step: a placement, as for the view.
                else if (_primed && live != _pair.Cur) _pair.Shift(live);

                if (_carriable && _pair.Delta != 0)
                {
                    drawn = (ushort)(_pair.Prev + _pair.Step(FramePacing.LogicPhase));
                    _live = live;
                    m.WriteU16(address, drawn);
                    _applied = true;
                    Carried++;
                }
            }

            Frames++;
            if (drawn != _drawn) Moves++;
            _drawn = drawn;
        }

        /// <summary>Put the stepped value back the moment the builder has drawn it,
        /// so the game's next step starts from what the game stepped.</summary>
        public void After(IMemory m)
        {
            if (!_applied) return;
            m.WriteU16(address, _live);
            _applied = false;
        }

        public string Report() => $"{Name} carried on {Carried}/{Frames} frames, drawn at a new value on {Moves}";
        public void ClearReport() => Carried = Frames = Moves = 0;
    }

    static readonly HudReading[] _hud =
    [
        new("compass", Stage13.NeedleYaw, wraps: true, () => Compass && Stage13.NeedleOnTick, () => Stage13.NeedleSteps),
        new("gauge 1", Stage13.GaugeLengths[0], wraps: false, () => Gauges && Stage13.HudOnTick, () => Stage13.HudTicks),
        new("gauge 2", Stage13.GaugeLengths[1], wraps: false, () => Gauges && Stage13.HudOnTick, () => Stage13.HudTicks),
    ];

    static bool _hudPaired;

    public static void BeforeHud(CpuContext c, IMemory m)
    {
        foreach (var r in _hud) r.Before(m, Enabled);
    }

    public static void AfterHud(CpuContext c, IMemory m)
    {
        foreach (var r in _hud) r.After(m);
    }

    // ---- the mouse leads the tick ------------------------------------------------
    //
    // See "The mouse leads the tick" in docs/INPUT.md. A mouse turn is a
    // displacement the game adds to the angle exactly, so the view can show it
    // the frame it happens instead of a tick or two later: the tick's angle, plus
    // whatever the mouse has moved since, plus the part of the last tick's mouse
    // turn the interpolation has not reached yet.

    const int PitchLimit = 0x2BC;

    static long _leadFrame = -1;
    static int _tickYaw, _tickPitch;

    // The part of the lead the tick has not spent yet: all of it, on a frame
    // that is not interpolated and so already shows the tick's own turn.
    static int _aheadYaw, _aheadPitch;
    static long _leadFrames, _leadTicks;
    static double _leadSum, _leadMiss;

    /// <summary>The yaw and pitch to add to this frame's view for the mouse.
    /// <paramref name="frac"/> is the interpolation's phase; 1 when nothing is
    /// interpolated.</summary>
    static (double Yaw, double Pitch) MouseLead(IMemory m, double frac)
    {
        _aheadYaw = _aheadPitch = 0;
        if (!Mouse.Lead) return (0, 0);
        Mouse.Poll();

        // On the tick the look routine spent the mouse, what the game actually
        // turned by for it; the lerp from prev reaches it only at phase 1.
        if (FramePacing.FirstWalkOfTick(ref _leadFrame))
        {
            var spent = Mouse.SpentThisFrame(m);
            (_tickYaw, _tickPitch) = spent is { } t ? (t.Yaw, t.Pitch) : (0, 0);
            if (_probe && spent is { } q)
            {
                _leadTicks++;
                _leadMiss += Math.Abs(q.Yaw - q.AskedYaw) + Math.Abs(q.Pitch - q.AskedPitch);
            }
        }

        double keep = 1.0 - frac;
        var (turn, look) = Mouse.Pending;

        _aheadYaw = (int)Math.Round(turn);
        double yaw = _tickYaw * keep + turn;

        // Held inside the game's pitch limit, off the base angle the next tick
        // will add to, so looking into the limit does not overshoot and come back.
        // The base is a 12-bit angle: just above level it is 0x0FFx, and only the
        // clamp writes the sign-extended 0xFD44.
        int basePitch = S12(m.ReadU16(Analog.Pitch));
        int ahead = Math.Clamp(basePitch + (int)Math.Round(look), -PitchLimit, PitchLimit) - basePitch;
        _aheadPitch = ahead;
        double pitch = _tickPitch * keep + ahead;

        if (_probe && (yaw != 0 || pitch != 0))
        {
            _leadFrames++;
            _leadSum += Math.Abs(turn) + Math.Abs(ahead);
        }
        return (yaw, pitch);
    }

    /// <summary>The live view plus the unspent lead, on a frame nothing is
    /// interpolated; <see cref="After"/> puts it back.</summary>
    static void LeadOnly(IMemory m)
    {
        _pitch = m.ReadU16(ComposedPitch);
        _yaw = m.ReadU16(ComposedYaw);
        _x = m.ReadU32(PosX);
        _y = m.ReadU32(PosY);
        _z = m.ReadU32(PosZ);
        _bob = m.ReadU16(Bob);
        _land = m.ReadU16(Landing);
        _applied = true;

        m.WriteU16(ComposedYaw, (ushort)(_yaw + _aheadYaw));
        m.WriteU16(ComposedPitch, (ushort)(_pitch + _aheadPitch));
    }

    /// <summary>Placements the pair was shifted across: the game moved the view
    /// between ticks, or a tick's step was a teleport.</summary>
    public static long OffTickMoves { get; private set; }

    static bool Moved(IMemory m) =>
        (int)m.ReadU32(PosX) != _curX || (int)m.ReadU32(PosY) != _curY ||
        (int)m.ReadU32(PosZ) != _curZ || m.ReadU16(ComposedYaw) != _viewYaw.Cur ||
        m.ReadU16(ComposedPitch) != _curPitch;

    /// <summary>Read a 12-bit angle as signed, in [-2048, 2047]. Pitch is a small
    /// signed angle (±0x2BC) stored this way; yaw uses the whole range.</summary>
    static int S12(int raw)
    {
        int v = raw & 0xFFF;
        return v >= 0x800 ? v - 0x1000 : v;
    }

    /// <summary>
    /// A value as the game produced it on the last two ticks: the view's yaw, and the
    /// HUD's readings. The frame draws it at <c>Prev + Step(phase)</c>, stepped from
    /// the game's own previous word in its own 16-bit domain (see
    /// <see cref="Before"/>). A 12-bit angle that wraps is stepped the short way
    /// round the wrap; anything else as a signed halfword.
    /// </summary>
    struct TickPair(bool wraps)
    {
        public ushort Prev, Cur;
        readonly bool _wraps = wraps;

        /// <summary>A tick: this tick's value becomes last tick's.</summary>
        public void Roll(ushort next)
        {
            Prev = Cur;
            Cur = next;
        }

        /// <summary>A placement off the tick: move the whole pair by it, so the lag
        /// and the speed carry straight through.</summary>
        public void Shift(ushort live)
        {
            Prev = (ushort)(Prev + (live - Cur));
            Cur = live;
        }

        public readonly int Delta => _wraps ? Delta12(Prev, Cur) : (short)Cur - (short)Prev;

        public readonly int Step(double phase) => (int)Math.Round(Delta * phase);
    }

    /// <summary>
    /// The shortest signed distance from one 12-bit angle to another, in
    /// [-2048, 2048]. Interpolating a yaw with this instead of the raw difference is
    /// what stops a turn through the 0/4095 wrap from spinning the long way round.
    /// </summary>
    static int Delta12(ushort from, ushort to)
    {
        int d = (to & 0xFFF) - (from & 0xFFF);
        if (d > 2048) d -= 4096;
        else if (d < -2048) d += 4096;
        return d;
    }

    /// <summary>
    /// One word for what this patch is *doing*, for `KF2_FPS_PROBE=1`. It consumes
    /// the window it reports on, hence `Take`.
    ///
    /// The pacing line used to end at <see cref="FramePacing.Extrapolating"/>,
    /// which is a pure function of <c>LogicMode</c>, <c>Enabled</c>,
    /// <c>TargetFps</c> and <c>LogicHz</c> -- configuration, not a reading. It says
    /// the smoothing *may* carry and can never say whether it *did*, so a session
    /// with a dead smoother printed a line byte-identical to a healthy one. That
    /// was measured from play: the probe was aimed one layer too high.
    ///
    /// The words are ordered by how far the patch got, so the first one that is
    /// not "carrying" names the layer to look at:
    /// <list type="bullet">
    /// <item><c>unpaired</c> -- the detour never installed, so
    /// <see cref="SetEnabled"/> can never turn it on and the Video checkbox is
    /// inert for the whole session. <see cref="Attach"/> says so on stderr and
    /// nothing else ever mentions it again.</item>
    /// <item><c>off</c> -- the saved setting or the env var reads off.</item>
    /// <item><c>at the tick rate</c> -- no frame lands between two ticks.</item>
    /// <item><c>unprimed</c> -- attached and on, but two ticks have not been
    /// sampled since the last <c>OverlayLoadedEvent</c>, which fires for the fdat
    /// area modules as well as the executables.</item>
    /// <item><c>idle</c> -- running, with nothing moving to carry.</item>
    /// <item><c>carrying</c> -- the healthy reading.</item>
    /// </list>
    /// </summary>
    public static string TakeHealth()
    {
        long n = _carries;
        _carries = 0;

        if (!_paired) return "unpaired";
        if (!Enabled) return "off";
        if (!FramePacing.Extrapolating) return "at the tick rate";
        if (!_carriable) return "unprimed";
        return n > 0 ? "carrying" : "idle";
    }

    static void Report()
    {
        double now = _clock.Elapsed.TotalMilliseconds;
        if (now - _reportedAt < 2000.0) return;
        _reportedAt = now;

        long total = _carried + _skipped;

        if (_carried == 0)
            // Not returned on total == 0: zero frames reaching the hook at all is
            // itself the finding, and it is the one this probe was written for.
            Console.WriteLine($"[KF2] smoothing: 0 of {total} frames carried -- " +
                              $"{_skipStill} with nothing moving, " +
                              $"{_skipUnprimed} before two ticks were sampled, " +
                              $"{_skipOff} off or at the tick rate" +
                              $"{(FramePacing.Extrapolating ? "" : ", not extrapolating")}");
        else
            Console.WriteLine($"[KF2] smoothing: {_carried}/{total} frames carried, " +
                              $"mean phase {_fracSum / _carried:0.00} tick, " +
                              $"yaw {_yawSum / _carried:0.0} u, pitch {_pitchSum / _carried:0.0} u, " +
                              $"pos {_posSum / _carried:0.0} u, " +
                              $"bob carried on {_bobFrames} ({_bobOffTick} between the ticks' values)");
        Console.WriteLine($"[KF2] smoothing: {string.Join("; ", _hud.Select(r => r.Report()))}" +
                          $"{(Stage13.NeedleOnTick ? "" : ", the needle is not on the tick")}");
        foreach (var r in _hud) r.ClearReport();

        if (_leadFrames > 0 || _leadTicks > 0)
            Console.WriteLine($"[KF2] smoothing: mouse led {_leadFrames} frames, " +
                              $"mean {(_leadFrames > 0 ? _leadSum / _leadFrames : 0):0.0} u ahead of the tick; " +
                              $"{_leadTicks} ticks spent it, mean |applied - asked| " +
                              $"{(_leadTicks > 0 ? _leadMiss / _leadTicks : 0):0.00} u");
        _leadFrames = _leadTicks = 0;
        _leadSum = _leadMiss = 0.0;

        _carried = _skipped = _skipStill = _skipOff = _skipUnprimed = 0;
        _yawSum = _pitchSum = _posSum = _fracSum = 0.0;
        _bobFrames = _bobOffTick = 0;
    }
}
