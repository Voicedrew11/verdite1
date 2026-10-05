using RecompOne.Runtime.Memory;
using Recompiled;

// Entry point for the King's Field (SLPS-00017) port. Hand-owned, so RecompOne
// does not generate one into generated/. Init and hooks go here, before Entry.Run.

Game.Configure(tag: "KF1");

// The runtime's log channels, through an env var:
//     KF1_LOG=bios,cd,gpu,dma,sdk,spu,mdec,irq   (or KF1_LOG=all)
var channels = (Environment.GetEnvironmentVariable("KF1_LOG") ?? "")
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
    RecompOne.Runtime.Log.IrqOn = all || channels.Contains("irq");
    Console.WriteLine($"[KF1] log channels: {string.Join(",", channels)}");
}

// libapi's interrupt-callback table, per executable: the table InterruptCallback
// indexes by irq*4. See "The interrupt-callback table" in docs/RECOMPILATION.md.
RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.OverlayLoadedEvent>(e =>
{
    uint table = e.Name switch
    {
        "open" => 0x800423C0u,
        "game" => 0x800642F0u,
        _ => 0u,
    };
    if (table == 0) return;
    RecompOne.Runtime.Interrupts.CallbackTable = table;
    Console.WriteLine($"[KF1] irq callback table: {e.Name} 0x{table:X8}");
});

// The agent harness: a state beacon and a command channel. See "Driving the game
// without a person" in docs/DEVELOPMENT.md.
Kf1.AgentBeacon.Configure(Environment.GetEnvironmentVariable("KF1_AGENT"));
Kf1.AgentBeacon.Install();
Kf1.AgentServer.Configure(Environment.GetEnvironmentVariable("KF1_SHELL"));
Kf1.AgentServer.Install();
Kf1.AutoStart.Configure(Environment.GetEnvironmentVariable("KF1_AUTOSTART"));
Kf1.AutoStart.Install();

// Frame pacing: off unless KF1_FPS is set. See "Frame pacing" in docs/DEVELOPMENT.md.
Kf1.FramePacing.Configure(Environment.GetEnvironmentVariable("KF1_FPS"),
                          Environment.GetEnvironmentVariable("KF1_TICKRATE"),
                          Environment.GetEnvironmentVariable("KF1_FPS_PROBE"));
Kf1.FramePacing.Install();
Kf1.RateCensus.Install();

// The camera carried between ticks, whenever pacing is on (KF1_SMOOTH=0 compares).
// See "The camera carried" in docs/SMOOTHING.md.
Kf1.ViewSmoothing.Configure(Environment.GetEnvironmentVariable("KF1_SMOOTH"),
                            Environment.GetEnvironmentVariable("KF1_SMOOTH_PROBE"));
Kf1.ViewSmoothing.Install();

// Creatures, objects and their clip times carried between ticks, whenever pacing
// is on (KF1_SMOOTH_MODELS=0 compares). See docs/SMOOTHING.md.
Kf1.ModelSmoothing.Configure(Environment.GetEnvironmentVariable("KF1_SMOOTH_MODELS"),
                             Environment.GetEnvironmentVariable("KF1_SMOOTH_MODELS_PROBE"));
Kf1.ModelSmoothing.Install();

// The picture: 24-bit shading, no dither, perspective, sub-pixel and the Z-buffer,
// each off until judged. See docs/PICTURE.md.
Kf1.TrueColor.Configure(Environment.GetEnvironmentVariable("KF1_TRUECOLOR"));
Kf1.NoDither.Configure(Environment.GetEnvironmentVariable("KF1_NODITHER"),
                       Environment.GetEnvironmentVariable("KF1_NODITHER_PROBE"));
Kf1.NoDither.Install();
Kf1.Perspective.Configure(Environment.GetEnvironmentVariable("KF1_PERSPECTIVE"),
                          Environment.GetEnvironmentVariable("KF1_PERSPECTIVE_PROBE"));
Kf1.Perspective.Install();
Kf1.Subpixel.Configure(Environment.GetEnvironmentVariable("KF1_SUBPIXEL"),
                       Environment.GetEnvironmentVariable("KF1_SUBPIXEL_PROBE"));
Kf1.Subpixel.Install();
Kf1.ZBuffer.Configure(Environment.GetEnvironmentVariable("KF1_ZBUFFER"),
                      Environment.GetEnvironmentVariable("KF1_ZBUFFER_PROBE"));
Kf1.ZBuffer.Install();

// The Testing tab in Settings: every switch above, live.
Kf1.TestingSection.Install();

// VSync calls outside the renderer wait a real vblank, as the console's did. On by
// default; KF1_VBLANKPACING=0 compares against the runtime's clock. See "Menus wait
// for a vblank" in docs/DEVELOPMENT.md.
Kf1.VBlankPacing.Configure(Environment.GetEnvironmentVariable("KF1_VBLANKPACING"),
                           Environment.GetEnvironmentVariable("KF1_VBLANKPACING_PROBE"));
Kf1.VBlankPacing.Install();

// Scripted pad input, seconds:button:holdMs, timed from GAME.EXE's load, or from
// OPEN.EXE's with KF1_AUTOPAD_FROM=open (the title needs input to reach the game):
//     KF1_AUTOPAD=5:Start:1000,8:Circle:200
// Written through the BIOS pad read, the path that reaches the game's menus too.
var autopad = Environment.GetEnvironmentVariable("KF1_AUTOPAD");
if (!string.IsNullOrWhiteSpace(autopad))
{
    var press = new List<(double At, double Until, ushort Bit)>();
    foreach (var step in autopad.Split(',', StringSplitOptions.RemoveEmptyEntries))
    {
        var f = step.Split(':');
        if (f.Length != 3 || !Kf1.AgentServer.Buttons.TryGetValue(f[1].Trim(), out var bit))
            throw new ArgumentException($"KF1_AUTOPAD: bad step '{step}'");
        double at = double.Parse(f[0], System.Globalization.CultureInfo.InvariantCulture);
        double hold = double.Parse(f[2], System.Globalization.CultureInfo.InvariantCulture) / 1000.0;
        press.Add((at, at + hold, bit));
    }

    string from = Environment.GetEnvironmentVariable("KF1_AUTOPAD_FROM") ?? "game";
    var clock = new System.Diagnostics.Stopwatch();
    RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.OverlayLoadedEvent>(e =>
    {
        if (clock.IsRunning || e.Name != from) return;
        clock.Start();
        Console.WriteLine($"[KF1] autopad: {press.Count} step(s) armed at {from}");
    });

    ushort last = 0;
    RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.PadReadEvent>(e =>
    {
        if (e.Port != 0 || !clock.IsRunning) return;
        double t = clock.Elapsed.TotalSeconds;
        ushort held = 0;
        foreach (var (at, until, bit) in press)
            if (t >= at && t < until) held |= bit;
        if (held != last)
        {
            Console.WriteLine($"[KF1] autopad t={t:F1}s held=0x{held:X4}");
            last = held;
        }
        if (held != 0) e.Buttons &= (ushort)~(ushort)((held >> 8) | (held << 8));
    });
}

// The frame gate waits for the vblank handler without calling VSync, so the
// interrupt poll delivers the vblanks (fork 0091). See "The frame gate" in
// docs/GAME_INTERNALS.md.
RecompOne.Runtime.Sdk.LibEtc.VBlankFromPoll = true;

RecompOne.Runtime.Runtime.AppId = "verdite1";

var memory = new PSMemory();
Entry.Run(memory, args.Length > 0 ? args[0] : null);
return 0;
