using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using RecompOne.Runtime.Sdk;

namespace Kf2;

/// <summary>
/// What frame rate the port runs at, and how the game's own clock is held to
/// <see cref="LogicHz"/> while it does.
///
///     KF2_FPS=20      (default) draw at 20, the rate the console achieved
///     KF2_FPS=60      render at 60, tick the world at 20
///     KF2_FPS=144     any number is allowed; the world still ticks at 20
///     KF2_FPS=off     no pacing at all -- the picture is uncapped, the world is not
///     KF2_TICKRATE=30 tick the world at what the game's code asks for instead
///     KF2_FPS_LOGIC=full   do not gate anything; scale the movement deltas instead
///     KF2_FPS_GATE=80037C0C+8002A550+80040348+80046A60+8004910C+80033FBC+8002DC78
///                     the stages to tick at LogicHz -- naming any replaces the set
///
/// It is also a setting, under Video -- see Kf2.Settings.FramePacingPage. The
/// saved choice is read on RuntimeReadyEvent rather than in Configure, since
/// ConfigManager only loads inside HostWindow.Initialize, after Program.cs; the
/// environment variable still wins over it.
///
/// King's Field's speed *is* its frame rate -- everything advances a fixed amount
/// per loop iteration -- so a port that draws more often has to decide what to do
/// about that, and this is where that decision lives.
///
/// ## What used to hold the port at 30, and why none of it does now
///
/// Three separate gates, and they are worth keeping straight because only the
/// first is the game's:
///
/// 1. **The game's own frame gate, `func_80017880`.** Stage 13 (the renderer)
///    ends by calling it. It spins on a vblank credit at 0x801B6CA8 -- bumped by
///    the game's own vblank callback, `func_80017850` -- until the credit reaches
///    **2**, then zeroes it. That literal 2 is the game's frame rate, in software.
///    Since patches/recompone/0021 ticks the emulated vblank on a wall-clock 60 Hz
///    grid, this alone paces the port to exactly 30 fps whatever the host does.
///    **<see cref="BeforeFrameGate"/> now skips it at every rate, not only above
///    30**, because the rate the world is held to is no longer the rate that
///    literal asks for -- see below.
/// 2. **RecompOne's FrameClock**, which used to be a hard-coded 60 Hz applied per
///    *VSync call*. patches/recompone/0025 makes it settable; this class sets it
///    as a permissive ceiling rather than as the pacer, because a frame can carry
///    more than one VSync call and a per-call throttle therefore cannot express a
///    frame rate.
/// 3. **<see cref="Floor"/>**, below -- the port's own deadline at the frame
///    boundary, which is the only one of the three that knows where a frame ends,
///    and now the only thing pacing the picture at any rate.
///
/// With the game's gate gone a rendered frame carries **exactly one VSync call**
/// at every rate -- the presenter's, `func_8002E0FC`, which does VSync(0)
/// immediately before its DrawOTag. The gate's spin calls were the second one.
/// That makes the frame boundary in <see cref="AfterDrawOTag"/> the same shape at
/// 20 fps as at 144.
///
/// ## The logic clock
///
/// The game's gate is removed and the loop runs at whatever the floor allows -- so
/// the world would advance a fixed amount that many times a second.
/// The fix is a fixed timestep: a wall-clock accumulator ticks at
/// <see cref="LogicHz"/>, and the main-loop stages that hold per-tick state run
/// only on a frame where it ticked. **The accumulator is the only thing holding
/// the world to a rate now**, which is why it runs in every configuration,
/// including uncapped and including a render rate below the tick rate. Everything the game counts in frames -- the
/// death sequence, spell lifetimes, buff timers, the poison tick, entity AI --
/// then keeps hardware timing exactly, at any rate.
///
/// What is gated is everything found to hold per-tick state that cannot draw:
///
///     2  func_80037C0C   the object-table state machine: 396 slots of 0x44 at
///                        0x80177714, dispatched on the type byte at rec+0x4
///                        through a 224-entry jump table at 0x8001191C. Every
///                        world prop that moves is in here -- doors, the
///                        drawbridge, the minecart, the crystals -- and it writes
///                        their position VECTOR itself, so ungated they moved at
///                        the render rate
///     3  func_8002A550   pad read, turn, walk, angle fold, the death counter at
///                        0x8019951A, the poison tick, the buff timers at
///                        0x80199472..0x80199482, and 0x80199488, the global frame
///                        counter it bumps last
///     4  func_80040348   the 200-record entity table at 0x8016C544. Its AI runs
///                        one entity in four off 0x80175908 &amp; 3, which stays
///                        right for free once the stage itself is gated
///     5  func_80046A60   128 effect/projectile slots at 0x8019CC6C, each with a
///                        lifetime at rec+0x0E decremented once per call. Ungated,
///                        every spell and effect expires at the render rate
///     6  func_8004910C   the area module's per-frame entry, slot 1 of the module
///                        header, reached as *(u32*)(*(u32*)0x8017E068 + 4). Six
///                        of the nine modules leave it an empty `jr $ra`; fdat11,
///                        fdat14 and fdat20 run proximity and trigger logic there
///     13 func_80033FBC   the fade state machine stage 13 calls -- state byte
///                        0x80192D42, brightness 0x80192D44 stepping +0x14 in and
///                        -0x14 out, hold counter 0x80192D43. Its own subtree is
///                        three functions and none can draw, so the fade is gated
///                        even though its caller is the renderer
///     13 func_8002DC78   the animated-texture updater stage 13 calls -- eight
///                        slots at 0x80192D58 (stride 0x18), each a scroll phase at
///                        rec+0x4 re-uploaded to VRAM through func_80060624. The
///                        water, the fire and the creatures' skins. Its subtree is
///                        the VRAM upload alone, so like the fade it is gated though
///                        its caller draws; ungated it scrolled at the render rate
///
/// **Can it draw** is the test each of those had to pass, because a stage that
/// submits primitives cannot be skipped -- at 120 fps three frames in four would
/// have nothing from it. That is checked against the emitted C#: the subtree of a
/// gated function must contain no `DrawOTag`, `VSync`, `PutDispEnv` or
/// `PutDrawEnv`. **Stages 2 and 3 are the two exceptions and they are the same
/// shape**: what they reach is **stage 13 itself**, not a drawing primitive of
/// their own. Stage 3 gets there through `func_80029CBC -> func_80018E80` (the
/// in-game menu) and `func_80037B5C` (the transition fade), both of which take
/// the main loop over and render their own frames, and through `func_80022DC4`
/// (the menu blip) which VSyncs without drawing. Stage 2's case is strictly
/// narrower: enumerating every function in its subtree that calls a submitting or
/// presenting entry point directly leaves exactly one, reached by exactly one
/// edge -- `func_80037C0C -> func_80037B5C -> func_800342D8 -> func_8002E0FC` --
/// plus the area modules' own message-box and cutscene loops (`func_80047000`,
/// `func_80048208`, `func_8004831C`) off its indirect arm, which reach stage 13
/// the same way. Those are *extra* renders inside the stage rather than the
/// frame's own: the main loop still runs stage 13 afterwards, so skipping either
/// stage costs a redundant draw and not a frame's picture. Skipping decides
/// whether such a loop is entered; it cannot cut one in half, since once entered
/// the stage is on the stack and drives its own frames. The cost is that entering
/// a fade or a cutscene can be deferred by up to one tick. (`scripts/check_gate.py`
/// re-derives this; both are recorded exceptions there, and the single-path
/// version of the stage 3 sentence was wrong.)
///
/// **What this still does not cover, stated rather than discovered later.**
/// The **compass needle's spring at 0x8006E608** is in stage 13's own body rather
/// than in a callee, so a gate cannot reach it; above the tick rate the needle
/// settles sooner. Stage 13 is C# now (<see cref="Stage13"/>), so it is one line. **The modal loops are covered, and not by this
/// class**: `func_80037B5C` (the transition fade), the item-use and spell-cast
/// animations and the menu all step once per *rendered* frame inside a loop of
/// their own, which a gate cannot reach because it decides only whether such a
/// loop is entered. <see cref="LoopPacing"/> holds the loop's *body* to one run per
/// tick instead, and fills the gap between its iterations by **redrawing** -- stage
/// 13 called again, with the two view pointers the loop itself passed it, at the
/// phase the frame now stands at. The world then advances at <see cref="LogicHz"/>
/// while the picture is drawn at the render rate, with the smoothing carrying it.
/// And **the object
/// record's rec+0x40 on two slots survived gating stage 2 at ratio 3.69**: that
/// one is stepped by stage 13's own object pass `func_800331B4` (proved by gating
/// it as a probe -- 53.4/s fell to 17.6/s), where it is an ambient-sound retrigger
/// deadline in vblank units against 0x801B6CAC. `func_800331B4` steps the timer
/// and draws the models in one loop, so no whole-function hook separates them.
///
/// The camera would then move <see cref="LogicHz"/> times a second while the
/// picture updated more often, which looks worse than not drawing faster at all. <see cref="FrameSmoothing"/> is the
/// other half of this and carries the view between ticks.
///
/// This lives in `patches/` and not in `mods/` on purpose. It is a correctness
/// fix that has to be on: shipping it as a runtime-loaded package would let it be
/// absent, disabled or fail to compile, and the failure would look like the game
/// simply running too fast. The genuinely optional extras are real mods -- see
/// mods/kf2debug.
/// </summary>
public static class FramePacing
{
    /// <summary>
    /// Ticks a second the game's own world is held to, whatever the port draws at.
    ///
    /// **20, not 30, and that is a judgement rather than a reading.** The game's
    /// own frame gate asks for two vblanks -- 30 fps -- and for a long time this
    /// was 30 for that reason. But the literal is what the code *asks* for, not
    /// what the console *achieved*: King's Field is heavy enough that the loop
    /// lands in the three-vblank band under load, and since the game's speed is
    /// its frame rate, that band is the speed the game was played at. This port
    /// has an HLE GPU and native MIPS, so it makes the two-vblank deadline every
    /// single frame and never bands down -- which is why it needs to be told.
    ///
    /// **The literal 2 is a ceiling, not a target**, and it is worth being exact
    /// about that: <c>func_80017880</c> spins *while* the vblank credit is below 2,
    /// so it forbids a frame faster than 30 and asks nothing of a slower one. A
    /// limit that was never the binding constraint is not a statement of intended
    /// speed. 20 is: it is the band the console actually landed in, and King's
    /// Field's speed *is* its frame rate, so it is the speed the game was built and
    /// tuned at.
    ///
    /// No counter here can settle it, and **that is not a reason to make the player
    /// settle it**: 20 is the rate, and 30 -- the fastest the gate permits -- is a
    /// comparison. It lives on the console under <c>KF2_TICKRATE</c>, which is where
    /// comparisons belong; the combo that used to offer both is gone. See "Frame
    /// pacing" in docs/PATCHES_AND_MODS.md.
    /// </summary>
    public static double LogicHz { get; private set; } = 20.0;

