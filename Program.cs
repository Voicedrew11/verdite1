using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Hardware;
using Recompiled;

// Entry point for the King's Field (SLUS-00158) port.
//
// RecompOne generates its own Program.cs into generated/ if one is missing; this
// file takes that role instead so startup stays hand-editable. Custom init and
// patching hooks go here, before Entry.Run.

// Diagnostics. The runtime's log channels are plain static bools with no CLI of
// their own, so expose them through an env var:
//     KF2_LOG=bios,cd,gpu,dma,sdk,spu,mdec   (or KF2_LOG=all)
var channels = (Environment.GetEnvironmentVariable("KF2_LOG") ?? "")
    .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
    .Select(s => s.ToLowerInvariant())
    .ToHashSet();

if (channels.Count > 0)
{
    bool all = channels.Contains("all");
    RecompOne.Runtime.Log.BiosOn = all || channels.Contains("bios");
    RecompOne.Runtime.Log.CdOn = all || channels.Contains("cd");
    RecompOne.Runtime.Log.GpuOn = all || channels.Contains("gpu");
    RecompOne.Runtime.Log.DmaOn = all || channels.Contains("dma");
    RecompOne.Runtime.Log.SdkOn = all || channels.Contains("sdk");
    RecompOne.Runtime.Log.SpuOn = all || channels.Contains("spu");
    RecompOne.Runtime.Log.MdecOn = all || channels.Contains("mdec");
    Console.WriteLine($"[KF2] log channels: {string.Join(",", channels)}");
}

// PSY-Q's interrupt-callback table, per overlay.
//
// libapi's InterruptCallback(irq, func) writes the handler into a table of 11
// slots, and the runtime reads that table to deliver an IRQ. It cannot find the
// table on its own: it derives an address from the pointer the game hands to
// HookEntryInt, but that pointer is a jmp_buf (jb[1] is the interrupt stack this
// game puts 4 KB above it), and where the table sits relative to it differs per
// link. Here the derived address lands in game data -- 0x80074348 in GAME.EXE,
// which is a live variable, not a callback -- so IRQ 0 was calling whatever word
// happened to be sitting there once a frame.
//
// The real table is the one InterruptCallback indexes with irq*4, identified in
// each overlay from that function's body (it also ORs 1<<irq into I_MASK, and
// ResetCallback clears the 11 slots below it):
//
//     overlay  InterruptCallback  table         checks out as
//     open     0x8001E75C         0x8003DD48    table + 11*4 == 0x8003DD74, the
//     game     0x8005F8CC         0x8006E3D4    DMA callback table the DMA IRQ
//     end      0x8001AD28         0x80038D90    dispatcher reads, in all three
//
// The three executables are mutually exclusive, so the address is rebound as
// each one loads. What comes out of the table confirms it: slot 3 in `game`
// holds 0x8005FAE0, which is the DMA interrupt dispatcher -- the routine that
// walks DICR's seven channel flags and calls each channel's callback -- and slot
// 0 holds 0x8005F45C, its vblank counterpart. Those are the handlers the library
// registers, arriving at the slots the identification predicts.
RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.OverlayLoadedEvent>(e =>
{
    uint table = e.Name switch
    {
        "open" => 0x8003DD48u,
        "game" => 0x8006E3D4u,
        "end"  => 0x80038D90u,
        _ => 0u,   // the FDAT.T code modules share the resident executable's libapi
    };

    // FDAT.T modules sit at 0x8019F07C, which does not overlap OPEN/GAME/END, so
    // the dispatcher's region-overwrite path will not drop them on an executable
    // swap. END.EXE's heap starts at 0x80100000 and covers that RAM; leaving the
    // area module mapped would keep its functions callable from a table the
    // ending never owns.
    if (e.Name is "open" or "end")
    {
        foreach (var n in RecompOne.Runtime.Dispatch.Dispatcher.ActiveNames)
            if (n.StartsWith("fdat", StringComparison.Ordinal))
                RecompOne.Runtime.Dispatch.Dispatcher.Unload(n);
    }

    if (table == 0) return;
    RecompOne.Runtime.Interrupts.CallbackTable = table;
    Console.WriteLine($"[KF2] irq callback table: {e.Name} 0x{table:X8}");
});

// Scripted pad input, for reproducing a bug that needs a button press without a
// human at the keyboard:
//
//     KF2_AUTOPAD=5:Start:1000,8:Circle:200      seconds:button:holdMs
//
// The clock starts when the first area module loads -- that is the only moment
// in the boot sequence that means "in game", and it drifts with disc timing, so
// counting from process start would need recalibrating every run. Buttons are
// the Controller field names (Start, Select, Cross, Circle, Square, Triangle,
// L1, R1, L2, R2, Up, Down, Left, Right).
//
// It writes Controller.State only while a button is held, so an idle script
// never fights the keyboard. InputManager.Poll rewrites that field once a frame
// from the real keyboard; the game reads the pad far more often than that, so a
// hold of a few hundred ms lands regardless of who wrote last.
var autopad = Environment.GetEnvironmentVariable("KF2_AUTOPAD");
if (!string.IsNullOrWhiteSpace(autopad))
{
    var buttons = new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase)
    {
        ["Select"] = Controller.Select, ["Start"] = Controller.Start,
        ["Cross"] = Controller.Cross, ["Circle"] = Controller.Circle,
        ["Square"] = Controller.Square, ["Triangle"] = Controller.Triangle,
        ["L1"] = Controller.L1, ["R1"] = Controller.R1,
        ["L2"] = Controller.L2, ["R2"] = Controller.R2,
        ["L3"] = Controller.L3, ["R3"] = Controller.R3,
        ["Up"] = Controller.Up, ["Down"] = Controller.Down,
        ["Left"] = Controller.Left, ["Right"] = Controller.Right,
    };

    var press = new List<(double At, double Until, ushort Bit)>();
    foreach (var step in autopad.Split(',', StringSplitOptions.RemoveEmptyEntries))
    {
        var f = step.Split(':');
        if (f.Length != 3 || !buttons.TryGetValue(f[1].Trim(), out var bit))
            throw new ArgumentException($"KF2_AUTOPAD: bad step '{step}'");
        double at = double.Parse(f[0]), hold = double.Parse(f[2]) / 1000.0;
        press.Add((at, at + hold, bit));
    }

    var inGame = new ManualResetEventSlim(false);
    RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.OverlayLoadedEvent>(e =>
    {
        if (e.Name.StartsWith("fdat", StringComparison.Ordinal)) inGame.Set();
    });

    new Thread(() =>
    {
        inGame.Wait();
        Console.WriteLine($"[KF2] autopad: {press.Count} step(s) armed");
        var clock = System.Diagnostics.Stopwatch.StartNew();
        var last = 0xFFFF;
        while (true)
        {
            double t = clock.Elapsed.TotalSeconds;
            int mask = 0xFFFF;
            foreach (var (at, until, bit) in press)
                if (t >= at && t < until) mask &= ~bit;

            if (mask != 0xFFFF) Controller.State = (ushort)mask;
            if (mask != last)
            {
                Console.WriteLine($"[KF2] autopad t={t:F1}s state=0x{mask:X4}");
                last = mask;
            }
            Thread.Sleep(1);
        }
    }) { IsBackground = true, Name = "kf2-autopad" }.Start();
}

// The way back from an interface scaled too large to use:
//
//     KF2_UISCALE=1     force the interface scale for this run, and save it
//
// Theme.Scale is the runtime's DpiScale times the saved UiScale, and every popup is
// sized from it, so the 780x500 settings popup stops fitting a 1280x720 window at
// 1.44 -- inside UiScale's own range, and reachable at a UiScale of 1 by itself,
// since QueryDpiScale reads the primary monitor's GLFW content scale and GLFW's
// Wayland path reports the integer wl_output scale (a 1.15 display arrives as 2).
// patches/recompone/0019 clamps a popup to the viewport so the controls can no
// longer leave the window; this repairs a settings file that is already past that
// point, by writing the value as well as applying it.
Kf2.UiScale.Configure(Environment.GetEnvironmentVariable("KF2_UISCALE"));
Kf2.UiScale.Install();

// The keyboard layout the port ships. RecompOne's defaults are a console's
// defaults spelled on a keyboard -- face buttons on Z X A S, D-pad on the arrows
// -- and this game walks *and turns* on the D-pad, so the arrows alone are a tank
// control and a mouse in the other hand has nothing to do. W and S walk, A and D
// strafe (the game strafes on L1/R1), the arrows turn and look:
//
//     KF2_KEYS=stock    leave RecompOne's own bindings alone
//
// Configure must run before ConfigManager.Load, which is inside HostWindow's
// Initialize: Load overwrites these from settings.json when there is one and saves
// them when there is not, which is exactly what a default should do. Install then
// migrates an existing settings.json once, and only if every binding in it is
// still stock.
Kf2.KeyLayout.Configure();
Kf2.KeyLayout.Install();

// Host chrome a first run would otherwise inherit from RecompOne: windowed, no
// FPS overlay, a dark-grey theme, master volume at half. The picture settings
// the port ships live in the patches' own fallbacks; these four are the
// runtime's, so they are filled here the way KeyLayout fills the keymap --
// Defaults runs after ConfigManager.Load and only writes a key that is missing,
// so an existing config keeps what it had. MasterVolume is on Game, which Load
// deserialises over this when settings.json exists and saves it when it does not.
// Guarded for the same reason as KeyLayout.Configure: under the launcher,
// settings.json has already been loaded by now and this would reset the volume.
if (!System.IO.File.Exists("settings.json"))
    RecompOne.Runtime.Config.ConfigManager.Game.MasterVolume = 1f;
RecompOne.Runtime.Runtime.Defaults(v =>
{
    v.Default("ShowFps", true);
    v.Default("Fullscreen", true);
    v.Default("Background", "#000000");
});

