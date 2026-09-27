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
// It holds its buttons down through Controller.ScriptMask, which the input poll
// ANDs into the keyboard's state (0085), so an idle script never fights the
// keyboard and a held one is not undone by the poll each pad read makes first.
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

            // Through the script mask (0085), not State: the input poll a pad read
            // makes first would overwrite State before the game saw it.
            Controller.ScriptMask = (ushort)mask;
            if (mask != 0xFFFF) Controller.State &= (ushort)mask;
            if (mask != last)
            {
                Console.WriteLine($"[KF1] autopad t={t:F1}s state=0x{mask:X4}");
                last = mask;
            }
            Thread.Sleep(1);
        }
    }) { IsBackground = true, Name = "kf1-autopad" }.Start();
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
// AoWorld (the world-space term) marches King's Field II's 80x80 tile grid and
// is not ported: KF1's map format is its own.

// PGXP, upstream's vertex source (0034-0036); env only and off, as in the KF2 port.
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

// Frame pacing and view smoothing. King's Field draws at 20 fps: its frame gate
// (func_800149F4) spins on a vblank counter until three vblanks have passed. The
// gate is replaced by the same wait, which redraws the world at the target rate
// while it waits; the logic stages still run once per three vblanks on the
// game's own clock. The view is carried between the last two ticks for each
// picture, so the extra frames move. See "Frame pacing" in docs/KF1.md.
//
//     KF2_FPS=60           frames drawn a second (default 60); 20 or off is the game's own
//     KF2_FPS_PROBE=1      a line a second: drawn, redraws, ticks, time spun
//     KF2_SMOOTH=0         leave the view at the tick
//     KF2_SMOOTH_PROBE=1   a line a second: renders carried, phase, step sizes
Kf2.GateRedraw.Configure(Environment.GetEnvironmentVariable("KF2_FPS"),
                         Environment.GetEnvironmentVariable("KF2_FPS_PROBE"));
Kf2.GateRedraw.Install();
Kf2.ViewCarry.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH"),
                        Environment.GetEnvironmentVariable("KF2_SMOOTH_PROBE"));
Kf2.ViewCarry.Install();
//     KF2_SMOOTH_OBJECTS=0  leave creatures and objects at the tick
Kf2.ObjectCarry.Configure(Environment.GetEnvironmentVariable("KF2_SMOOTH_OBJECTS"),
                          Environment.GetEnvironmentVariable("KF2_SMOOTH_OBJECTS_PROBE"));
Kf2.ObjectCarry.Install();

// King's Field's main loop, stage by stage: calls, time, GTE projections and
// DrawOTag calls per stage, for deciding what frame pacing may gate.
//
//     KF2_STAGE_PROBE=1
Kf2.StageProbe.Configure(Environment.GetEnvironmentVariable("KF2_STAGE_PROBE"));
Kf2.StageProbe.Install();

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

// What the window calls itself to the desktop; on Wayland this is how a
// compositor finds the icon ("Wayland takes the icon from the desktop entry" in
// docs/PACKAGING.md), and it has to be set before the window is made.
RecompOne.Runtime.Runtime.AppId = "verdite1";

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

var memory = new PSMemory();
Entry.Run(memory, args.Length > 0 ? args[0] : null);
return 0;