    const double SpinMs = 1.5;   // spin the last stretch; Thread.Sleep granularity is a few ms

    // libgpu DrawOTag, per overlay. The frame boundary: past it the frame's
    // drawing is done and the loop is about to VSync and show it.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    // libetc VSync, per overlay -- the same three addresses config/kf2.json binds
    // to LibEtc.VSync. A `replace` in the config and a `pre` here coexist:
    // HookManager runs every pre before the replacement, which is what
    // mods/framestats already relies on. Counting *calls* here is what makes the
    // frame boundary below rate-independent; VSyncEvent is a vblank, not a call.
    static readonly (string Overlay, uint Addr)[] VSyncThunk =
    [
        .. SdkAddr.VSync,
    ];

    /// <summary>
    /// The game's own frame gate: spin until the vblank callback has counted two,
    /// then zero the count. Called from stage 13. Skipping it is what lets the
    /// loop run faster than 30.
    /// </summary>
    const uint FrameGate = 0x80017880;

    /// <summary>The u32 the game's vblank callback (`func_80017850`) bumps and the
    /// frame gate consumes. Zeroed here when the gate is skipped, so that going
    /// back to 30 mid-session finds it where the game left it.</summary>
    const uint VBlankCredit = 0x801B6CA8;

    /// <summary>Frames a second a first run draws at. The world stays at
    /// <see cref="LogicHz"/>; this is the picture only. 0 is uncapped -- no
    /// pacing at all, which draws as fast as the host can. It no longer runs the
    /// *game* fast: the logic clock holds the world to <see cref="LogicHz"/> in
    /// every configuration, uncapped included.</summary>
    public const double DefaultFps = 60;

    /// <summary>Frames a second the port aims for.</summary>
    public static double TargetFps { get; private set; } = DefaultFps;

    /// <summary>False only when uncapped.</summary>
    public static bool Enabled => TargetFps > 0.0;

    /// <summary>Where the chosen rate is kept between runs.</summary>
    public const string FpsKey = "kf2.framepacing.fps";

    /// <summary>The key this used to be kept under, as a vblank divisor. Read once
    /// and converted, so an existing config keeps the rate it had.</summary>
    public const string VBlankKey = "kf2.framepacing.vblanks";

    // The tick rate had a key here (kf2.framepacing.logichz) and a combo under
    // Video, and both are gone: 20 is the speed the game was played at, and the
    // 30 beside it was a comparison rather than a preference. KF2_TICKRATE still
    // takes any rate. The key is deliberately not read any more -- a saved 30 with
    // no combo left to see it would be a session running half again too fast with
    // nothing in the window to explain it.

    /// <summary>What to do about the game advancing once per loop iteration.</summary>
    public enum Logic
    {
        /// <summary>Tick the world at <see cref="LogicHz"/> however often the port
        /// draws. Every counter in the game keeps a consistent rate, and the game's
        /// own frame gate is never allowed to set one.</summary>
        Fixed,

        /// <summary>Run every stage every frame and scale the movement deltas
        /// instead -- true input and collision at the render rate, but every
        /// per-tick *counter* in the game then runs at that rate too. A comparison
        /// mode; see <see cref="FullRateLogic"/>.</summary>
        Full,
    }

    public static Logic LogicMode { get; private set; } = Logic.Fixed;

    /// <summary>Main-loop stages run at <see cref="LogicHz"/> rather than at the
    /// render rate.</summary>
    static readonly List<uint> _gated = [];

    /// <summary>
    /// What ticks at <see cref="LogicHz"/> when <c>KF2_FPS_GATE</c> names nothing:
    /// main-loop stages 2, 3, 4, 5 and 6, plus stage 13's fade state machine and
    /// its animated-texture updater. Hooking
    /// them unconditionally is what makes the rate a setting rather than a launch
    /// argument. It costs nothing when the render rate equals the tick rate, where
    /// the accumulator ticks on every frame and <see cref="BeforeStage"/> always
    /// runs the original.
    ///
    /// Every one of them was checked for whether it can *draw* before being put
    /// here, because a stage that submits primitives cannot be skipped -- the
    /// picture would flicker at three frames in four. Stages 2 and 3 are the two
    /// recorded exceptions, and they are the same shape: what they reach is stage
    /// 13 itself, called as an extra render from inside a modal loop of their own,
    /// never a primitive the frame's picture depends on. `scripts/check_gate.py`
    /// re-derives that and holds both reasons. See "Any frame rate" in
    /// docs/PATCHES_AND_MODS.md.
    /// </summary>
    static readonly uint[] DefaultGate =
    [
        0x80037C0C,   // 2  the object-table state machine. It walks the 396 slots
                      //    of 0x44 at 0x80177714, skipping a slot whose type byte
                      //    at rec+0x4 is 0xFF, publishes the record to 0x8017E04C
                      //    and its 0x18-stride definition (indexed by the u16 at
                      //    rec+0x6) to 0x8017E048, then dispatches on that type
                      //    byte through a 224-entry jump table at 0x8001191C --
                      //    thirty distinct arms, plus an indirect arm into the
                      //    area module's own handler. This is every world prop
                      //    that moves: doors, the drawbridge, the minecart, the
                      //    crystals. It writes the position VECTOR directly
                      //    (rec+0x14/+0x18/+0x1C), the state word at rec+0x08 (43
                      //    sites, the busiest field in the function) and the
                      //    timers at rec+0x24 and rec+0x40. Ungated, all of it ran
                      //    once per rendered frame -- the census measured rec+0x18,
                      //    rec+0x24 and rec+0x40 at ratio 3.8-4.05, 13/s against
                      //    50/s, standing still in area 1 at 144 fps.
                      //
                      //    It reaches DrawOTag, and is gated anyway: the only edge
                      //    out is func_80037B5C, the transition fade, which renders
                      //    its own frames by calling stage 13. The main loop still
                      //    runs stage 13 afterwards, so this costs the *entry* to a
                      //    fade or a cutscene up to one tick of delay, never a
                      //    frame's picture. Recorded in check_gate.py's KNOWN.
        0x8002A550,   // 3  pad read, turn, walk, the death counter at 0x8019951A,
                      //    the poison tick, the buff timers, the frame counter
        0x80040348,   // 4  the 200-record entity table at 0x8016C544
        0x80046A60,   // 5  128 effect/projectile lifetimes at 0x8019CC6C
        0x8004910C,   // 6  the area module's own per-frame entry, dispatched
                      //    through *(u32*)(*(u32*)0x8017E068 + 4) -- slot 1 of the
                      //    module header. Six of the nine modules leave it an
                      //    empty `jr $ra`; fdat11, fdat14 and fdat20 use it for
                      //    proximity and trigger logic that writes state bytes.
                      //    No SDK entry point in any of the nine subtrees.
        0x80033FBC,   // 13 the fade state machine, called from stage 13 -- state
                      //    byte 0x80192D42, brightness 0x80192D44 stepping +0x14
                      //    in and -0x14 out, hold counter 0x80192D43 counting
                      //    down. Three functions in its subtree, none of which
                      //    can draw, so it is safe to skip even though its caller
                      //    is the renderer. Ungated, an area fade is four times
                      //    quicker at 120 fps than on hardware.
        0x8002DC78,   // 13 the animated-texture updater, also called from stage 13
                      //    (at 0x800346..). It walks 8 slots at 0x80192D58 (stride
                      //    0x18): each advances a scroll phase at rec+0x4 by the
                      //    per-slot rate at rec+0x3, wrapping at rec+0xC, then
                      //    re-uploads the scrolled texture region to VRAM through
                      //    func_80060624 (a LoadImage-style GPU transfer). This is
                      //    the animated water, the main-hall fire and the creatures'
                      //    scrolling skins. Ungated it advanced once per rendered
                      //    frame, so the scroll ran at the render rate -- measured
                      //    rec+0x4 changing on 100% of frames at 120 fps, i.e. six
                      //    times too fast against the 20 Hz world. Its subtree is
                      //    the VRAM upload alone -- no DrawOTag/VSync/PutDispEnv/
                      //    PutDrawEnv -- so skipping it on a non-tick frame is safe:
                      //    the phase holds and VRAM keeps the last tick's frame, so
                      //    the texture animates at the tick rate.
    ];