// Frame pacing. The game's speed is its frame rate, so the rate the port draws at
// and the rate the game's own clock runs at are two different numbers here:
//
//     KF2_FPS=60                  frames a second to draw (default); any number,
//                                 or off for no pacing at all
//     KF2_TICKRATE=20             ticks a second the world runs at (default); 30
//                                 is the rate the game's own code asks for
//     KF2_FPS_LOGIC=full          do not tick the world on its own clock; scale
//                                 the movement deltas instead -- a comparison mode
//     KF2_FPS_GATE=80037C0C+8002A550+80040348+80046A60+8004910C+80033FBC+8002DC78   what is ticked
//
// The game's own frame gate (func_80017880, which spins on the vblank credit at
// 0x801B6CA8 until it reaches two) is skipped at every rate, because it decides
// the render rate and the world rate together and only knows one answer for both:
// 30. The port's own deadline paces the picture, and everything holding per-tick
// state that cannot draw -- stages 3, 4, 5, 6 and stage 13's fade stepper -- runs
// on the wall-clock tick rate instead of once per frame. 20 rather than 30 is a
// judgement about what the console achieved under load rather than what the code
// asked for; see FramePacing.LogicHz. A frame ends at the DrawOTag that
// follows a VSync *call*, not the one that follows an emulated vblank: the vblank
// is a fixed 60 Hz grid, so keying on it silently stopped pacing and stopped
// ticking above 60 fps, and the world ran at double speed.
//
// This is a correctness fix rather than an optional extra, so it lives in the
// project rather than in mods/ -- see the class comment. It attaches through
// RecompOne's HookManager, which detours a recompiled function by address at run
// time, so it needs no entry in config/kf2.json. Optional extras *are* real
// mods: mods/kf2debug, loaded by ModLoader and toggled in the game's own Mods
// panel.
Kf2.FramePacing.Configure(Environment.GetEnvironmentVariable("KF2_FPS"),
                          Environment.GetEnvironmentVariable("KF2_FPS_GATE"),
                          Environment.GetEnvironmentVariable("KF2_FPS_LOGIC"),
                          Environment.GetEnvironmentVariable("KF2_TICKRATE"),
                          Environment.GetEnvironmentVariable("KF2_FPS_PROBE"));
Kf2.FramePacing.Install();

// Stage 1 clears the screen tint every frame and the stages that set it are gated,
// so above the tick rate the death fade and the damage flash strobed. Stage 1 is
// in C# now and resets the tint only on a frame the world ticks. See "The tints
// strobed between ticks".
//
//     KF2_TINTHOLD=0        the recompiled routine: the strobe back -- comparison only
//     KF2_TINTHOLD=verify   run both and compare RAM and registers
Kf2.TintHold.Configure(Environment.GetEnvironmentVariable("KF2_TINTHOLD"));
Kf2.TintHold.Install();

// The frame profiler: where each frame's time goes, by section. Every hooked
// function is timed inside HookManager (its recompiled body and each patch's
// delegate apart), the runtime times the present path, and the sleeps are sections
// of their own so a capped frame reads as work plus waiting. Shift+P opens the panel,
// and recording runs while it is open. See "Profiling a frame" in docs/DEVELOPMENT.md.
//
//     KF2_PROFILE=1             record from boot, a console summary every 5 s
//     KF2_PROFILE=panel         record from boot and open the panel
//     KF2_PROFILE_OUT=path.csv  every frame's sections; scripts/profile_report.py
//     KF2_PROFILE_SPIKE=12      a console line per frame over 12 ms of work
//     KF2_PROFILE_FUNCS=stages  time all thirteen main-loop stages, or name
//                               functions: game:80040348+800342D8
Kf2.FrameProfiler.Configure(Environment.GetEnvironmentVariable("KF2_PROFILE"),
                            Environment.GetEnvironmentVariable("KF2_PROFILE_OUT"),
                            Environment.GetEnvironmentVariable("KF2_PROFILE_SPIKE"),
                            Environment.GetEnvironmentVariable("KF2_PROFILE_FUNCS"));
Kf2.FrameProfiler.Install();

// The frame viewer: capture one run of stage 13 and scrub it a GP0 command at a
// time, replayed on a detached software GPU -- who emitted each primitive, what it
// cost to send, the fragments it rasterized and the GL batch submits it forced.
// Shift+F opens the panel. See "Watching a frame being built" in docs/DEVELOPMENT.md.
//
//     KF2_FRAMEVIEW=panel          open the panel at boot
//     KF2_FRAMEVIEW_CAPTURE=20,40  capture at these seconds after boot, summary on the console
//     KF2_FRAMEVIEW_OUT=dir        also write each capture's commands as CSV
Kf2.FrameCapture.Configure(Environment.GetEnvironmentVariable("KF2_FRAMEVIEW"),
                           Environment.GetEnvironmentVariable("KF2_FRAMEVIEW_CAPTURE"),
                           Environment.GetEnvironmentVariable("KF2_FRAMEVIEW_OUT"));
Kf2.FrameCapture.Install();

// The one thing frame pacing cannot reach: the in-game menu is a modal sub-loop
// inside stage 3 (func_80029CBC jal's func_80018E80, which blocks for the whole
// session and renders its own frames), so no gated stage is being called while it
// runs and nothing in it is on the tick clock. Two things in there are counted in
// vblanks and so ran at the render rate instead. The cursor's auto-repeat is a
// spin on six VSync(0) calls in func_80022E90 -- a vblank each on hardware, 1.2 ms
// at 144 fps here -- and those calls are held to the 60 Hz grid. The cursor's
// blink steps once per menu frame in func_80022530, the frame head, which cannot
// be skipped because it swaps the buffer, so a pre/post pair puts its counter
// back on a frame the grid did not advance on. Nothing sleeps for the blink: the
// menu still draws at the render rate, the wink is only capped at 60 Hz.
//
//     KF2_MENUPACING=0        leave both on the frame clock -- comparison only
//     KF2_MENUPACING_PROBE=1  what each repeat cost, and the blink's step rate
Kf2.MenuPacing.Configure(Environment.GetEnvironmentVariable("KF2_MENUPACING"),
                         Environment.GetEnvironmentVariable("KF2_MENUPACING_PROBE"));
Kf2.MenuPacing.Install();

// A menu -- the in-game menu, a shop, the save list -- pastes a stored 320x240 copy
// of the frozen frame behind itself every frame, so the widescreen margin is never
// drawn and nothing that needs depth (AO, the Z-buffer) reaches it. MenuWorld
// replaces the menu's presenter and draws the world live, into an ordering table
// of its own chained in front of the menu's; nothing that advances the world runs.
//
//     KF2_MENUWORLD=0         the game's frozen copy -- comparison only
//     KF2_MENUWORLD_PROBE=1   world passes a second, primitive bytes, overflows
Kf2.MenuWorld.Configure(Environment.GetEnvironmentVariable("KF2_MENUWORLD"),
                        Environment.GetEnvironmentVariable("KF2_MENUWORLD_PROBE"));
Kf2.MenuWorld.Install();

// Sign and dialogue text drawn as text on an opaque box, decoded from the message's
// own picture in RAM against a glyph table built by scripts/msg_glyphs.py. Off.
//
//     KF2_MESSAGETEXT=1         on
//     KF2_MESSAGETEXT_PROBE=1   each message's key and decoded text
//     KF2_MESSAGETEXT_TEST=3:0  open these messages from the main loop (KF2_AUTOPAD dismisses)
Kf2.MessageText.Configure(Environment.GetEnvironmentVariable("KF2_MESSAGETEXT"),
                          Environment.GetEnvironmentVariable("KF2_MESSAGETEXT_PROBE"),
                          Environment.GetEnvironmentVariable("KF2_MESSAGETEXT_TEST"));
Kf2.MessageText.Install();

// The loading screen's walking figure is the same bug in a third place, and the
// shared cause is that the game counts time in VSync(0) calls while a VSync(0)
// call here is no longer a vblank. A disc read is a blocking wait rather than a
// frame: func_80017CA8 spins until the CD job drains, calling the animator
// func_8001883C four times an iteration, and that function draws the figure
// straight into VRAM with ClearImage/MoveImage -- no ordering table, so no frame
// boundary, so neither FramePacing nor LoopPacing can see it at all. It ends in
// DrawSync(0); VSync(0), so one call was one vblank on hardware; here the figure
// took its 84 steps in 352 ms at KF2_FPS=144 (238.6 a second) against 1715 ms at
// the 20 fps default (49.0). Every blocking VSync inside the two disc waits is
// held to the 60 Hz grid instead, which restores the console's 49 a second at
// every rate -- and costs the load its length back: 1.7 s rather than 0.35 s
// above 60 fps, which is what the 20 fps default already pays. Holding the
// animator's three counters instead keeps loads short and was measured worse (it
// drops the default to 36-40), so it is not what shipped.
//
//     KF2_LOADPACING=0        leave it on the render rate -- comparison only
//     KF2_LOADPACING_PROBE=1  the figure's steps, elapsed and rate per load, and
//                             the blocking VSync calls the wait was made of
Kf2.LoadPacing.Configure(Environment.GetEnvironmentVariable("KF2_LOADPACING"),
                         Environment.GetEnvironmentVariable("KF2_LOADPACING_PROBE"));
Kf2.LoadPacing.Install();

// The flames. Billboard sprites -- table 4 of the four the renderer walks,
// 0x80195174, 128 records of 0x18 -- animate by stepping a cel index at rec+0x5
// whenever a single global counter at 0x80195170 divides exactly by the slot's
// interval at rec+0x4, and that counter is incremented as the last act of
// func_800331B4, stage 13's world and object walk. So it counts *rendered* frames
// and every animated sprite in the game burns, sparks or flickers at the render
// rate -- 7.2x too fast at 144 against a 20 Hz world, and reported from play as
// "these flames still run really fast at a high framerate". These are not the
// eight scrolling texture slots at 0x80192D58, which func_8002DC78 owns and the
// stage gate has held since it was found.
//
// The gate cannot reach this one: func_800331B4 draws, so it cannot be skipped.
// A hold/restore pair around it puts the counter and the 128 cel bytes back on a
// frame the world did not advance on, which holds the whole system through its
// one clock word. At the tick rate every frame is a tick frame, so the port's own
// 20 fps default is unchanged with this on or off.
//
//     KF2_SPRITEANIM=0        leave the animation on the render rate -- comparison only
//     KF2_SPRITEANIM_PROBE=1  cel changes a second, live slots, walks a second
Kf2.SpriteAnim.Configure(Environment.GetEnvironmentVariable("KF2_SPRITEANIM"),
                         Environment.GetEnvironmentVariable("KF2_SPRITEANIM_PROBE"));
Kf2.SpriteAnim.Install();

// Which words of the game's memory change at the render rate rather than at the
// tick rate -- the instrument that turns "something looks too fast" into an
// address. Samples on the emulated vblank, which is a wall-clock 60 Hz grid since
// patches/recompone/0021, so two runs at different render rates are directly
// comparable. Off by default; it costs a compare over the game's data region
// sixty times a second.
//
//     KF2_RATECENSUS=1                       on
//     KF2_RATECENSUS_RANGE=80060000:801C0000 the window to watch (the default)
//     KF2_RATECENSUS_OUT=path                where to write (default ratecensus.txt)
//     KF2_RATECENSUS_PERIOD=5                seconds between dumps
Kf2.RateCensus.Configure(Environment.GetEnvironmentVariable("KF2_RATECENSUS"),
                         Environment.GetEnvironmentVariable("KF2_RATECENSUS_RANGE"),
                         Environment.GetEnvironmentVariable("KF2_RATECENSUS_OUT"),
                         Environment.GetEnvironmentVariable("KF2_RATECENSUS_PERIOD"));
