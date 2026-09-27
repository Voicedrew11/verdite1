using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Hardware;
using Recompiled;

// Entry point for the King's Field (JP, SLPS-00017) port.
//
// RecompOne generates its own Program.cs into generated/ if one is missing; this
// file takes that role instead so startup stays hand-editable. The King's Field
// II port's version is reference/kf2/Program.cs: every patch it installs is
// still compiled, and only the ones that do not depend on KF2's own addresses
// are installed here. See docs/KF1.md.

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
    Console.WriteLine($"[KF1] log channels: {string.Join(",", channels)}");
}

// PSY-Q's interrupt-callback table, per overlay (patch 0006). The runtime cannot
// derive it; it is the table libapi's InterruptCallback indexes with irq*4, and
// ResetCallback clears its 11 slots. In the 1994 library the DMA callback table
// sits *below* it (0x20 bytes), where KF2's sat above:
//
//     overlay  InterruptCallback  table
//     open     0x8002FA80         0x800423C0
//     game     0x8004FCAC         0x800642F0
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

// Scripted pad input, for reproducing a bug that needs a button press without a
// human at the keyboard:
//
//     KF2_AUTOPAD=5:Start:1000,8:Circle:200      seconds:button:holdMs
//
// The clock starts when OPEN.EXE loads (KF2_AUTOPAD_FROM=game starts it at
// GAME.EXE instead); King's Field has no area modules, which is what the KF2
// port counted from. Buttons are
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
        if (e.Name == (Environment.GetEnvironmentVariable("KF2_AUTOPAD_FROM") ?? "open")) inGame.Set();
    });

    new Thread(() =>
    {
        inGame.Wait();
        Console.WriteLine($"[KF1] autopad: {press.Count} step(s) armed");
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
                Console.WriteLine($"[KF1] autopad t={t:F1}s state=0x{mask:X4}");
                last = mask;
            }
            Thread.Sleep(1);
        }
    }) { IsBackground = true, Name = "kf1-autopad" }.Start();
}

RecompOne.Runtime.Runtime.AppId = "verdite1";

var memory = new PSMemory();
Entry.Run(memory, args.Length > 0 ? args[0] : null);
return 0;