    /// <summary>True once KF2_FPS has spoken, so the saved rate does not overrule it.</summary>
    static bool _fromEnv;

    static readonly Stopwatch _clock = Stopwatch.StartNew();

    // When the current frame is allowed to end. Absolute rather than now+min, so a
    // frame that overruns is paid for out of the next one and the rate averages to
    // exactly the target instead of drifting down by the accumulated jitter.
    static double _due;
    static long _frames;

    /// <summary>VSync calls since the last frame boundary. Reset there, and the
    /// only thing that decides where a frame ends -- see <see cref="AfterDrawOTag"/>.</summary>
    static int _vsyncCalls;

    /// <summary>
    /// Whether a frame boundary has to have seen a <c>VSync</c> call.
    ///
    /// True normally, and that is the whole point of the boundary -- see
    /// <see cref="AfterDrawOTag"/>. **Cleared when no VSync thunk could be hooked**,
    /// because the alternative is not "a slightly worse boundary", it is *no
    /// boundary at all*: nothing paced, and <see cref="_tickThisFrame"/> frozen at
    /// its initial <c>true</c>, so every gated stage runs on every frame and the
    /// world advances at the render rate. One ordering table per frame is this
    /// game's own arrangement, so charging every <c>DrawOTag</c> as a frame is
    /// right here and merely coarse elsewhere. Degrading to coarse beats degrading
    /// to nothing.
    /// </summary>
    static bool _boundaryNeedsVSync = true;

    /// <summary>When the last frame boundary was reached, on <see cref="_clock"/>.
    /// Negative until the first one. The watchdog in <see cref="BeforeStage"/> is
    /// what reads it.</summary>
    static double _lastBoundaryMs = -1.0;

    /// <summary>Frames a second over the last window, so the settings can show whether
    /// the chosen rate is the one being achieved. Zero until the first window ends.</summary>
    public static double Measured { get; private set; }

    static double _windowStart;
    static long _windowFrames;

    /// <summary>
    /// `KF2_FPS_PROBE=1`: one line a second saying what the port is actually
    /// doing -- the rate achieved against the rate asked for, the world's tick
    /// rate against the ticks it actually took, and whether a frame can land
    /// between two ticks at all.
    ///
    /// It exists because every other probe in the port hangs off a *game*
    /// function, so none of them says anything at the title screen -- and the
    /// title screen is where a session that is going to have no smoothing in it
    /// announces itself, by running at the wrong speed. This one is driven by the
    /// frame boundary, which is reached in all three executables, so it reports
    /// from the first frame of the boot.
    /// </summary>
    static bool _probe;
    static long _windowTicks;
    static long _probeVsyncMark;

    // ---- the sentinel ---------------------------------------------------------

    /// <summary>
    /// Presents a second, counted inside <c>LibEtc.VSync</c> rather than by any hook
    /// here, against what this class's own hooks say happened -- because some boots
    /// run the whole session at exactly twice the asked-for rate (240 asked, 480 on
    /// the menu bar, title and areas alike, world speed normal), and 2T is precisely
    /// <see cref="ApplyHostCeiling"/>'s ceiling: nothing of this class is holding the
    /// picture. The title-menu loop has no wait of its own, one VSync a picture, so
    /// it can only read 2T if <see cref="Floor"/> is not running there. Which of the
    /// ways that can be true is not decidable from the source, and the boot is
    /// intermittent, so this watches every session and speaks once when it happens.
    /// Driven from <c>VSyncEvent</c>, which fires from inside <c>LibEtc.VSync</c>
    /// whether or not a detour does. See "The smoothing is sometimes dead for a whole
    /// session" in docs/TODO.md.
    /// </summary>
    static double _sentinelStartMs = -1.0;
    static long _sVsyncMark, _sAutoMark;
    static long _winVsyncPre, _winOtPost, _winBoundaries;
    static long _winFloorCalls, _winFloorWaits, _winFallbackFresh, _winFallbackHeld, _winFallbackTicks;
    static double _winFloorSleptMs;
    static int _sentinelHigh;
    static bool _sentinelSaid;

    // ---- the logic clock ------------------------------------------------------

    // Unspent logic ticks, in ticks. Advanced by wall-clock time at the frame
    // boundary rather than by 1/TargetFps per frame, so a host that misses the
    // target still runs the world at LogicHz instead of at whatever it achieved.
    static double _logicCredit;
    static double _logicClockMs = -1.0;

    /// <summary>Whether the gated stages run on the frame now being built. Decided
    /// at the previous frame's boundary, because that is the last moment before
    /// the stages run.
    ///
    /// **It starts <c>true</c> and is only ever written at the boundary, so a
    /// boundary that stops arriving freezes it there** -- every gated stage then
    /// runs on every frame and the world advances at the render rate. That is the
    /// single failure that reads as "the whole game runs fast", and it used to be
    /// silent in both halves: <see cref="Floor"/> has its only call site at the
    /// boundary too, so the picture went uncapped in the same instant. The
    /// watchdog in <see cref="BeforeStage"/> is what refuses to let that stand.
    /// </summary>
    static bool _tickThisFrame = true;

    // ---- pause ----------------------------------------------------------------

    /// <summary>
    /// What is asking for the world to stand still, if anything.
    ///
    /// **Pause is the stage gate held shut rather than a new mechanism**, which is
    /// what makes it cheap and what bounds what it can break: the six gated stages
    /// are already the whole of the per-tick world -- the object state machine, the
    /// pad read and player movement, the entity table, the effect lifetimes, the
    /// area module's own logic, the fade and the texture scroll -- and
    /// <see cref="BeforeStage"/> already declines to run them on a frame the logic
    /// clock did not tick. A pause is that decision made for every frame instead of
    /// for three in four. Nothing is written to game memory and nothing is
    /// restored, so a pause that ends leaves the game exactly where the last tick
    /// put it.
    ///
    /// **Stage 13 is not gated, so the picture keeps being drawn** -- which is the
    /// property that lets an overlay be laid over a *live* frame rather than over
    /// a frozen buffer that the port would have to keep re-presenting itself. The
    /// world in it simply does not move.
    ///
    /// It is a predicate rather than a flag because the thing that pauses is a
    /// panel whose open state is owned elsewhere and can be closed by more than
    /// one route -- the key, the pad button, and the Gameplay switch that turns
    /// the map off. A latch set by whichever of those opened it would have to be
    /// cleared by all of them, and a
    /// latch left set is a game that never resumes. Asking the panel is the state.
    /// </summary>
    static Func<bool>? _pauseWhen;

    /// <summary>
    /// Ask for the world to stand still while <paramref name="predicate"/> is
    /// true. Several callers OR together, so a second pause request does not
    /// silently take the first one's place.
    /// </summary>
    public static void PauseWhen(Func<bool> predicate)
    {
        var prev = _pauseWhen;
        _pauseWhen = prev == null ? predicate : () => prev() || predicate();
    }