Kf2.RateCensus.Install();

// The other half of drawing above 30: the world advances 30 times a second, so
// without this the camera does too and the extra frames show the same view twice.
// One pre/post pair around stage 8 (func_80025A1C), which is the whole of "build
// the render camera from the player state" and the only thing between that state
// and the picture -- so the extrapolation lives for one function call and reaches
// nothing else.
//
//     KF2_SMOOTH=0        off; on by default, or the view steps at the logic rate
//     KF2_SMOOTH_POS=0    leave the position at the tick (carried by default)
//     KF2_SMOOTH_COMPASS=0  leave the compass needle on the tick (carried by default)
//     KF2_SMOOTH_GAUGES=0   leave the HP and MP gauges on the tick (carried by default)
//     KF2_SMOOTH_PROBE=1  what is being carried, per second
//     KF2_SMOOTH_PROBE=2  also trace every frame for 400 ms after an area load
Kf2.FrameSmoothing.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH"),
                             Environment.GetEnvironmentVariable("KF2_SMOOTH_POS"),
                             Environment.GetEnvironmentVariable("KF2_SMOOTH_COMPASS"),
                             Environment.GetEnvironmentVariable("KF2_SMOOTH_GAUGES"),
                             Environment.GetEnvironmentVariable("KF2_SMOOTH_PROBE"));
Kf2.FrameSmoothing.Install();

// The other half of the same problem: the camera moves every frame now, but an
// object whose position advances on the tick still arrives in 50 ms steps, and
// against a smoothly sliding world that reads worse than not smoothing at all.
// One pre/post pair around stage 13, interpolating the object table at 0x80177714
// -- the table the renderer is actually handed, not the entity records, which are
// a copy stage 4 makes of it.
//
//     KF2_SMOOTH_OBJECTS=1        on; off by default
//     KF2_SMOOTH_OBJECTS_PROBE=1  how much is being carried, per second
//     KF2_SMOOTH_OBJECTS_GUARD=   strict | sticky | continuous; which step counts
//                                 as a placement rather than as motion, on the
//                                 entity table only -- every other table keeps the
//                                 strict limit, since the projectile tables recycle
//                                 slots. `continuous` by default. A picture, so it
//                                 is a setting under Video as well.
Kf2.ObjectSmoothing.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH_OBJECTS"),
                              Environment.GetEnvironmentVariable("KF2_SMOOTH_OBJECTS_PROBE"),
                              Environment.GetEnvironmentVariable("KF2_SMOOTH_OBJECTS_GUARD"));
Kf2.ObjectSmoothing.Install();

// The third of the same problem: origin and facing are carried now, but a
// creature's *pose* is a mesh morph and its clock only advances on the tick, so
// the shape still steps against a body that glides. Stage 13 re-runs the blender
// every frame -- what is stuck is the time it blends to -- so this drives that
// clock instead of touching vertices: one pre on stage 13 for the frame, a
// pre/post pair on the model submit (func_80032588) and one on the MO clip clock
// (func_8003486C), handing it floor(lerp(prev, cur, phase)) and adding the
// leftover fraction onto the 12.12 weight the decoder consumes. Nothing is
// written to game state -- a register and the caller's own stack temp.
//
//     KF2_SMOOTH_ANIM=1         on, in whichever mode the settings hold;
//                               off by default
//     KF2_SMOOTH_ANIM=timeline  interpolate on the clip's own timeline, the
//                               length read out of the segment table (default)
//     KF2_SMOOTH_ANIM=time      comparison: lerp the clip time between the two
//                               ticks, with the wrap and re-seek classifiers
//     KF2_SMOOTH_ANIM=weight    comparison: the blend weight only, clamped to
//                               the segment the game itself chose
//     KF2_SMOOTH_ANIM_PROBE=1   morph vs rigid submits, the verdict census and
//                               the weights carried, per second
Kf2.AnimSmoothing.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH_ANIM"),
                            Environment.GetEnvironmentVariable("KF2_SMOOTH_ANIM_PROBE"));
Kf2.AnimSmoothing.Install();

// The fourth of the same problem: the camera glides and the creatures walk, but
// the water, a slime's skin and the other scrolling textures still jump a texel
// every tick. func_8002DC78 re-uploads a vertically wrapped copy into VRAM, and
// the stage gate already holds that to 20 Hz -- the rate per second is right,
// the picture of it is not. A fractional UV offset rounds away (the rasterizer
// snaps to a whole texel), so this publishes the dest rects and leftover phase
// to the prim shaders, which blend the two wrap-rows the integer upload sits
// between. GL only; Fast geometry is not required.
//
//     KF2_SMOOTH_FLUID=0        leave them on the tick -- comparison only
//     KF2_SMOOTH_FLUID_PROBE=1  live slots, dest rects, leftover shift
Kf2.FluidSmoothing.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH_FLUID"),
                             Environment.GetEnvironmentVariable("KF2_SMOOTH_FLUID_PROBE"));
Kf2.FluidSmoothing.Install();

// The menu is not the only loop of that shape, and naming them one at a time is
// how the list was being found. A *modal loop* -- a function that takes the main
// loop over and presents its own frames -- is entered from a gated stage, so the
// gate decides only whether it is entered and never cuts one in half; inside, the
// loop iterates once per rendered frame and everything it steps runs at the render
// rate. That is the transition fade, the cutscene and message-box loops, and the
// item-use and spell-cast animations: a picked-up item spinning too fast at 144.
// The fix is structural rather than per-counter -- the loop's *body* is held to one
// run per world tick, so every number inside it is right without being found. One
// pre-hook on stage 9, whose only caller is the main loop, tells a modal frame from
// the main loop's.
//
// Holding it there is only half. Pacing the loop's frames to the tick makes the
// speed right and the picture step at the tick rate, because the frame the loop
// draws *is* the tick and LogicPhase is 0 on every one of them, so the smoothing
// patches have nothing to carry. So the gap between iterations is filled with
// redraws -- stage 13 called again at the phase the frame stands at, which is what
// func_80037B5C already does inside a stage. The world then advances at LogicHz
// while the picture is drawn at the render rate, with objects and poses carried by
// the patches that already do it everywhere else. The menu draws no world, so it is
// paced at the vblank instead. It does nothing at or below the tick rate, which is
// where the defect does not exist either.
//
// A redraw replays stage 13 with the two pointers the modal loop itself passed it
// (func_800342D8(VECTOR *pos, SVECTOR *rot)), recorded by a pre-hook. Calling it
// without them -- which the first version of this did -- builds the frame's view
// matrix out of whatever the register file held, which is a black flicker and a
// stale buffer left on screen. Stage 8 is deliberately not replayed: it would
// overwrite a cutscene's scripted camera with the player's, and the player camera
// cannot move inside a modal loop anyway.
//
// The player camera cannot move inside a modal loop -- no gated stage runs there --
// but a camera the *loop itself* builds can: func_8004831C ramps a heading 0->0x1000
// by 0x200 an iteration, a full turn in 32 steps, which held to the tick is what play
// reported as "the camera is visibly moving at a lower framerate in the modals". So a
// redraw carries the block the loop passed, lerp(prev, cur, phase), applied in the
// pre and taken back in the post.
//
//     KF2_LOOPPACING=0        leave them on the render rate -- comparison only
//     KF2_LOOPPACING=pace     hold the loop but do not redraw -- comparison only
//     KF2_LOOPPACING=nocarry  redraw, but do not carry the loop's own pan
//     KF2_LOOPPACING_PROBE=1  modal frames a second, world and interface
//     KF2_LOOPPACING_PROBE=2  also how far the loop's own view moves per iteration
//
// The redraw has to be asked for once the smoothing patches' own posts have put
// the tables back. That is declared on the hook (Stage13.HookOrder.Redraw, 0070),
// so where this line sits no longer matters.
Kf2.LoopPacing.Configure(Environment.GetEnvironmentVariable("KF2_LOOPPACING"),
                         Environment.GetEnvironmentVariable("KF2_LOOPPACING_PROBE"));
Kf2.LoopPacing.Install();
Kf2.EndingHold.Install();

// Which routine in the renderer drew how much of the frame. Stage 13 is the only
// filler of the display list, but nothing records which of its callees draws the
// map, the objects, or the first-person weapon -- and "the arm steps at the tick
// rate" has two different fixes depending on which. Measured off the primitive
// arena's bump pointer across each routine, so it costs two reads per routine per
// frame and nothing per polygon.
//
//     KF2_DRAWCENSUS=1    the per-routine primitive census, every two seconds
Kf2.DrawCensus.Configure(Environment.GetEnvironmentVariable("KF2_DRAWCENSUS"));
Kf2.DrawCensus.Install();

// Whether the frame could be smoothed without knowing any of the tables at all.
// Every smoother in the port carries a named table between ticks, and that set has
// been incomplete three times running -- each miss found by someone noticing a
// stepping door rather than by a counter. One layer down, at DrawOTag, the frame
// is a list of finished primitives; if a primitive can be recognised in the
// previous tick's list its screen position could be carried by the tick phase, and
// that would be one hook covering doors, tiles, enemies and the drawbridge alike.
// Whether it can be recognised is the whole question, and this only measures it.
//
//     KF2_PACKETMATCH=1   the match census, once a second, by three candidate keys
//     KF2_PACKETMATCH=2   also name the worst-matching contexts
Kf2.PacketMatch.Configure(Environment.GetEnvironmentVariable("KF2_PACKETMATCH"));
Kf2.PacketMatch.Install();

// Refuse a hit resolution that would read a non-pointer as a pointer. For
// docs/TODO.md #14 -- the crash after the final boss dies and the cutscene hands
// the player the Moonlight Sword, above the tick rate and not at it. The first
// stack trace of it puts the fault in stage 3's weapon reach scan
// (func_800271D0 -> func_8003A9CC -> func_8003A490 -> func_8003A448 reading
// 0x0FFF0000), in the main loop and nowhere near fdat23's post-boss modal loops,
// which is where it had been assumed to live. Nothing on that path bounds the
// creature type byte or checks the slot is occupied, and the console has no fault
// path for the read -- so the guard replays the derivation, refuses only the four
// states that are certain to fault, and names which one it was. Reads only, and
// the report is what replaces the stack trace on the next playthrough.
//
//     KF2_HITGUARD=0      let it fault, which is a hard crash -- comparison only
//     KF2_HITPROBE=1      also report a census of what the hit check saw
//     KF2_HITPROBE=2      also report every call, which is very loud
Kf2.HitGuard.Configure(Environment.GetEnvironmentVariable("KF2_HITGUARD"),
                       Environment.GetEnvironmentVariable("KF2_HITPROBE"));
Kf2.HitGuard.Install();

// Dithering. Clearing the GPU's dither bit is one pre/post hook pair on each of
// PutDrawEnv and DrawOTag, and it is a patch rather than a mod because it is a
// picture the port should be able to offer without a package having to load: the
// switch itself lives in Display beside vsync.
//
//     KF2_NODITHER_PROBE=1   report the two draw-mode sources, and GPUSTAT bit 9
Kf2.NoDither.Configure(Environment.GetEnvironmentVariable("KF2_NODITHER_PROBE"));
Kf2.NoDither.Install();

// What the renderer was asked to draw, and what was in VRAM to draw it from. For
// "the game runs but has no textures" on a machine that cannot be attached to: it
// separates a game that submitted flat polygons from a VRAM page that never
// received its upload from a fetch that is sampling it wrongly, and writes the
// answer to texprobe.log beside the saves.
//
//     KF2_TEXPROBE=1   a line a second, plus a per-page VRAM census
Kf2.TexProbe.Configure(Environment.GetEnvironmentVariable("KF2_TEXPROBE"));

// Perspective-correct textures. The GPU is handed polygons with no depth in them,
// so it interpolates U and V linearly and the texture swims; the depth still exists
// one step earlier, in the GTE, and the address the coordinate is stored at is what
// ties the two back together -- the same word is copied into the packet and read
// back out of it by DrawOTag. All of the work is in the runtime (GteVertexMap, the
// rasterizer and the prim shaders) because a texture coordinate is decided far below
// anything HookManager can reach -- this is the switch and the report:
//
//     KF2_PERSPECTIVE=0           off, for the console's own affine mapping
//     KF2_PERSPECTIVE_PROBE=1     report the vertex map's hit rate
//     KF2_PERSPECTIVE_FALLBACK=1  also guess by screen position on a miss, which is
//                                 the mechanism the map replaced, kept for comparison
//
// A patch rather than a mod for the same reason the dither switch is one: it is a
// picture the port should be able to offer without a package having to load. Its
// switch is under Video beside that one.
Kf2.Perspective.Configure(Environment.GetEnvironmentVariable("KF2_PERSPECTIVE"),
                          Environment.GetEnvironmentVariable("KF2_PERSPECTIVE_PROBE"),
                          Environment.GetEnvironmentVariable("KF2_PERSPECTIVE_FALLBACK"));
Kf2.Perspective.Install();

// Sub-pixel vertex positioning, which is the other half of the same discarded
// number: the GTE projects to 16.16 and keeps only the whole part, so a vertex
// drifting slowly sits still and then jumps a pixel, and its polygon twitches. The
// fraction is recovered through the same map and the same address, one shift earlier
// in the same expression, and served independently of the depth:
//
//     KF2_SUBPIXEL=1        on; 0 or unset leaves vertices on whole pixels
//     KF2_SUBPIXEL_PROBE=1  report how far vertices are actually moving
//     KF2_SUBPIXEL_CULL=0   cull on whole pixels, as the game does (fractional corners by default)
//
// Off by default where perspective correction is on -- not because it is riskier,
// the same "a miss is the old behaviour" argument covers both, but because that one
// was measured before it became a default and this one has not been. Its switch is
// under Video with the others.
Kf2.Subpixel.Configure(Environment.GetEnvironmentVariable("KF2_SUBPIXEL"),
                       Environment.GetEnvironmentVariable("KF2_SUBPIXEL_PROBE"),
                       Environment.GetEnvironmentVariable("KF2_SUBPIXEL_CULL"));
Kf2.Subpixel.Install();

// Z-buffer. The GPU is handed polygons with no depth in them, so occlusion is
// whatever order the game stuffed the ordering table in — one OTZ per polygon,
// back to front. Interpenetrating surfaces can only take turns in front of each
// other. The depth still exists one step earlier, in the GTE, and the same map
// that feeds perspective correction hands it to both rasterizers as a per-pixel
// test:
//
//     KF2_ZBUFFER=1        on; 0 or unset leaves the ordering table in charge
//     KF2_ZBUFFER_PROBE=1  report how many triangles actually depth-tested
//     KF2_ZBUFFER_PROBE=2  also the frame's occlusion census: which large primitive
//                          is standing in front of which, and how much of the
//                          picture that costs
//     KF2_ZBUFFER_SOURCE=map  depth from the address map instead of the assemblers
//     KF2_ZBUFFER_BIAS=1 KF2_ZBUFFER_SLOPE=0.5  coplanar tolerance: SZ units, and pixels of slope
//     KF2_BLENDORDER=0        blended surfaces (water) drawn in table order again, under
//                             opaque geometry behind them that the table put later (0079)
//     KF2_BLENDORDER_PROBE=1  opaque samples drawn over a nearer translucent surface
//
// Off by default where perspective correction is on -- the recovered number is
// the same one, but the picture has not been checked by eye. Its switch is under
// Video with the others.
Kf2.ZBuffer.Configure(Environment.GetEnvironmentVariable("KF2_ZBUFFER"),
                      Environment.GetEnvironmentVariable("KF2_ZBUFFER_PROBE"),
                      Environment.GetEnvironmentVariable("KF2_ZBUFFER_THRESHOLD"),
                      Environment.GetEnvironmentVariable("KF2_ZBUFFER_SOURCE"),
                      Environment.GetEnvironmentVariable("KF2_ZBUFFER_BIAS"),
                      Environment.GetEnvironmentVariable("KF2_ZBUFFER_SLOPE"),
                      Environment.GetEnvironmentVariable("KF2_BLENDORDER"),
                      Environment.GetEnvironmentVariable("KF2_BLENDORDER_PROBE"));
Kf2.ZBuffer.Install();

// Ambient occlusion -- contact shading in the corners, under the doorframes and
// where a pillar meets the floor, from the same recovered view depth. The GPU has
// no depth buffer and the game hands it none, so there is no G-buffer to run a
// screen-space pass against and no way to build one by a prepass: the geometry
// arrives incrementally through GP0 and nothing knows the frame is finished until
// it is. What supplies it instead is painter's order -- with every 3D triangle
// writing its recovered depth and the test left at GL_ALWAYS, the last write at a
// pixel is the nearest visible surface, so the finished attachment is a correct
// visible-surface depth buffer that rejected nothing on the way:
//
//     KF2_AO=1              on; 0 or unset leaves the picture flat
//     KF2_AO_RADIUS=512     how far a surface reaches to shade its neighbour, in
//                           the game's own world units (a floor tile is 2048)
//     KF2_AO_STRENGTH=0.8   how dark a fully occluded pixel goes, 0..1
//     KF2_AO_BIAS=0.08      the angular bias that stops a flat wall shading itself
//     KF2_AO_SAMPLES=16     samples per pixel in the occlusion pass
//     KF2_AO_MAXDEPTH=24000 beyond this view depth the pass returns unoccluded
//     KF2_AO_PROBE=1        the coverage, the projection recovered from the GTE,
//                           and the passes actually run
//     KF2_AO_PROBE=2        also read the occlusion texture back and census it --
//                           the one reading that tells a pass that shaded
//                           something from a pass that ran and returned white
//
// Everything with no recovered depth stamps the *far* plane instead, so the HUD,
// the menus and any triangle the vertex map missed are neither shaded nor allowed
// to occlude; semi-transparent primitives write nothing, so a death fade does not
// erase the world's depth under it. Off by default for the sub-pixel reason -- the
// mechanism is measured, the picture has not been judged -- and it is deliberately
// not authentic. Installed after ZBuffer because it shares that patch's writes and
// after Pgxp would be too late for nothing; its switch is under Video with the
// others and the tuning is on the console. GL backend only; the work is
// patches/recompone/0040.
Kf2.AmbientOcclusion.Configure(Environment.GetEnvironmentVariable("KF2_AO"),
                               Environment.GetEnvironmentVariable("KF2_AO_RADIUS"),
                               Environment.GetEnvironmentVariable("KF2_AO_STRENGTH"),
                               Environment.GetEnvironmentVariable("KF2_AO_BIAS"),
                               Environment.GetEnvironmentVariable("KF2_AO_SAMPLES"),
                               Environment.GetEnvironmentVariable("KF2_AO_MAXDEPTH"),
                               Environment.GetEnvironmentVariable("KF2_AO_PROBE"),
                               Environment.GetEnvironmentVariable("KF2_AO_NORMALS"),
                               Environment.GetEnvironmentVariable("KF2_AO_QUALITY"));
Kf2.AmbientOcclusion.Install();
Kf2.AoWorld.Configure(Environment.GetEnvironmentVariable("KF2_AO_WORLD"),
                      Environment.GetEnvironmentVariable("KF2_AO_WORLD_STRENGTH"),
                      Environment.GetEnvironmentVariable("KF2_AO_WORLD_RADIUS"),
                      Environment.GetEnvironmentVariable("KF2_AO_WORLD_PROBE"));
Kf2.AoWorld.Install();

// Screen-space reflections on water (patches/recompone/0067): a pass at present
// beside the occlusion pass, reading the same depth and a surface buffer that keeps
// the water the depth buffer cannot (it is translucent). Off by default: the
// mechanism is measured, the picture has not been judged. See "Screen-space
// reflections" in docs/RENDERING.md.
Kf2.Reflections.Configure(Environment.GetEnvironmentVariable("KF2_SSR"),
                          Environment.GetEnvironmentVariable("KF2_SSR_STRENGTH"),
                          Environment.GetEnvironmentVariable("KF2_SSR_F0"),
                          Environment.GetEnvironmentVariable("KF2_SSR_DISTANCE"),
                          Environment.GetEnvironmentVariable("KF2_SSR_STEPS"),
                          Environment.GetEnvironmentVariable("KF2_SSR_THICKNESS"),
                          Environment.GetEnvironmentVariable("KF2_SSR_SKY"),
                          Environment.GetEnvironmentVariable("KF2_SSR_RESOLUTION"),
                          Environment.GetEnvironmentVariable("KF2_SSR_PROBE"),
                          Environment.GetEnvironmentVariable("KF2_SSR_FOGCURVE"));