    /// <summary>
    /// Whether the world is standing still on the frame now being built.
    ///
    /// **Latched once a frame rather than read per call**, for the reason
    /// <see cref="_tickThisFrame"/> is: host input is polled from inside the
    /// game's own <c>VSync</c> (patches/recompone/0007), so the key that opens the
    /// map can arrive between two stages of one main-loop iteration, and a
    /// predicate read per stage would then run half a tick -- a state machine
    /// stepped against an entity table that was not. The latch is taken at the two
    /// places that already mean "a new frame": the frame boundary, and
    /// <see cref="FallbackTick"/> when the boundary has been lost.
    /// </summary>
    public static bool Paused => _paused;

    static bool _paused;

    /// <summary>Read the pause predicate for the frame about to run. Cheap enough
    /// to be unconditional -- one delegate call and two bool reads, once a
    /// frame.</summary>
    static void LatchPause() => _paused = _pauseWhen != null && _pauseWhen();

    // ---- the watchdog ---------------------------------------------------------

    /// <summary>
    /// How long the stage gate will keep believing a boundary it has stopped being
    /// told about, in ms. Comfortably past the 250 ms the logic clock already
    /// treats as "the game stopped drawing" and past any real disc read, since a
    /// blocked game is not calling gated stages either -- a stage running while no
    /// boundary arrives is the broken case and nothing else.
    /// </summary>
    const double BoundaryDeadMs = 500.0;

    /// <summary>
    /// How long one decision of <see cref="FallbackTick"/> is held. The gated
    /// stages of one main-loop iteration run microseconds apart and must all get
    /// the same answer, or the world half-ticks; a quarter of a tick bounds the
    /// error if two iterations ever do land inside one window. Not a rate.
    /// </summary>
    static double FallbackHoldMs => Math.Min(1000.0 / LogicHz * 0.25, 3.0);

    static double _fallbackNextMs = -1.0, _fallbackHoldUntilMs = -1.0;
    static bool _fallbackTick, _fallbackSaid;
    static long _fallbackVsyncMark = -1;

    /// <summary>
    /// How far the frame being drawn is past the last logic tick, in ticks, in
    /// [0,1). This is what <see cref="FrameSmoothing"/> extrapolates the view by;
    /// it is continuous across a tick boundary, so the camera does not jump on the
    /// frames where the world did advance.
    /// </summary>
    public static double LogicPhase => Paused || !Gating ? 0.0 : Math.Clamp(_logicCredit, 0.0, 1.0);

    /// <summary>
    /// Whether the gated stages ran on the frame now being drawn -- that is,
    /// whether the world advanced since the last one. Decided at the previous
    /// frame's boundary like the phase, so it is stable for the whole frame and
    /// can be read from anywhere in it.
    ///
    /// <see cref="ObjectSmoothing"/> is what wants it: to carry a moving object
    /// between ticks you need last tick's position as well as this one's, and the
    /// only moment worth re-sampling is a frame the world actually moved on.
    /// </summary>
    public static bool TickedThisFrame => !Paused && (!Gating || _tickThisFrame);

    /// <summary>
    /// Whether the gated stages will run in this main-loop iteration: what
    /// <see cref="BeforeStage"/> returns, asked with no side effect, for an ungated
    /// stage that runs before them. With the boundary lost the answer is only made
    /// inside <see cref="FallbackTick"/>, so this says true there -- the game's own
    /// every-frame behaviour, in a state that already announces itself.
    /// </summary>
    public static bool StagesWillRun
    {
        get
        {
            if (!Gating) return !Paused;
            bool live = _lastBoundaryMs >= 0.0 &&
                        _clock.Elapsed.TotalMilliseconds - _lastBoundaryMs <= BoundaryDeadMs;
            return !live || (_tickThisFrame && !Paused);
        }
    }

    /// <summary>
    /// How many frame boundaries have been reached. Not a rate and not a clock --
    /// an *identity*, so a patch that must act once a frame can tell a second call
    /// inside one frame from the first call of the next. <see cref="SpriteAnim"/>
    /// is what wants it: a modal loop's redraw and the transition fade both reach
    /// stage 13's world walk again, and <see cref="TickedThisFrame"/> alone cannot
    /// separate those from the frame's own walk.
    /// </summary>
    public static long Frames => _frames;

    /// <summary>
    /// True on the **first stage-13 walk of a frame the world ticked on**, which
    /// is the test a patch wants when it re-samples once a tick rather than once
    /// per walk. <paramref name="lastFrame"/> is the caller's own memory of which
    /// frame it last answered true for; seed it at -1.
    ///
    /// <see cref="TickedThisFrame"/> alone cannot say this. It is decided at the
    /// frame boundary and is stable for the whole frame, so a *second* walk
    /// inside one frame -- a <see cref="LoopPacing"/> redraw, or the transition
    /// fade rendering its own frames from inside stage 2 -- reads it true again.
    /// <see cref="Frames"/> is the identity that separates them, and a patch
    /// re-sampling on the second walk destroys the tick's real prev/cur pair:
    /// both samples are the same instant, so every slot reads as standing still
    /// and drops back to the tick rate for the rest of the tick, on exactly the
    /// transitions where it shows most.
    ///
    /// **The identity fails closed**, which is why the watchdog is in here rather
    /// than in each caller: with the boundary lost, <see cref="Frames"/> freezes
    /// and no walk is ever the first of a new frame again, so a sampler would
    /// carry forever off one stale pair. Past <see cref="BoundaryDeadMs"/> --
    /// the same window <see cref="BeforeStage"/> falls back on -- the identity is
    /// dropped and <see cref="TickedThisFrame"/> decides alone, which is the
    /// behaviour the callers had before the guard.
    /// </summary>
    public static bool FirstWalkOfTick(ref long lastFrame)
    {
        if (!TickedThisFrame) return false;

        bool live = _lastBoundaryMs >= 0.0 &&
                    _clock.Elapsed.TotalMilliseconds - _lastBoundaryMs <= BoundaryDeadMs;
        if (live && _frames == lastFrame) return false;

        lastFrame = _frames;
        return true;
    }

    /// <summary>
    /// True when the world is on the logic clock rather than on the loop.
    ///
    /// **This used to require a rate above 30**, because below that the game's own
    /// frame gate did the job. The gate is now skipped at every rate, so nothing
    /// else is left to hold the world down and the accumulator has to run in every
    /// configuration -- including at the tick rate, where it simply ticks on every
    /// frame, and uncapped, where it is the only thing keeping the game playable.
    /// </summary>
    public static bool Gating => LogicMode == Logic.Fixed;

    /// <summary>
    /// True when a frame can land part-way between two logic ticks, which is the
    /// only condition under which <see cref="FrameSmoothing"/> has anything to
    /// carry. <see cref="Gating"/> is no longer that test -- it is true at the tick
    /// rate too, where <see cref="LogicPhase"/> is always ~0.
    /// </summary>
    public static bool Extrapolating => Gating && (!Enabled || TargetFps > LogicHz + 0.001);

    /// <summary>
    /// True when the game's own two-vblank wait is being skipped, which is always.
    /// It is kept as a name rather than inlined because it is the single decision
    /// the whole rate scheme rests on: the game's gate asks for 30 fps and 30 ticks
    /// together, and the port wants those to be two different numbers.
    /// </summary>
    static bool SkipFrameGate => true;

    // HookManager attributes hooks to a mod so they can be removed again. This is
    // in-project rather than a loaded package, so it declares its own identity.
    static readonly ModInfo _self = new()
    {
        Id = "kf2.framepacing",
        Name = "Frame pacing",
        Version = "2.0",
        Description = "Draws at the chosen rate and ticks the world at its own.",
    };

    public static void Configure(string? fps, string? gate, string? logic = null,
                                 string? tickRate = null, string? probe = null)
    {
        _probe = probe == "1";

        if (!string.IsNullOrWhiteSpace(tickRate))
        {
            if (!double.TryParse(tickRate, NumberStyles.Float, CultureInfo.InvariantCulture,
                                 out double hz))
                throw new ArgumentException($"KF2_TICKRATE: cannot read '{tickRate}'");

            // Nothing else writes LogicHz after this: there is no saved tick rate
            // to overrule, so KF2_TICKRATE needs no latch to defend itself with.
            LogicHz = ClampLogic(hz);
        }

        if (!string.IsNullOrWhiteSpace(fps))
        {
            if (string.Equals(fps, "off", StringComparison.OrdinalIgnoreCase)) TargetFps = 0.0;
            else if (double.TryParse(fps, NumberStyles.Float, CultureInfo.InvariantCulture, out double rate))
                TargetFps = Clamp(rate);
            else throw new ArgumentException($"KF2_FPS: cannot read '{fps}'");

            _fromEnv = true;
        }

        if (!string.IsNullOrWhiteSpace(gate))
            foreach (var a in gate.Split('+', StringSplitOptions.RemoveEmptyEntries))
                _gated.Add(Convert.ToUInt32(a.Trim(), 16));

        if (!string.IsNullOrWhiteSpace(logic))
            LogicMode = string.Equals(logic, "full", StringComparison.OrdinalIgnoreCase)
                ? Logic.Full : Logic.Fixed;
    }