Kf2.Reflections.Install();

// Murky water: the same pass lays a dark colour over water by how much of it the
// view ray crosses, on its own switch -- no reflection needs to be on. Off by
// default. See "Murky water" in docs/RENDERING.md.
Kf2.Murk.Configure(Environment.GetEnvironmentVariable("KF2_MURK"),
                   Environment.GetEnvironmentVariable("KF2_MURK_DISTANCE"),
                   Environment.GetEnvironmentVariable("KF2_MURK_HAZE"));
Kf2.Murk.Install();
Kf2.MurkLevel.Configure(Environment.GetEnvironmentVariable("KF2_MURK_PROBE"));

// Water waves: a slow swell moves the water's own vertices (the tile walk points each
// water mesh at a moved copy), and ripples push and shade its texture per pixel
// (patches/recompone/0078), on the world's clock. Off by default. See "Water waves" in docs/RENDERING.md.
Kf2.Waves.Configure(Environment.GetEnvironmentVariable("KF2_WAVES"),
                    Environment.GetEnvironmentVariable("KF2_WAVES_PROBE"));
Kf2.Waves.Install();

// Planar reflections (patches/recompone/0068): the tile walk and the object walk's
// submits run a second time from the camera mirrored in the water, into an ordering
// table of the port's own, drawn into a texture the reflection pass reads first.
// Independent of KF2_SSR. Off by default: measured, not judged. See "Planar
// reflections" in docs/RENDERING.md.
Kf2.PlanarWalk.Configure(Environment.GetEnvironmentVariable("KF2_PLANAR"),
                         Environment.GetEnvironmentVariable("KF2_PLANAR_TOLERANCE"),
                         Environment.GetEnvironmentVariable("KF2_PLANAR_RIPPLE"),
                         Environment.GetEnvironmentVariable("KF2_PLANAR_BIAS"),
                         Environment.GetEnvironmentVariable("KF2_PLANAR_PROBE"));
Kf2.PlanarWalk.Install();

// The retained scene (patches/recompone/0072): the map kept on the GPU in world
// space, built from the map data, so the reflections draw the world again without
// the game's walks: every water or authored plane, and a cubemap from the camera
// in place of the screen-space march. Independent of KF2_SSR. Off by default:
// measured, not judged. See "The retained scene" in docs/RENDERING.md.
Kf2.RetainedMap.Configure(Environment.GetEnvironmentVariable("KF2_RETAINED"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_PLANAR"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_CUBE"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_CUBESIZE"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_CULL"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_GATE"),
                          Environment.GetEnvironmentVariable("KF2_RETAINED_PROBE"));
Kf2.RetainedMap.Install();

// The remaster (docs/REMASTER.md): authored data from a pack, applied over the
// game: materials on tile faces, read by the reflection pass, and point and spot
// lights in the prim shader (0071). Off by default and nothing is authored until
// the editor (Shift+E) saves something.
Kf2.Remaster.Host.Configure(Environment.GetEnvironmentVariable("KF2_REMASTER"),
                            Environment.GetEnvironmentVariable("KF2_REMASTER_PACK"),
                            Environment.GetEnvironmentVariable("KF2_REMASTER_PROBE"),
                            Environment.GetEnvironmentVariable("KF2_REMASTER_LIGHTS"),
                            Environment.GetEnvironmentVariable("KF2_REMASTER_ATMOS"));
// Shadows for the authored lights (0077): a depth cubemap per light from the retained map
// and the frame's models.
Kf2.Remaster.Lights.ConfigureShadows(Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOWS"),
                                     Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOW_SIZE"),
                                     Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOW_BIAS"),
                                     Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOW_OFFSET"),
                                     Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOW_SOFT"),
                                     Environment.GetEnvironmentVariable("KF2_REMASTER_SHADOW_MODELS"));
Kf2.Remaster.Host.Install();
// Phase 4's texture-key census: which replacement keys an area draws, and which a
// pack covers. KF2_TEXKEY=triangle keys each triangle on its own UVs, as upstream does.
Kf2.Remaster.TextureCensus.Configure(Environment.GetEnvironmentVariable("KF2_TEXCENSUS"),
                                     Environment.GetEnvironmentVariable("KF2_TEXKEY"));
Kf2.Remaster.TextureCensus.Install();
// Phase 5's read census: which code reads and writes the area's light records.
Kf2.Remaster.LightCensus.Configure(Environment.GetEnvironmentVariable("KF2_LIGHTCENSUS"));
Kf2.Remaster.LightCensus.Install();

// PGXP -- upstream RecompOne's own vertex tracking, backported as
// patches/recompone/0034-0036, and the second mechanism the port has for the one
// number everything above depends on. GteVertexMap pairs memory reads and writes
// by value and cannot see a vertex the game computes; PGXP is told what every
// register holds, by hooks the recompiler emits, so it does not have to guess.
//
//     KF2_PGXP=1              use it instead of the address map
//     KF2_PGXP_TEXTURE=0      its share of perspective correction off
//     KF2_PGXP_CULLING=0      leave backface culling on truncated positions
//     KF2_PGXP_CPU=0          no per-instruction register tracking
//     KF2_PGXP_MEMORY=0       no RAM shadow
//     KF2_PGXP_VERTEXCACHE=0  no screen-position fallback
//     KF2_PGXP_CACHEW=0       let that fallback answer positions but not depths
//     KF2_PGXP_TOLERANCE=2    how far a recovered position may sit from the
//                             packet's before it is refused; -1 turns it off
//     KF2_PGXP_PROBE=1        report the coverage, against the address map's
//
// Off by default: the coverage is measured, the picture is not. Installed after
// ZBuffer because the depth buffer reads whatever this decides.
Kf2.Pgxp.Configure(Environment.GetEnvironmentVariable("KF2_PGXP"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_TEXTURE"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_CULLING"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_CPU"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_MEMORY"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_VERTEXCACHE"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_CACHEW"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_TOLERANCE"),
                   Environment.GetEnvironmentVariable("KF2_PGXP_PROBE"));
Kf2.Pgxp.Install();

// True color (24-bit) output for the GL backend. The console renders into 15-bit
// VRAM, so a shaded fog gradient bands into steps unless the dither hides it with
// a crosshatch; this keeps eight bits per channel and removes the banding without
// the crosshatch. Off by default -- the default picture is the console's:
//
//     KF2_TRUECOLOR=1  on; 0 or unset keeps the 15-bit output
//
// The mechanism is patches/recompone/0021 (the render-target format and the
// fragment shader). Its switch is under Video with the others.
Kf2.TrueColor.Configure(Environment.GetEnvironmentVariable("KF2_TRUECOLOR"));
Kf2.TrueColor.Install();

// Anisotropic filtering. A screen pixel covers an *area* of the texture, not a
// point, and on a floor running away to the horizon that area is a long thin
// sliver of texels; the console read one texel out of it, and which texel it read
// changes completely for a sub-pixel movement of the camera. That is the crawling,
// sparkling floor. The fragment shader samples along the sliver instead:
//
//     KF2_ANISO=8           taps along the long axis; 1 or "off" is one sample
//     KF2_ANISO_PROBE=1     the level, and whether the uniform reaches the shader
//
// Off by default -- the mechanism is measured and the picture has not been looked
// at. The mechanism is patches/recompone/0041 (a decode() holding the whole
// per-texel job, and the kernel that calls it per tap); a paletted texel is a CLUT
// index, so no filter can run before the lookup and none of this can be sampler
// state. Its switch is under Video with the others.
// KF2_MIPMAPS=1 adds mipmaps under it (0060): each minified texture decoded
// through its CLUT into an atlas with its own mip chain.
Kf2.Anisotropic.Configure(Environment.GetEnvironmentVariable("KF2_ANISO"),
                          Environment.GetEnvironmentVariable("KF2_ANISO_PROBE"),
                          Environment.GetEnvironmentVariable("KF2_MIPMAPS"));
Kf2.Anisotropic.Install();

// Per-pixel lighting: the depth cue and the models' light evaluated per pixel from
// what PolyAssembler recorded about each packet, instead of interpolated between
// the corner colours. Off by default until the picture has been judged. GL core
// backend only; the runtime half is patches/recompone/0048.
//     KF2_PERPIXEL=1          on
//     KF2_PERPIXEL_PROBE=1    packets recorded, polygons lit per pixel and not
Kf2.PerPixelLighting.Configure(Environment.GetEnvironmentVariable("KF2_PERPIXEL"),
                               Environment.GetEnvironmentVariable("KF2_PERPIXEL_PROBE"));
Kf2.PerPixelLighting.Install();

// Even fog and lighting: clipped map tiles fogged on the tiles' curve instead of
// the emitter's half (func_800302E8), and fog and light colour blended across the
// tile edges where one light record meets another. Off by default: the steps are
// the game's own.
//     KF2_EVENFOG=1           on
//     KF2_EVENFOG_BLEND=0     no fog blend
//     KF2_EVENLIGHT=0         no light blend
Kf2.EvenFog.Configure(Environment.GetEnvironmentVariable("KF2_EVENFOG"),
                      Environment.GetEnvironmentVariable("KF2_EVENFOG_BLEND"),
                      Environment.GetEnvironmentVariable("KF2_EVENLIGHT"));
Kf2.EvenFog.Install();

// Voice interpolation and reverb (patches/recompone/0043); see docs/AUDIO.md.
//
//     KF2_SPU_INTERP=gauss|cubic|sinc      KF2_REVERB=legacy|hardware|enhanced
//     KF2_AUDIO_PROBE=1                    KF2_AUDIO_DUMP=dir
Kf2.AudioQuality.Configure(Environment.GetEnvironmentVariable("KF2_SPU_INTERP"),
                           Environment.GetEnvironmentVariable("KF2_REVERB"));
Kf2.AudioQuality.Install();
Kf2.AudioProbe.Configure(Environment.GetEnvironmentVariable("KF2_AUDIO_PROBE"),
                         Environment.GetEnvironmentVariable("KF2_AUDIO_DUMP"));
Kf2.AudioProbe.Install();

// Positional sound effects: the game pans a 3D sound once at key-on, folded front
// to back; this keeps each one aimed at its source while it plays, and renders it
// for speakers or headphones (SpuSpatialVoice). See "Positional audio" in docs/AUDIO.md.
//
//     KF2_POSAUDIO=off|speakers|headphones  KF2_POSAUDIO_PROBE=1
Kf2.PositionalAudio.Configure(Environment.GetEnvironmentVariable("KF2_POSAUDIO"),
                              Environment.GetEnvironmentVariable("KF2_POSAUDIO_PROBE"));
Kf2.PositionalAudio.Install();

// The render scale survives a menu. A modal sub-loop -- the in-game menu, a shop,
// an NPC's message box -- keeps the world behind it by reading the finished frame
// out of VRAM once and blitting it back at the head of every iteration, and that
// roundtrip goes through the console's own resolution: the restore stamped a 1x
// picture over the game's own 320 columns every frame, leaving the widescreen
// margin (which never goes through VRAM) at full scale beside it. The GL backend
// keeps a scaled copy of every readback and serves an upload of the same pixels
// from it instead:
//
//     KF2_VRAMSNAP=0        off, the 1x upload the game asked for
//     KF2_VRAMSNAP_PROBE=1  restores served against uploads that missed
//
// Content-keyed, so anything the game actually built in RAM -- a texture, an MDEC
// frame, a decoded sprite -- fails the compare and uploads as it always did.
RecompOne.Runtime.Hle.GlVram.Snapshots =
    Environment.GetEnvironmentVariable("KF2_VRAMSNAP") != "0";
RecompOne.Runtime.Hle.GlVram.SnapshotProbe =
    Environment.GetEnvironmentVariable("KF2_VRAMSNAP_PROBE") == "1";
if (!RecompOne.Runtime.Hle.GlVram.Snapshots)
    Console.WriteLine("[KF2] vram snapshots: off (a menu restores the frame at 1x)");

// Which vblank timeline VSync runs on. Upstream grew its own after the pin this
// port was vendored from: Interrupts owns a wall-clock grid and VSync *blocks*
// in WaitVBlanks until the count reaches its target, which is what the hardware
// does -- and is also a hard 60 Hz ceiling on every VSync call. This port cannot
// have that ceiling: FramePacing hands FrameClock a deliberately permissive rate
// and keeps its own deadline at DrawOTag, and MenuPacing, LoadPacing and
// SpriteAnim are each measured against a VSync that returns immediately. So the
// port's own non-blocking grid (patches/recompone/0021-vblank-wall-clock) is the
// default and upstream's is the comparison:
//
//     KF2_VSYNC=block  upstream's blocking timeline; anything else keeps ours
//
// Both ship. Interrupts.PollSlow and Runtime.PresentFrame gate their own IRQ 0
// on this, so whichever timeline is chosen delivers each vblank exactly once.
RecompOne.Runtime.Sdk.LibEtc.BlockingVSync =
    Environment.GetEnvironmentVariable("KF2_VSYNC") == "block";
if (RecompOne.Runtime.Sdk.LibEtc.BlockingVSync)
    Console.WriteLine("[KF2] vsync: upstream's blocking timeline (the port's own grid is off)");

// Auto reload. One post-hook on the end of main-loop stage 3 watches for the
// player's death and reloads the last save through the game's own loader, so a
// death costs a couple of seconds instead of four screens of menu:
//
//     KF2_AUTORELOAD=1        on (the default); 0 leaves the death alone
//     KF2_AUTORELOAD_DELAY=2  seconds of the game's death sequence first -- the
//                             comparison, not a setting: 0 puts the reload inside
//                             the death animation and 10 leaves the player long
//                             enough to reach the menu this patch exists to skip
//     KF2_AUTORELOAD_SLOT=0   0 = the game's own "last used" slot, 1..3 pins one
//
// A patch rather than a mod because it answers a design of the original that a
// package should not have to be present to fix; its settings are under Gameplay.
Kf2.AutoReload.Configure(Environment.GetEnvironmentVariable("KF2_AUTORELOAD"),
                         Environment.GetEnvironmentVariable("KF2_AUTORELOAD_DELAY"),
                         Environment.GetEnvironmentVariable("KF2_AUTORELOAD_SLOT"));
Kf2.AutoReload.Install();

// The map. King's Field is a maze, the original shipped no automap, and every
// other thing in this port that knows where you are is a debug instrument. The
// floor plan is already in RAM and is not polygon soup: the area loader copies
// 64,000 bytes to 0x801C8484, an 80x80 grid of 10-byte tile records, two stacked
// 5-byte floors each, a tile spanning 2048 world units. So the map is a read --
// no hook, no write to game memory, and with the panels closed it costs a bool
// test a frame:
//
//     KF2_MAP=0             the whole feature off (on by default)
//     KF2_MAP_MINIMAP=1     the corner minimap on (off by default)
//     KF2_MAP_PROBE=1       dump the 80x80 grid as ASCII on each area load
//     KF2_MAP_PAUSE=0       leave the world running while the full map is up
//                           (it pauses by default)
//
// Three comparisons for what are no longer settings. The map is drawn the way
// the game's own map screen is, with no height ramp and no wall tint on it: the
// argument for that picture is that it belongs, so offering the debugger's one
// beside it asks the player to settle what the port is for. The blueprint is
// still the picture the fog, the extents and the markers were judged against,
// so it stays reachable:
//
//     KF2_MAP_STYLE=blueprint   the port's original blueprint plan
//     KF2_MAP_SHADE=1           colour each tile by its height byte
//     KF2_MAP_WALLS=1           tint the tiles whose +4 bit 0x80 is set
//     KF2_MAP_FLOOR=lower       pin a stacked half instead of following the player
//     KF2_MAP_ARROW=1           the player as an arrow with a heading, not a dot
//
// KF2_MAP_MINIMAP and KF2_MAP_PAUSE are in that list now too. The minimap's
// picture has never been judged by eye, which is the port's standing reason for a
// default and a poor reason for a settings block, so its six knobs went with its
// checkbox and N toggles it for the session. The pause is not a preference: the
// full map is a screen with no chrome and no input, opened in a corridor.
//
// A patch rather than a mod for auto reload's reason -- it is something the port
// itself should offer, so it should not be able to be absent -- and its knobs are
// under Gameplay beside it. The minimap defaults off because the mechanism is
// measured and the picture is not.
Kf2.Map.Configure(Environment.GetEnvironmentVariable("KF2_MAP"),
                  Environment.GetEnvironmentVariable("KF2_MAP_MINIMAP"),
                  Environment.GetEnvironmentVariable("KF2_MAP_PROBE"),
                  Environment.GetEnvironmentVariable("KF2_MAP_PAUSE"),
                  Environment.GetEnvironmentVariable("KF2_MAP_STYLE"),
                  Environment.GetEnvironmentVariable("KF2_MAP_SHADE"),
                  Environment.GetEnvironmentVariable("KF2_MAP_WALLS"),
                  Environment.GetEnvironmentVariable("KF2_MAP_FLOOR"),
                  Environment.GetEnvironmentVariable("KF2_MAP_ARROW"));

// The other half of that map: what is *in* the area rather than what shape it is.
// The four world tables the renderer walks -- creatures, props, effects,
// billboards -- read at their own liveness tests and drawn where the game says
// they stand:
//
//     KF2_MAP_MARKERS=1     the marker layer on (off, and no longer a setting)
//
// **Off, and the save points are the exception rather than part of it.** A live
// read of where every creature, prop, effect and torch is standing is an
// instrument, not a map, and King's Field's difficulty is not knowing what is
// round the corner. A save point is the opposite case -- the one object in a
// maze a player wants a map to find, and the game names it itself (definition
// kind 0x0E) -- so it is drawn always and independently, which is why
// MapMarkers.Refresh stands down only when both are off.
// KF2_MAP_PROBE=1 also dumps a per-table census and a histogram of the object
// table's type byte, which is what would let those types be paired with nouns.
Kf2.MapMarkers.Configure(Environment.GetEnvironmentVariable("KF2_MAP_MARKERS"));
Kf2.Map.Install();

// Fog of war for that map: the tiles the player has actually seen, remembered per
// save slot and kept between sessions in a file beside the memory card. It is the
// game's own 24x24 visibility grid at 0x80192EAC -- occlusion already flooded --
// ORed into an 80x80 bitset, sampled on the vblank rather than through a hook, so
// it accumulates with the map closed and at the same 60 Hz whatever the render
// rate is:
//
//     KF2_MAP_FOG=1         fog on for the run (off by default)
//     KF2_MAP_FOG_PROBE=1   a line a second: tiles seen, lit, rejected, flushes
//     KF2_MAP_FOG_LOS=0     the line-of-sight gate off (on by default)
//
// The cull grid is a *culling* test, so it is allowed to over-report and does:
// its flood lights a cell when either of its two ring parents is lit, which
// spreads 45 degrees a ring and paints rooms through the wall beside a doorway.
// A cell is therefore checked against a recursive symmetric shadowcast out of
// the player's own tile before it is written, and only the intersection is
// remembered.
//
// Off by default for the sub-pixel reason: the mechanism is measured and the
// picture has never been looked at.
Kf2.MapFog.Configure(Environment.GetEnvironmentVariable("KF2_MAP_FOG"),
                     Environment.GetEnvironmentVariable("KF2_MAP_FOG_PROBE"),
                     Environment.GetEnvironmentVariable("KF2_MAP_FOG_LOS"));
Kf2.MapFog.Install();

// Auto start, and the agent beacon -- the pair that lets an automated tester get
// into the game and know it got there. Scripted input cannot drive the boot menus
// (KF2_AUTOPAD's clock only starts once an area has loaded), and screenshots are
// off the table, so an agent left at the title waits on nothing:
//
//     KF2_AUTOSTART=2   load slot 1..3 through the game's own loader at boot,
//                       skipping the title and Continue menus (off unless set);
//                       =new stops in the New Game instead
//     KF2_AGENT=1       emit [KF2-AGENT] stdout lines -- an overlay transition on
//                       each load and a JSON state snapshot ~1/s, whose inGame
//                       field is how a program tells "stuck at title" from "in an
//                       area" (off unless set)
//
// Patches for the KF2_AUTOPAD reason: they must work from an environment variable
// with no package to enable.
Kf2.AutoStart.Configure(Environment.GetEnvironmentVariable("KF2_AUTOSTART"));
Kf2.AutoStart.Install();
Kf2.AgentBeacon.Configure(Environment.GetEnvironmentVariable("KF2_AGENT"));
Kf2.AgentBeacon.Install();

// The command channel -- the half that lets an agent act instead of only watch.
// A small TCP server on loopback, off unless KF2_SHELL is set:
//
//     KF2_SHELL=1   line protocol on 127.0.0.1:27900 (or =<port>): one request
//                   per line, one single-line JSON response back --
//                   state | load <slot> | warp <area> | press <button> [ms] | kill
//
// Commands arrive on socket threads and are marshalled onto the game thread
// through two drainers -- a VSync listener for the cheap ones and a post hook on
// main-loop stage 3 for load/warp, whose loader waits on the CD by looping VSync,
// so running it inside the VSync event would nest VSync inside itself and swap
// overlays under a live frame. Patches for the KF2_AUTOPAD reason; see "The
// command channel" in docs/PATCHES_AND_MODS.md.
Kf2.AgentServer.Configure(Environment.GetEnvironmentVariable("KF2_SHELL"));
Kf2.AgentServer.Install();

// Analog twin-stick control. Two pre-hooks on the game's own turn/look and
// walk/strafe routines pre-load the velocities those routines are about to
// accumulate, so the sticks pick the amount and the game's own movement code
// still applies it:
//
//     KF2_ANALOG=1                on (the default); 0 hands the sticks back
//     KF2_ANALOG_TURN/PITCH/MOVE  sensitivities; DEADZONE, CURVE, ACCEL* the feel
//     KF2_ANALOG_INVERTY=1        look-Y inversion, and INVERTTURN/STRAFE/FWD
//     KF2_ANALOG_PROBE=1          the control-state report
//
// A patch rather than a mod because a pad without it has its left stick wired to
// the D-pad, which in this game turns instead of walking — working sticks are not
// a taste a player should have to find a package to fix. Configure reads its own
// environment here rather than being handed the strings: there are eighteen of
// them and they are listed above its own class. Its settings are under Input.
Kf2.Analog.Configure();
Kf2.Analog.Install();

// Mouse look, and the mouse buttons. The look half is not a hook of its own: a
// mouse is another way of choosing the same per-frame turn and pitch step, so it
// is spent inside Analog's look hook and runs even with the sticks handed back.
// The buttons take the other route, through PadReadEvent, so they are pressed as
// pad buttons at the moment the game reads the pad -- which is what makes them
// follow the game's own control-config screen and work in its menus:
//
//     KF2_MOUSE=1                              on; off by default
//     KF2_MOUSE_TURN/LOOK=1.0                  sensitivities; INVERTY=1 flips look Y
//     KF2_MOUSE_BUTTONS=Square,Triangle,Cross  left, right, middle, as pad buttons
//     KF2_MOUSE_KEY=Escape                     the key that captures and releases
//
// Off by default, unlike the sticks: nothing is broken about playing this with the
// keyboard, and a pointer that vanishes into the game unasked is worse than one
// switch to find. Its settings sit under Input beside the stick ones.
Kf2.Mouse.Configure();
Kf2.Mouse.Install();

// The menu pointer: hover an in-game menu item and the game's own cursor moves to
// it, left click confirms, right click backs out.
//
//     KF2_MENUMOUSE=0        off -- the menu is pad and keyboard only again
//     KF2_MENUMOUSE_PROBE=1  the layout table, the pointer's row, and what it did
//
// Independent of mouse look and on by default, because it needs no capture: a
// player who never locks the pointer still has one, and pointing it at a menu is
// the one thing a mouse can do here without being locked to the window first. It
// drives the cursor stepper's return value rather than the pad, which is not a
// preference -- injected Up/Down is already recorded as not moving func_8001EA14
// at all, and the menu never reads the pad word Analog writes. Hover only takes
// the cursor once the pointer has moved, and hands it back the moment the pad
// moves it, so nothing about playing with a pad changes.
Kf2.MenuMouse.Configure(Environment.GetEnvironmentVariable("KF2_MENUMOUSE"),
                        Environment.GetEnvironmentVariable("KF2_MENUMOUSE_PROBE"));
Kf2.MenuMouse.Install();

// Widescreen. The runtime already renders a margin either side of the display
// buffer and presents the whole thing at Display.WideAspect, so setting that one
// number is the entire hookup; the replacement of DrawOTag here is only for the
// HUD, which is drawn in screen space and would otherwise sit inset from the new
// edges:
//
//     KF2_WIDESCREEN=16:9       aspect for the run; "1.777" and "off" also parse
//     KF2_WIDESCREEN_PROBE=1    the margin census, on the console
//     KF2_WIDESCREEN_EFFECTS=0  leave the death fade and the damage flash 320 wide
//     KF2_WIDESCREEN_HUD=1      anchor the HP/MP panel and the icons to the new
//                               edges -- off, and no longer a setting: it is the
//                               one thing widescreen does that moves something the
//                               game placed deliberately, and where it lands has
//                               never been looked at by eye
//
// A patch rather than a mod because an aspect ratio is a picture the port should
// be able to offer without a package having to load, and Video is where a player
// looks for it. It still defaults to 4:3: the census says a quarter of every frame
// in an area crosses the screen edge and is there to recover, but the two ways it
// can go wrong -- a 2D screen the game draws 320 wide, and per-object culling
// against the game's own 4:3 frustum -- are invisible to a primitive counter and
// have never been checked by eye.
Kf2.Widescreen.Configure(Environment.GetEnvironmentVariable("KF2_WIDESCREEN"),
                         Environment.GetEnvironmentVariable("KF2_WIDESCREEN_PROBE"),
                         Environment.GetEnvironmentVariable("KF2_WIDESCREEN_EFFECTS"),
                         Environment.GetEnvironmentVariable("KF2_WIDESCREEN_HUD"));
Kf2.Widescreen.Install();

// Present-path census. Counts, per two-second window, what PresentDisplay
// picked: the widened render target, a plain one, or a fallback to raw VRAM.
// The fallback presents at 4:3, so a high fallback rate is the flashing margin
// -- see "Widescreen" in docs/WIDESCREEN.md and patches/recompone/0022:
//
//     KF2_PRESENT_PROBE=1    the census, on the console
//     KF2_PRESENT_PROBE=2    also name the verdict, and every live target, the
//                            frame the verdict changes -- which is the only frame
//                            that says *why* the picture dropped to 4:3
{
    var pp = Environment.GetEnvironmentVariable("KF2_PRESENT_PROBE");
    if (pp == "1" || pp == "2") RecompOne.Runtime.Hle.GpuHle.PresentProbe = true;
    if (pp == "2") RecompOne.Runtime.Hle.GpuHle.PresentVerdictProbe = true;
}

// The cull the margin runs into. King's Field decides what to draw from a 24x24
// grid of tile visibility, rebuilt each frame as a top-down trapezoid whose corners
// are seven s16 pairs in GAME.EXE's data -- the 4:3 frustum, flattened onto the
// map. Widening the picture without widening that leaves the sides showing only
// what the game happened to overdraw, so this scales the trapezoid's four lateral
// corners by the same ratio the margin widens by, and fixes the game's scanline
// fill so a cone that now leaves the 24x24 window does not lose whole ranks of
// tiles:
//
//     KF2_WIDESCREEN_CULL=0        leave the cone at its 4:3 shape
//     KF2_WIDESCREEN_CULL=1.5      pin a widening factor instead of the aspect's
//     KF2_WIDESCREEN_CULL_PROBE=1  the cone's shape and what the grid clipped
//     KF2_CULL_RESCUE_RADIUS=N     Chebyshev radius of the force-lit disc around
//                                  the camera (default 3, 0 disables) — the
//                                  occlusion flood's eye sits five tiles ahead
//                                  of the camera, so near-camera tiles can be
//                                  shadowed by walls that do not block the eye
Kf2.CullCone.Configure(Environment.GetEnvironmentVariable("KF2_WIDESCREEN_CULL"),
                       Environment.GetEnvironmentVariable("KF2_WIDESCREEN_CULL_PROBE"),
                       Environment.GetEnvironmentVariable("KF2_CULL_RESCUE_RADIUS"));
Kf2.CullCone.Install();

// The rest of the third cull: the 24x24 window itself, which fits the shipped
// cone exactly and so truncates the widened one at some yaw -- a straight line
// cut in world space at the screen edges. This rebuilds the whole visibility
// pipeline at 32x32 (build, point query, box query; the legacy array gets our
// central 24x24 crop), transcribed cell for cell from the generated code:
//
//     KF2_CULLGRID=shadow        run ours after the stock build, change nothing
//     KF2_CULLGRID=on            replace the build and both queries with ours
//     KF2_CULLGRID_COMPARE=1     in shadow, diff our grid against the stock one;
//                                at factor 1 the acceptance gate is zero
//                                mismatches over a whole attract session
Kf2.CullGrid.Configure(Environment.GetEnvironmentVariable("KF2_CULLGRID"),
                       Environment.GetEnvironmentVariable("KF2_CULLGRID_COMPARE"));
Kf2.CullGrid.Install();

// The frame's primitive buffers. The game hands out 0x19000 bytes a frame -- 1969
// POLY_GT4 packets -- and its assemblers abandon the rest of the frame when the bump
// passes the end, which a widened cull cone reaches. The port moves them above 2 MB
// and enlarges them; the RAM is sized for that before PSMemory is made below. See
// "The primitive buffer ran out" in docs/WIDESCREEN.md.
//
//     KF2_PRIMBUF=4           size as a multiple of the game's; 1 leaves them where they were
//     KF2_PRIMBUF_PROBE=1     peak usage, capacity and overflows, on the console
//     KF2_RAMSIZE=4           guest RAM in MB, at least what the buffers need
//     KF2_RAM_PROBE=1         with the probe, accesses above 2 MB per 64 KiB page
Kf2.PrimBuffer.Configure(Environment.GetEnvironmentVariable("KF2_PRIMBUF_PROBE"));
Kf2.PrimBuffer.Install();

// func_80030540, the polygon assembler that builds most of the world's packets, in
// C#: the same reads and stores in the same order, with the registers in locals.
// See "The polygon assembler in C#" in docs/PATCHES_AND_MODS.md.
//
//     KF2_POLYASM=0           the recompiled routine
//     KF2_POLYASM=verify      run both on every call and compare RAM, registers and the GTE
//     KF2_POLYASM_REJECT=0    send every oversized polygon to the clipper again
//     KF2_POLYASM_REJECT=replay  a rejection also writes the clipper's scratch
//     KF2_POLYASM_UNCLIPPED=0 func_8002FECC, the far map tiles' assembler, recompiled
//     KF2_POLYASM_TRANSFORM=0 func_8002E650 and func_8002E7CC, the vertex transforms, recompiled
//     KF2_POLYASM_LIT=0       func_8002F214 and func_8002EAEC, the models' lit assembler, recompiled
//     KF2_POLYASM_CLIPPER=0   Clip4FTP and Clip3FTP, the view-space clipper, recompiled
Kf2.PolyAssembler.Configure(Environment.GetEnvironmentVariable("KF2_POLYASM"),
                            Environment.GetEnvironmentVariable("KF2_POLYASM_REJECT"),
                            Environment.GetEnvironmentVariable("KF2_POLYASM_UNCLIPPED"),
                            Environment.GetEnvironmentVariable("KF2_POLYASM_TRANSFORM"),
                            Environment.GetEnvironmentVariable("KF2_POLYASM_LIT"),
                            Environment.GetEnvironmentVariable("KF2_POLYASM_CLIPPER"));
// KF2_POLYASM_FACING=0 culls a polygon on its first three corners again, as the game
// does; see "A floor quarter missing at a short edge" in docs/RENDERING.md.
Kf2.PolyAssembler.WholeFacing = Environment.GetEnvironmentVariable("KF2_POLYASM_FACING")?.Trim() != "0";
Kf2.PolyAssembler.Install();

// The map-tile walk in C#: func_80031C94 (the 24x24 cell sweep), func_80031B1C
// (one cell's two stacked halves) and func_80031950 (one half, set up and handed to
// an assembler). Not a performance change -- the walk's own body is 0.041-0.124 ms a
// frame -- but the point at which the port learns which tile, at what world
// position, is drawn by which assembler. CullGrid already writes the grid it reads
// and PolyAssembler already owns two of its three consumers.
//
//     KF2_TILEWALK=0        all three recompiled
//     KF2_TILEWALK=verify   run both on every call and compare RAM, registers, GTE
//     KF2_TILEWALK_CELL=0   func_80031B1C recompiled
//     KF2_TILEWALK_TILE=0   func_80031950 recompiled
//     KF2_TILEWALK_PROBE=1  cells walked, halves drawn, by assembler
Kf2.TileWalk.Configure(Environment.GetEnvironmentVariable("KF2_TILEWALK"),
                       Environment.GetEnvironmentVariable("KF2_TILEWALK_CELL"),
                       Environment.GetEnvironmentVariable("KF2_TILEWALK_TILE"),
                       Environment.GetEnvironmentVariable("KF2_TILEWALK_PROBE"));
Kf2.TileWalk.Install();

// The object and creature walk in C#: func_800331B4 (the creature, object, effect
// and billboard tables) and func_80032588 (the model submitter it dispatches to).
// TileWalk taught the port the static world; this is everything that moves. Not a
// performance change -- the submitter's own body is 0.005-0.047 ms a frame -- but
// the point at which the port learns which creature, at what world position, in
// which pose, is drawn by which assembler. PolyAssembler already owns all three
// assemblers it dispatches to, and ObjectSmoothing already carries its two tables.
//
//     KF2_MODELWALK=0        both recompiled
//     KF2_MODELWALK=verify   run both on every call and compare RAM, registers, GTE
//     KF2_MODELWALK_WALK=0   func_800331B4 recompiled, the submitter still C#
//     KF2_MODELWALK_SUBMIT=0 func_80032588 recompiled, the walk still C#
//     KF2_MODELWALK_PROBE=1  what each of the four tables submitted
Kf2.ModelWalk.Configure(Environment.GetEnvironmentVariable("KF2_MODELWALK"),
                        Environment.GetEnvironmentVariable("KF2_MODELWALK_WALK"),
                        Environment.GetEnvironmentVariable("KF2_MODELWALK_SUBMIT"),
                        Environment.GetEnvironmentVariable("KF2_MODELWALK_PROBE"));
Kf2.ModelWalk.Install();

// Stage 13 itself, func_800342D8, and the camera block it opens with, func_8002E22C,
// in C#. The renderer is nineteen calls and one block of HUD arithmetic; with it here
// the frame's view is a value (Stage13.ViewOverride), the drawing half is one call
// (Stage13.DrawScene), and the compass needle's spring inside its body steps on the
// world tick rather than on every frame drawn. Every call still goes through its hooks.
//
//     KF2_STAGE13=0            the recompiled renderer
//     KF2_STAGE13=verify       the recompiled one draws; ours is replayed against a record of it
//     KF2_STAGE13_NEEDLE=0     step the compass needle every frame drawn, as the routine does
//     KF2_STAGE13_PROBE=1      frames drawn and needle steps a second
//     KF2_CAMERABLOCK=0        the recompiled camera block
//     KF2_CAMERABLOCK=verify   run both on every call and compare RAM, registers, GTE
Kf2.CameraBlock.Configure(Environment.GetEnvironmentVariable("KF2_CAMERABLOCK"));
Kf2.CameraBlock.Install();
Kf2.Stage13.Configure(Environment.GetEnvironmentVariable("KF2_STAGE13"),
                      Environment.GetEnvironmentVariable("KF2_STAGE13_NEEDLE"),
                      Environment.GetEnvironmentVariable("KF2_STAGE13_PROBE"));
Kf2.Stage13.Install();

// The game's other cull: a six-plane view-space clipper (func_8005CAC8) that only
// the near floor and ceiling are big enough to reach, set to twice the screen
// frustum as a guard band against the GPU's 1023-pixel limit. Twice the frustum is
// 320 px either side of centre, which the picture passes at about 8:3 -- and what a
// plane removes has a straight edge, which is how this was told apart from the
// per-tile cull:
//
//     KF2_VIEWCLIP=0          leave the clip volume at its 4:3 shape
//     KF2_VIEWCLIP=1.5        force a widening factor instead of the aspect's
//     KF2_VIEWCLIP_PROBE=1    what it wrote, and where the cut lands on screen
Kf2.ViewClip.Configure(Environment.GetEnvironmentVariable("KF2_VIEWCLIP"),
                       Environment.GetEnvironmentVariable("KF2_VIEWCLIP_PROBE"));
Kf2.ViewClip.Install();

// What the ending's last frame does when it is reached. END.EXE finishes in a
// spin with no VSync and never asks the boot stub for anything else, so on
// hardware "The End" is a hang you leave with the reset button:
//
//     KF2_ENDINGEXIT=0    keep that hang; by default any button returns to the title
Kf2.EndingHold.Configure(Environment.GetEnvironmentVariable("KF2_ENDINGEXIT"));

// Boot straight into one of the three executables, for a defect that lives past
// the point a session can reach by playing:
//
//     KF2_BOOTEXE=end     the ending, without finishing the game
//
// It writes the boot stub's own file-name index before its loader loop reads it.
Kf2.BootExe.Configure(Environment.GetEnvironmentVariable("KF2_BOOTEXE"));
Kf2.BootExe.Install();

// Where the patches' own settings live. A patch registers a page against one of
// the runtime's settings sections and is drawn inside it, so the frame rate is in
// System > Settings > Video beside vsync rather than in a box of its own; the one
// section the port adds itself is Gameplay, for patches that change how the game
// plays rather than how the machine behaves.
Kf2.Settings.PatchSettings.Install();

// Compile the recompiled code ahead of the game running it. QuickJit is off (a
// tier-up loses a MonoMod detour), so every function is compiled by the full JIT
// on its first call -- which for an area change is 234 methods inside one frame,
// measured at 297.87 ms of work of which 292.37 ms was the JIT. This warms the
// lot on a background thread while the title is up. Installed last, so the
// patches' own attach listeners have run before the first method is prepared.
//
//     KF2_PREJIT=0        leave every method to its first call -- the comparison
//     KF2_PREJIT_PROBE=1  a line per overlay as it is warmed
Kf2.Prejit.Configure(Environment.GetEnvironmentVariable("KF2_PREJIT"),
                     Environment.GetEnvironmentVariable("KF2_PREJIT_PROBE"));
Kf2.Prejit.Install();

// What each drawn frame's ordering table actually got, across an area crossing.
// Luminance cannot answer this -- a door into an unlit corridor is dark whether or
// not the frame is right -- so this counts the map and the models instead.
//
//     KF2_CROSSPROBE=1    a per-frame dump of tiles and models around every fdat load
Kf2.CrossProbe.Configure(Environment.GetEnvironmentVariable("KF2_CROSSPROBE"));
Kf2.CrossProbe.Install();

// What the picture held across an area change, drawn frame by drawn frame,
// and where the renderer's camera was against the player.
//
//     KF2_BLACKPROBE=1        sample the display area and dump the window around a load
//     KF2_BLACKPROBE_OUT=dir  also write the display rect around the load as PNGs
//                             (default scratch/blackprobe; 0 skips the pictures)
Kf2.BlackProbe.Configure(Environment.GetEnvironmentVariable("KF2_BLACKPROBE"),
                         Environment.GetEnvironmentVariable("KF2_BLACKPROBE_OUT"));
Kf2.BlackProbe.Install();

// What the game's state was when an unhandled exception left the recompiled code.
// For docs/TODO.md #14, which is reproducible at 165 fps but *only with no hook on
// the faulting path* -- so a diagnostic that has to be present to see it is no use
// if being present is what stops it. This adds nothing to any path the game takes:
// it reads memory once, after the fault, from the catch below.
//
//     KF2_CRASHDUMP=0     say nothing; let the exception print on its own
Kf2.CrashDump.Configure(Environment.GetEnvironmentVariable("KF2_CRASHDUMP"));

// What the window calls itself to the desktop. On Wayland this is the whole of
// how a compositor finds the icon -- see "Wayland takes the icon from the desktop
// entry" in docs/PACKAGING.md -- and it has to be set before the window is made.
RecompOne.Runtime.Runtime.AppId = "verdite2";

foreach (var icon in new[]
{
    Path.Combine("packaging", "shared", "verdite2.png"),
    Path.Combine(AppContext.BaseDirectory, "verdite2.png"),
})
{
    if (!File.Exists(icon)) continue;
    RecompOne.Runtime.Runtime.SetIcon(File.ReadAllBytes(icon));
    break;
}

// And over it, the game's own memory-card icon off the player's disc, which the
// release cannot carry. The orb above is the fallback.
//
//     KF2_ICON=orb      keep the shipped mark
//     KF2_ICON=off      no icon at all
Kf2.CardIcon.Install(args.Length > 0 ? args[0] : null);

var memory = new PSMemory(Kf2.PrimBuffer.RamSize);
try
{
    Entry.Run(memory, args.Length > 0 ? args[0] : null);
}
catch (Exception e)
{
    // Dump first, then rethrow: the runtime still prints its own trace, and the
    // state that explains it goes out immediately above it rather than being lost.
    Kf2.CrashDump.Dump(e, memory);
    throw;
}
finally
{
    // The map's fog store. It flushes on a timer and on ProcessExit, but a run
    // that ends by throwing runs neither, and the exploration since the last
    // flush is worth more than the ten lines this costs.
    Kf2.MapFog.Flush();
}
return 0;