    /// <summary>
    /// Attach the hooks. Deferred to the first overlay load because
    /// <see cref="SymbolRegistry"/> reads the dispatcher's overlay tables, and
    /// those are registered inside Entry.Run -- after Program.cs has run, but
    /// before anything is loaded, so the first load event is the earliest moment
    /// every overlay is resolvable.
    /// </summary>
    public static void Install()
    {
        // The saved rate can only be read once ConfigManager has loaded, which
        // happens inside HostWindow.Initialize -- after Program.cs called Configure.
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            // Order matters: the tick rate is read first, because SetTargetFps
            // resets the accumulator. The tick rate is no longer read from the
            // config: it is 20 unless KF2_TICKRATE says otherwise, and Configure
            // has already applied that. The render rate defaults to DefaultFps,
            // not the tick rate, so a first run draws three pictures per tick.
            if (!_fromEnv) SetTargetFps(SavedRate());
            else ApplyHostCeiling();
        });

        // Attached whether or not the rate is the default: a rate is a choice that
        // can be taken back, and hooks cannot be added once the game is running
        // past the overlay loads.
        //
        // **Retried on a later overlay load if it did not finish**, and that is the
        // point rather than caution. Event.Dispatch wraps every listener in a
        // try/catch that writes one line to stderr, so a throw anywhere in Attach
        // -- HookManager.Commit's foreach aborts on the first detour that will not
        // install, and takes every function after it with it -- used to leave the
        // session unpaced with the latch already set, so it was never tried again.
        // Attach only claims what it actually hooked, so a retry adds the rest
        // rather than a second copy of the lot.
        // The sentinel listens on the vblank, not on any hook of its own; see _sentinelStartMs.
        Event.AddListener<VSyncEvent>(_ => Sentinel());

        HookAttach.OnOverlayLoad("pacing", Attach,
                                 "See \"Any frame rate\" in docs/PATCHES_AND_MODS.md.");
    }

    // What is hooked already, so a retry after a partial attach adds only what is
    // missing. Overlay names for the two per-overlay roles; counts for the rest.
    static readonly HashSet<string> _otHooked = new(StringComparer.Ordinal);
    static readonly HashSet<string> _vsHooked = new(StringComparer.Ordinal);
    static readonly HashSet<uint> _stageHooked = [];
    static bool _gateHooked, _fullRateHooked;

    /// <summary>
    /// The rate a previous run left behind. Migrates the vblank divisor this used
    /// to be saved as, once, so an existing config keeps the rate it had: n
    /// vblanks was 60/n fps, and 0 was uncapped.
    /// </summary>
    static double SavedRate()
    {
        var view = RecompOne.Runtime.Runtime.View;
        if (view.Values.ContainsKey(FpsKey)) return view.GetFloat(FpsKey, (float)TargetFps);

        if (!view.Values.ContainsKey(VBlankKey)) return TargetFps;

        int vblanks = view.GetInt(VBlankKey, 2);
        double rate = vblanks <= 0 ? 0.0 : Clamp(60.0 / Math.Clamp(vblanks, 1, 4));
        view.SetFloat(FpsKey, (float)rate);
        return rate;
    }

    /// <summary>
    /// Change the rate at run time. 0 is uncapped, otherwise frames a second.
    /// Safe at any moment -- every reader of this state is per frame.
    /// </summary>
    public static void SetTargetFps(double fps)
    {
        TargetFps = fps > 0.0 ? Clamp(fps) : 0.0;
        _due = _clock.Elapsed.TotalMilliseconds;
        _logicClockMs = -1.0;
        _logicCredit = 0.0;
        _tickThisFrame = true;
        ApplyHostCeiling();
    }

    /// <summary>
    /// Change the tick rate at run time. Safe at any moment -- every reader of this
    /// state is per frame -- and it resets the accumulator rather than rescaling
    /// the credit, so the change lands on the next tick instead of part-way
    /// through one.
    /// </summary>
    public static void SetLogicHz(double hz)
    {
        LogicHz = ClampLogic(hz);
        _logicClockMs = -1.0;
        _logicCredit = 0.0;
        _tickThisFrame = true;
    }

    /// <summary>The game's own achievable rates are 60/n, so 60, 30, 20, 15, 12.
    /// The range is that band set with room either side; a tick rate above the
    /// vblank grid is not a thing the game was ever asked to do.</summary>
    static double ClampLogic(double hz) => Math.Clamp(hz, 5.0, 60.0);

    /// <summary>Rates below this are the game's own slow bands and are allowed;
    /// above it is a host frame rate. The ceiling is arbitrary and only exists so
    /// that a typed-in number cannot ask for a zero-length frame.</summary>
    static double Clamp(double fps) => Math.Clamp(fps, 5.0, 1000.0);

    /// <summary>
    /// Hand RecompOne's own throttle a permissive ceiling rather than the target.
    /// It paces per <c>VSync</c> *call* and a frame can carry more than one, so it
    /// cannot express a frame rate; its job here is only to stop a loop that has
    /// stopped drawing -- a disc read, a menu -- from spinning. Never lowered below
    /// 60, which is what it was before patches/recompone/0025 made it settable.
    ///
    /// **Uncapped leaves it at 60 rather than turning it off.** "Uncapped" has
    /// always meant "the port's own floor is off", and what that produced was a
    /// port topping out around 50 fps because FrameClock held each VSync call to a
    /// vblank. Setting 0 here would mean something new -- present as fast as the
    /// host can, with nothing between the loop and the GPU -- which is not the
    /// diagnostic anyone asked for. The capability exists in 0025; nothing uses it.
    /// </summary>
    static void ApplyHostCeiling()
        => RecompOne.Runtime.Runtime.TargetFps = Enabled ? Math.Max(60.0, TargetFps * 2.0) : 60.0;

    /// <summary>The vblank floor this used to be expressed as, for the settings and
    /// for anything that still thinks in bands. 0 is uncapped.</summary>
    public static int Cap => Enabled ? Math.Max(1, (int)Math.Round(60.0 / TargetFps)) : 0;

    /// <summary>The form the old settings page took. Kept so a caller that thinks
    /// in vblanks still works: 1 is 60 fps, 2 is 30, 4 is 15, 0 is uncapped.</summary>
    public static void SetCap(int vblanks)
        => SetTargetFps(vblanks <= 0 ? 0.0 : 60.0 / Math.Clamp(vblanks, 1, 4));

    /// <summary>
    /// Attach the hooks, and say what actually landed. Returns true only when every
    /// role this build knows about is hooked; anything less is reported and left to
    /// be retried on the next overlay load, since the alternative is a session that
    /// paces nothing and never says so.
    /// </summary>
    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(FramePacing);
        var frameDrawn = self.GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;
        var stageGate = self.GetMethod(nameof(BeforeStage), BindingFlags.Public | BindingFlags.Static)!;
        var frameGate = self.GetMethod(nameof(BeforeFrameGate), BindingFlags.Public | BindingFlags.Static)!;
        var vsyncCall = self.GetMethod(nameof(BeforeVSync), BindingFlags.Public | BindingFlags.Static)!;

        // Registered here, *claimed* after the commit below: an Add* only appends a
        // delegate and returns true, so filling the sets from it claims what was
        // queued. A boundary reported 3/3 while MonoMod refused the detour is a
        // silent uncapped session, and the latch would have stopped the retry that
        // Commit's own null-Hook check would have fixed.
        List<(string Overlay, MethodInfo Target)> ot = [], vs = [];
        List<(uint Addr, MethodInfo Target)> stages = [];
        MethodInfo? gateTarget = null;

        foreach (var (overlay, addr) in DrawOTag)
        {
            if (_otHooked.Contains(overlay)) continue;
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] pacing: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPost(_self, target, frameDrawn)) ot.Add((overlay, target));
        }

        // Without this the frame boundary never fires and nothing here paces
        // anything, so say so rather than run silently at whatever the host does.
        foreach (var (overlay, addr) in VSyncThunk)
        {
            if (_vsHooked.Contains(overlay)) continue;
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] pacing: no VSync at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPre(_self, target, vsyncCall)) vs.Add((overlay, target));
        }

        // The game's own frame gate, skipped at every rate. Only GAME.EXE's copy is
        // hooked, so the title and the ending pace themselves as they always did --
        // they are CD-bound at 7-15 fps and have no world to tick.
        if (!_gateHooked)
        {
            var target = SymbolRegistry.Resolve("game", null, FrameGate);
            if (target == null)
                Console.Error.WriteLine($"[KF2] pacing: no game function at 0x{FrameGate:X8} -- " +
                                        "the game's own two-vblank wait cannot be removed, so the " +
                                        "port will draw and tick at 30 whatever was asked for.");
            else if (HookManager.AddPre(_self, target, frameGate)) gateTarget = target;
        }

        // The stage gate is hooked even when the render rate equals the tick rate,
        // where BeforeStage always lets the original run, so that a higher rate can
        // be chosen later. KF2_FPS_GATE
        // replaces the default set rather than adding to it, and can name any
        // address in `game`, not just the thirteen main-loop stages -- experimenting
        // is the point.
        var gate = _gated.Count > 0 ? _gated : (IReadOnlyList<uint>)DefaultGate;
        foreach (uint addr in gate)
        {
            if (_stageHooked.Contains(addr)) continue;
            var target = SymbolRegistry.Resolve("game", null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] pacing: no game function at 0x{addr:X8}, not gated");
                continue;
            }
            if (HookManager.AddPre(_self, target, stageGate)) stages.Add((addr, target));
        }

        if (!_fullRateHooked && FullRateLogic.Attach(_self) > 0) _fullRateHooked = true;

        // Anything after this line runs only if the detours actually installed. It
        // is the one step here that is not bookkeeping, and it is where a partial
        // attach comes from.
        HookManager.Commit();

        // What the detours say, not what the dictionary says. A pre and a post on
        // one function share one detour, so a role is one question per function.
        foreach (var (overlay, target) in ot)
            if (HookAttach.Installed(target)) _otHooked.Add(overlay);
            else Console.Error.WriteLine($"[KF2] pacing: DrawOTag on {overlay} queued but not installed");
        foreach (var (overlay, target) in vs)
            if (HookAttach.Installed(target)) _vsHooked.Add(overlay);
            else Console.Error.WriteLine($"[KF2] pacing: VSync on {overlay} queued but not installed");
        foreach (var (addr, target) in stages)
            if (HookAttach.Installed(target)) _stageHooked.Add(addr);
            else Console.Error.WriteLine($"[KF2] pacing: stage 0x{addr:X8} queued but not installed");
        if (gateTarget != null && HookAttach.Installed(gateTarget)) _gateHooked = true;

        int n = _otHooked.Count + _vsHooked.Count + _stageHooked.Count
              + (_gateHooked ? 1 : 0) + (_fullRateHooked ? 1 : 0);

        // The boundary is the load-bearing pair, so it is stated as a pair rather
        // than folded into the total: reading "15 hook(s)" and counting on your
        // fingers is how this went unnoticed.
        Console.WriteLine($"[KF2] pacing: {Describe()}, {n} hook(s), " +
                          $"boundary {_otHooked.Count}/{DrawOTag.Length} DrawOTag + " +
                          $"{_vsHooked.Count}/{VSyncThunk.Length} VSync, " +
                          $"{_stageHooked.Count}/{gate.Count} stage(s), " +
                          $"frame gate {(_gateHooked ? "skipped" : "IN PLACE")}, " +
                          $"world {(LogicMode == Logic.Full ? "at the render rate" : $"at {LogicHz:0.#} Hz")} " +
                          $"gating {string.Join(" ", gate.Select(g => g.ToString("X8")))}");

        // No VSync thunk means the boundary can never be reached as defined, which
        // is not a degraded pacer but no pacer at all -- and the world then runs at
        // the render rate with nothing said. One ordering table per frame is this
        // game's own arrangement, so charge every DrawOTag instead.
        // Recomputed every pass rather than only ever cleared: Attach is built to be
        // retried, so a first pass that got DrawOTag but no VSync thunk and a second
        // that got all three would otherwise stay permanently in the degraded mode
        // while the summary line above reported 3/3 VSync.
        _boundaryNeedsVSync = _vsHooked.Count > 0 || _otHooked.Count == 0;

        if (!_boundaryNeedsVSync && _otHooked.Count > 0)
        {
            Console.Error.WriteLine("[KF2] pacing: no VSync thunk could be hooked -- charging every " +
                                    "DrawOTag as a frame instead. A frame carrying two ordering " +
                                    "tables is then paced twice. See \"Any frame rate\" in " +
                                    "docs/PATCHES_AND_MODS.md.");
        }

        if (_otHooked.Count == 0)
            Console.Error.WriteLine("[KF2] pacing: no DrawOTag could be hooked, so there is no frame " +
                                    "boundary at all -- the picture is unpaced and the world is held " +
                                    $"to {LogicHz:0.#} Hz by the stage gate's watchdog alone.");

        if (_stageHooked.Count == 0 && LogicMode == Logic.Fixed)
            Console.Error.WriteLine("[KF2] pacing: no stage could be gated -- the world will advance " +
                                    "once per rendered frame instead of at the tick rate. See " +
                                    "\"Frame pacing\" in docs/PATCHES_AND_MODS.md.");

        if (!_gateHooked)
            Console.Error.WriteLine("[KF2] pacing: the game's own frame gate is still in place, so both " +
                                    "the picture and the world will sit at 30 whatever was asked for.");

        bool complete = _otHooked.Count == DrawOTag.Length
                     && _vsHooked.Count == VSyncThunk.Length
                     && _stageHooked.Count == gate.Count
                     && _gateHooked;

        if (!complete)
            Console.Error.WriteLine("[KF2] pacing: attach incomplete -- will try the rest on the next " +
                                    "overlay load.");

        return complete;
    }

    static string Describe() => Enabled ? $"{TargetFps:0.#} fps" : "uncapped";

    /// <summary>
    /// The game's own frame gate, skipped at every rate. The credit is zeroed here
    /// because the function that would have zeroed it did not run.
    ///
    /// **It used to be left alone at 30 and below**, on the reasoning that there
    /// the game paced itself exactly as it does on hardware and the port added
    /// nothing. That stopped being true when the tick rate became a number of its
    /// own: the gate does not just pace, it *also* decides how often the world
    /// advances, and it only knows one answer for both -- 30. Letting it run would
    /// pin the world back to 30 Hz at any render rate at or below 30, silently
    /// making <see cref="LogicHz"/> a lie exactly at the default.
    ///
    /// What is given up with it is the port's most-tested configuration, where
    /// none of this class did anything. <see cref="Floor"/> is now the pacer at
    /// every rate, and 0x801B6CAC and the vblank callback func_80017850 are
    /// untouched -- only the spin is skipped, so the CD timeout riding on that
    /// counter is unaffected.
    /// </summary>
    public static bool BeforeFrameGate(CpuContext c, IMemory m)
    {
        // Stage 13 is the only caller, so reaching here is the one signal that the
        // frame now being finished is a picture of the world rather than of the
        // interface. LoopPacing wants that and would otherwise need a hook of its
        // own; taking it here costs nothing.
        LoopPacing.WorldDrawn();

        if (!SkipFrameGate) return true;
        m.WriteU32(VBlankCredit, 0u);
        return false;
    }

    /// <summary>
    /// Counts the game asking to present. The frame boundary is defined off this
    /// rather than off the vblank; see <see cref="AfterDrawOTag"/>.
    /// </summary>
    public static void BeforeVSync(CpuContext c, IMemory m)
    {
        _vsyncCalls++;
        _winVsyncPre++;
    }

    /// <summary>
    /// The frame boundary: **the ordering table drawn after the game asked to
    /// present**. A second OT with no VSync call between it and the first belongs
    /// to the frame already in flight -- King's Field draws one OT per frame, but
    /// this keeps anything that draws two from being charged twice.
    ///
    /// **This used to be keyed on the vblank instead, and that was the bug that
    /// made every rate above 30 run the game fast.** Since
    /// patches/recompone/0021 the emulated vblank is a wall-clock 60 Hz grid, so
    /// once the port draws faster than 60 most frames reach here with no vblank
    /// elapsed. They were discarded: no <see cref="Floor"/>, no
    /// <see cref="AdvanceLogicClock"/> -- which left <c>_tickThisFrame</c> at the
    /// previous frame's value, so the gated stages ran again and the world ticked
    /// at 30 x (frames per vblank). Measured at KF2_FPS=60: 120 fps drawn, the
    /// 65-tick death clock at 0x8019951A finishing in 1.10 s instead of 2.17.
    /// A VSync call is once per rendered frame at any rate, which the vblank is
    /// not, so it is the boundary. At 30 nothing changes: the presenter's
    /// VSync(0) runs just before this, and the frame gate's spin calls land after
    /// it, so the count is non-zero exactly once per OT either way.
    /// </summary>
    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _winOtPost++;
        if (_boundaryNeedsVSync && _vsyncCalls == 0) return;
        _vsyncCalls = 0;
        _frames++;
        _winBoundaries++;

        double now = _clock.Elapsed.TotalMilliseconds;
        _lastBoundaryMs = now;

        _windowFrames++;
        double elapsed = now - _windowStart;
        if (elapsed >= 1000.0)
        {
            Measured = _windowFrames * 1000.0 / elapsed;

            long presents = LibEtc.VSyncCalls - _probeVsyncMark;
            _probeVsyncMark = LibEtc.VSyncCalls;

            if (_probe)
                // The three health words are the half of this line that reports a
                // *reading*. Everything before them -- including `Extrapolating`,
                // which is a pure function of LogicMode, Enabled, TargetFps and
                // LogicHz -- is configuration, and configuration is identical in a
                // session whose smoothing is dead and one whose is not. That was
                // measured from play, and it is why this probe could not close the
                // question it was written for. Each patch answers for itself, so a
                // line that reads `anim carrying` beside `view unprimed` names both
                // the layer and the two patches to look at, in one string, without
                // KF2_SMOOTH_PROBE having been switched on beforehand.
                Console.WriteLine($"[KF2] pacing: {Measured:0.0} fps drawn of {Describe()}, " +
                                  $"{presents * 1000.0 / elapsed:0.0} present(s)/s, " +
                                  $"{_windowTicks * 1000.0 / elapsed:0.0} tick(s)/s of {LogicHz:0.#} Hz, " +
                                  $"{(Extrapolating ? "smoothing can carry" : "nothing to carry at this rate")}; " +
                                  $"view {FrameSmoothing.TakeHealth()}, " +
                                  $"objects {ObjectSmoothing.TakeHealth()}, " +
                                  $"anim {AnimSmoothing.TakeHealth()}");

            _windowFrames = 0;
            _windowTicks = 0;
            _windowStart = now;
        }

        AdvanceLogicClock(now);
        if (_tickThisFrame) _windowTicks++;

        // How long this frame is allowed to be is no longer the render rate alone:
        // a frame the main loop did not produce belongs to a loop of the game's own
        // that steps its state once per iteration, and LoopPacing holds those to the
        // world's clock. It returns 1000/TargetFps for an ordinary frame, and 0 when
        // nothing should wait, so Floor runs in the uncapped configuration too --
        // which is correct, since the world is held to LogicHz there as well.
        double min = LoopPacing.FrameMinMs(Enabled, TargetFps);
        if (min > 0.0) Floor(min);
    }

    /// <summary>
    /// Decide whether the next frame's gated stages run. Advanced by wall-clock
    /// time rather than by the nominal rate, so a host that cannot hit the target
    /// still runs the world at <see cref="LogicHz"/> instead of at whatever it
    /// managed -- which is the difference between "the picture stutters" and "the
    /// game plays in slow motion".
    ///
    /// At the render rate *equal* to the tick rate this ticks on every frame and
    /// nothing is skipped, because <see cref="Floor"/> guarantees a frame is at
    /// least 1000/LogicHz ms long and the credit therefore always reaches 1.
    /// </summary>
    static void AdvanceLogicClock(double nowMs)
    {
        LatchPause();

        // Paused: the credit does not accrue and the clock is carried forward, so
        // resuming starts from now rather than paying out however many ticks the
        // map was open for. The frame boundary still arrives -- stage 13 is not
        // gated -- so this runs once a frame throughout.
        if (_paused)
        {
            _logicClockMs = nowMs;
            _logicCredit = 0.0;
            _tickThisFrame = false;
            return;
        }

        if (!Gating)
        {
            _logicCredit = 0.0;
            _logicClockMs = nowMs;
            _tickThisFrame = true;
            return;
        }

        bool first = _logicClockMs < 0.0;
        double dt = first ? 0.0 : nowMs - _logicClockMs;
        _logicClockMs = nowMs;

        // More than a quarter of a second means the game stopped drawing rather
        // than ran late -- a disc read, a module swap, the first frame of all.
        // Tick once and restart, rather than running the world forward through the
        // gap the way a naive accumulator would.
        //
        // **A dt of zero is not that case and no longer takes that branch.** It
        // used to, which granted a free tick to any two boundaries landing on the
        // same reading of the clock -- and LoopPacing's fill issues boundaries
        // back to back with almost no work between them, its own exit condition
        // being TickedThisFrame. Restarting is for the first sample, which `first`
        // now says outright; no elapsed time means no credit, and no tick.
        if (first || dt > 250.0) _logicCredit = 1.0;
        else if (dt > 0.0) _logicCredit = Math.Min(_logicCredit + dt * LogicHz / 1000.0, 2.0);

        _tickThisFrame = _logicCredit >= 1.0;
        if (_tickThisFrame) _logicCredit -= 1.0;
    }

    /// <summary>
    /// Skips a gated stage on a frame the logic clock did not tick. Returning
    /// false makes the recompiled body not run; HookManager's Invoke honours it.
    /// In <see cref="Logic.Full"/> this always runs the original, and at the tick
    /// rate the accumulator ticks on every frame, so it does there too.
    ///
    /// **The watchdog is the second half and it is not a tidy-up.** A gated stage
    /// is only ever reached from the main loop, and the main loop draws, so a
    /// stage running while no frame boundary has arrived for
    /// <see cref="BoundaryDeadMs"/> means the boundary is not being reached at all
    /// -- the <c>DrawOTag</c> post or the <c>VSync</c> pre did not take. Believing
    /// <see cref="_tickThisFrame"/> then means believing a stale <c>true</c>,
    /// which runs the world at the render rate; at the rates this port is played
    /// at that is the whole game several times too fast, and it says nothing.
    /// So the clock is run from here instead, off the same wall clock, and the
    /// port says once what it is doing.
    /// </summary>
    public static bool BeforeStage(CpuContext c, IMemory m)
    {
        if (!Gating) return !Paused;

        double now = _clock.Elapsed.TotalMilliseconds;
        if (_lastBoundaryMs >= 0.0 && now - _lastBoundaryMs <= BoundaryDeadMs)
            return _tickThisFrame && !Paused;

        // Not short-circuited on the pause: FallbackTick is also where the picture
        // is paced when there is no boundary to pace it at, and a paused game still
        // has to present frames or the overlay that paused it could never be closed.
        return FallbackTick(now) && !Paused;
    }

    /// <summary>
    /// Forgive a boundary gap that a caller can *explain*. The watchdog above
    /// cannot tell "the boundary hook is gone" from "nothing drew", and one window
    /// in the game legitimately draws nothing for seconds: the disc wait
    /// <see cref="LoadPacing"/> paces, which by its own measurement issues zero
    /// `DrawOTag` calls and which pacing stretches to 1.7 s. Called when such a
    /// window closes, this says a boundary is not overdue rather than that one
    /// arrived -- no frame is counted, no logic clock is advanced, and the credit
    /// is untouched, so the load still takes <see cref="AdvanceLogicClock"/>'s
    /// stopped-drawing branch on the next real boundary. It buys exactly one
    /// <see cref="BoundaryDeadMs"/>: a boundary that really has been lost trips the
    /// watchdog again on the frames that follow.
    /// </summary>
    public static void ExcuseBoundaryGap()
    {
        // Never *before* the first boundary -- that is the case the watchdog is
        // for, and a load happens before one at the title.
        if (_lastBoundaryMs >= 0.0) _lastBoundaryMs = _clock.Elapsed.TotalMilliseconds;
    }

    /// <summary>
    /// Hold the world to <see cref="LogicHz"/> with no frame boundary to hang it
    /// on. An absolute grid like <see cref="Floor"/>'s, so the rate averages to
    /// the target rather than drifting; more than four periods of debt means the
    /// game stopped rather than ran late, and the grid restarts instead of running
    /// the world forward through the gap.
    ///
    /// One decision is held for <see cref="FallbackHoldMs"/> because the gated
    /// stages of one main-loop iteration have to agree with each other -- half an
    /// iteration ticked is a state machine stepped against an entity table that
    /// was not.
    /// </summary>
    static bool FallbackTick(double now)
    {
        if (!_fallbackSaid)
        {
            _fallbackSaid = true;
            Console.Error.WriteLine(
                "[KF2] pacing: no frame boundary has been reached, so the render rate is not " +
                $"holding the world down -- ticking at {LogicHz:0.#} Hz and pacing the loop from " +
                "the wall clock instead. Check the boundary counts on the pacing line above; " +
                "see \"Any frame rate\" in docs/PATCHES_AND_MODS.md.");
        }

        // Held only inside one main-loop iteration. The hold used to be the whole
        // test, and an iteration shorter than it -- every one, once the port draws
        // faster than 1000/3 -- reused the previous iteration's decision: no Floor,
        // so two frames to one wait and the picture at the host ceiling (measured
        // 483 presents a second at 240, 235 fresh decisions against 3144 held), and
        // a tick decision run twice. A present in between is a new iteration; the
        // counter is LibEtc's own (0042), so it does not depend on the hook whose
        // loss put this class here.
        if (now < _fallbackHoldUntilMs && LibEtc.VSyncCalls == _fallbackVsyncMark)
        {
            _winFallbackHeld++;
            return _fallbackTick;
        }

        _fallbackVsyncMark = LibEtc.VSyncCalls;

        _winFallbackFresh++;
        LatchPause();

        // Paused, with no boundary: hold the grid rather than let it run on, so
        // resuming does not find four periods of debt and restart. Still paced,
        // for the reason BeforeStage does not short-circuit.
        if (_paused)
        {
            _fallbackNextMs = -1.0;
            _fallbackTick = false;
            _tickThisFrame = false;
            _logicCredit = 0.0;
            _fallbackHoldUntilMs = now + FallbackHoldMs;
            if (Enabled) Floor(1000.0 / TargetFps);
            return false;
        }

        double period = 1000.0 / LogicHz;
        if (_fallbackNextMs < 0.0 || now - _fallbackNextMs > 4.0 * period) _fallbackNextMs = now;

        _fallbackTick = now >= _fallbackNextMs;
        if (_fallbackTick)
        {
            _winFallbackTicks++;
            _fallbackNextMs += period;
            _logicCredit = 0.0;
        }
        else
        {
            // The phase the smoothing draws at, from the grid rather than from the
            // accumulator the boundary would have advanced.
            _logicCredit = Math.Clamp(1.0 - (_fallbackNextMs - now) / period, 0.0, 1.0);
        }

        _tickThisFrame = _fallbackTick;

        // A fresh decision means a new main-loop iteration, which without a
        // boundary is the only thing left that means "a frame". Pace it here so the
        // picture is not uncapped as well; LoopPacing's own deadline is not asked
        // for, since its flags are consumed at the boundary that is not arriving.
        if (Enabled) Floor(1000.0 / TargetFps);

        // Held from *after* the wait, or the sleep above would spend the whole
        // window and the rest of this iteration's stages would each decide again.
        _fallbackHoldUntilMs = _clock.Elapsed.TotalMilliseconds + FallbackHoldMs;

        return _fallbackTick;
    }

    /// <summary>
    /// Once a second, from the vblank: are presents running well past the asked-for
    /// rate? Two such seconds in a row print one report and the sentinel goes quiet
    /// for the session. The first high second asks <c>LibEtc.VSync</c> for the stack
    /// of its next call, so the report can show whether that call still came through
    /// <c>HookManager.Invoke</c>. The floor of 90 keeps a disc wait's 60 Hz VSyncs
    /// from reading as a fault at the low rates; the defect this is for sits at 2T.
    /// </summary>
    static void Sentinel()
    {
        double now = _clock.Elapsed.TotalMilliseconds;
        if (_sentinelStartMs < 0.0) { ResetSentinel(now); return; }

        double elapsed = now - _sentinelStartMs;
        if (elapsed < 1000.0) return;

        double k = 1000.0 / elapsed;
        long vs = LibEtc.VSyncCalls - _sVsyncMark, auto = LibGpu.AutoPresents - _sAutoMark;
        double presents = (vs + auto) * k;

        if (!_sentinelSaid && Enabled && presents > Math.Max(1.5 * TargetFps, 90.0))
        {
            if (++_sentinelHigh == 1) LibEtc.CaptureNextStack = true;
            else Report(now, k, vs, auto, presents);
        }
        else _sentinelHigh = 0;

        // The probe line is printed from the boundary, so a session that has lost
        // it says nothing at all through KF2_FPS_PROBE. This is the same line's
        // other half, from the vblank: what the watchdog is doing in its place.
        bool watchdog = _lastBoundaryMs >= 0.0 && now - _lastBoundaryMs > BoundaryDeadMs;
        if (_probe && watchdog && _winFallbackFresh > 0)
            Console.WriteLine($"[KF2] pacing: no boundary, {presents:0.0} present(s)/s of {Describe()}, " +
                              $"{_winFallbackTicks * k:0.0} tick(s)/s of {LogicHz:0.#} Hz from the watchdog " +
                              $"({_winFallbackFresh * k:0} fresh, {_winFallbackHeld * k:0} held decisions/s)");

        ResetSentinel(now);
    }

    static void ResetSentinel(double now)
    {
        _sentinelStartMs = now;
        _sVsyncMark = LibEtc.VSyncCalls;
        _sAutoMark = LibGpu.AutoPresents;
        _winVsyncPre = _winOtPost = _winBoundaries = 0;
        _winFloorCalls = _winFloorWaits = _winFallbackFresh = _winFallbackHeld = _winFallbackTicks = 0;
        _winFloorSleptMs = 0.0;
    }

    static void Report(double now, double k, long vs, long auto, double presents)
    {
        _sentinelSaid = true;

        bool watchdog = _lastBoundaryMs < 0.0 || now - _lastBoundaryMs > BoundaryDeadMs;
        double pre = _winVsyncPre * k, post = _winOtPost * k, bounds = _winBoundaries * k;
        double calls = vs * k;

        string verdict =
            pre < 0.25 * calls
                ? "VSync is being called but this class's VSync pre-hook is not running -- the detour is being bypassed"
            : post < 0.25 * calls
                ? "the VSync pre-hook runs but no DrawOTag post does -- that detour is being bypassed"
            : watchdog && _winFallbackHeld > 0
                ? "no boundary, and FallbackTick's hold is sharing one Floor between iterations"
            : _winFloorCalls > 0 && _winFloorWaits == 0
                ? "boundaries arrive and Floor is called, but never has to wait"
                : "none of the known shapes -- read the counts";

        var committed = string.Join(", ",
            DrawOTag.Select(t => $"{t.Overlay} DrawOTag {Committed(t.Overlay, t.Addr)}")
                    .Concat(VSyncThunk.Select(t => $"{t.Overlay} VSync {Committed(t.Overlay, t.Addr)}")));

        Console.Error.WriteLine(
            $"[KF2] pacing sentinel: {presents:0} present(s)/s against {Describe()} asked " +
            $"({calls:0} VSync, {auto * k:0} auto)\n" +
            $"    hooks fired: VSync pre {pre:0}/s, DrawOTag post {post:0}/s; boundaries {bounds:0}/s\n" +
            $"    Floor: {_winFloorCalls * k:0}/s called, {_winFloorWaits * k:0}/s waited, " +
            $"{_winFloorSleptMs * k:0.0} ms/s slept\n" +
            $"    fallback: {_winFallbackFresh * k:0}/s fresh, {_winFallbackHeld * k:0}/s held, " +
            $"watchdog {(watchdog ? "ACTIVE" : "idle")}\n" +
            $"    TargetFps {TargetFps:0.#}, host ceiling {RecompOne.Runtime.Runtime.TargetFps:0.#}, " +
            $"due-now {_due - now:0.00} ms, boundary needs VSync {_boundaryNeedsVSync}\n" +
            $"    committed: {committed}\n" +
            $"    reading: {verdict}\n" +
            $"    one VSync call's stack:\n{LibEtc.CapturedStack ?? "    (not captured)"}\n" +
            "    See \"The smoothing is sometimes dead for a whole session\" in docs/TODO.md.");
    }

    static string Committed(string overlay, uint addr)
    {
        try
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            return target == null ? "unresolved" : HookAttach.Installed(target) ? "yes" : "NO";
        }
        catch (Exception e)
        {
            return $"? ({e.GetType().Name})";
        }
    }

    /// <summary>
    /// Hold the frame boundary until <paramref name="min"/> ms have passed since
    /// the last one. The minimum is passed in rather than derived from
    /// <see cref="TargetFps"/> because a modal frame's is longer -- see
    /// <see cref="LoopPacing.FrameMinMs"/>.
    /// </summary>
    static void Floor(double min)
    {
        double now = _clock.Elapsed.TotalMilliseconds;

        // More than a frame of debt means the game stopped drawing rather than ran
        // late -- a disc read, a module swap, the first frame of all. Restart the
        // cadence instead of running flat out to pay it off.
        if (_due < now - min) _due = now;

        _winFloorCalls++;
        if (now < _due)
        {
            _winFloorWaits++;
            _winFloorSleptMs += _due - now;
            int profile = RecompOne.Runtime.Diagnostics.Profiler.Begin(FrameProfiler.FloorWait);
            double sleepUntil = _due - SpinMs;
            if (now < sleepUntil)
            {
                int ms = (int)(sleepUntil - now);
                if (ms > 0) Thread.Sleep(ms);
            }
            while (_clock.Elapsed.TotalMilliseconds < _due) Thread.SpinWait(48);
            RecompOne.Runtime.Diagnostics.Profiler.End(profile);
        }

        _due += min;
    }
}
