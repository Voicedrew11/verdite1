using System.Reflection;
using System.Runtime.Loader;
using RecompOne.Runtime;
using RecompOne.Runtime.Host.Window;
using Verdite2.Launcher;
using Verdite2.Launcher.Build;

// Verdite2 -- the shipped entry point.
//
// The port cannot ship a playable binary. generated/ is a translation of
// FromSoftware's code and the compiled form of it is no less derived than the
// source, so the assembly that plays the game has to be built on the machine of
// somebody who owns the disc. This program is what makes that a first launch
// rather than a developer setup: it carries the inputs to a build -- config/ and
// the port's own C# -- and turns them, plus the player's dump, into a game.
//
// It is also, and separately, why a release can be built at all: nothing in this
// project needs the disc to compile, so CI can produce it.
//
// The sequence is: settle where files live, teach the runtime what a valid disc
// is, ask for one, build if we have not built this one before, and hand over.

// Before any output. On Windows this is a WinExe, so it starts with no console
// and everything written to one is discarded unless it is given the terminal's.
ConsoleAttach.ToParent();

// Say which build this is, before anything can go wrong in it. A release is many
// commits wide, so the number alone does not identify one; Ver.Full carries the
// commit as well and is what a bug report should quote.
Console.WriteLine($"[Verdite2] {Ver.Full}");

try
{
    // Before anything reads a relative path. Every file the runtime owns --
    // settings.json, interface.ini, carda.sav, carda.fog, mods/.cache -- is
    // addressed relatively and would otherwise land beside the executable, which
    // an installed build cannot write to, or in whatever directory a shortcut
    // happened to start us in.
    Paths.Prepare();

    Runtime.DiscValidator = DiscCheck.Validate;

    // Before the window is made: on Wayland the app id is how the compositor
    // finds this entry, and so the icon.
    Runtime.AppId = "verdite1";
    Runtime.Initialize($"Verdite1 {Ver.Number}");
    Localization.Merge(BuildProgressPopup.Strings);

    var icon = Path.Combine(AppContext.BaseDirectory, "verdite2.png");
    if (File.Exists(icon)) Runtime.SetIcon(File.ReadAllBytes(icon));

    // The runtime's own picker: it opens a native file dialog, refuses anything
    // DiscValidator rejects, saves the accepted path, and pumps the window while
    // it waits. A player who has already chosen passes straight through.
    Runtime.WaitForValidDisc();

    var discPath = Runtime.CdPath;

    // An image on the command line is the developer form -- `Verdite2 other.cue`,
    // or a .chd -- and it has to be settled HERE, before the build key, rather than
    // only handed to the game at the end. The recompiled dispatch tables bake
    // absolute LBAs from one mastering, and Dispatcher arms an overlay swap on a
    // CD read hitting that exact sector, so an assembly built from the saved disc
    // and then pointed at a different image silently fails to load its area
    // modules -- which is the whole failure the per-user recompile exists to
    // avoid. Whatever is played is what is keyed and built, and an argument that
    // is not a usable disc is refused now rather than after fifteen seconds of
    // building.
    if (args.Length > 0)
    {
        if (DiscCheck.Validate(args[0]) is { } problem)
            throw new InvalidOperationException($"{args[0]}: {problem}");
        discPath = args[0];
    }

    var gameDll = Path.Combine(Paths.Builds, BuildKey.Compute(discPath), "KingsField2.dll");

    if (!File.Exists(gameDll)) BuildGame(discPath, gameDll);

    Play(gameDll, discPath);
}
catch (Exception e)
{
    // Nothing above this point has a window to report into for certain, so the
    // console is the last resort. A failure inside BuildGame is reported in the
    // popup and never reaches here.
    Console.Error.WriteLine($"[Verdite2 {Ver.Full}] {e}");
    return 1;
}

return 0;

// Build the game assembly, with the window alive throughout.
//
// The work runs on a worker thread and the main thread pumps, because both steps
// block for seconds and a window that stops pumping for seconds is a window the
// desktop offers to force-quit. The main thread is the one that must do the
// pumping: it owns the GL context.
static void BuildGame(string discPath, string gameDll)
{
    var popup = new BuildProgressPopup();
    PopupManager.Register(popup);
    popup.Open();

    var generated = Path.Combine(Paths.Data, "generated");
    var log = new System.Text.StringBuilder();

    var work = new Thread(() =>
    {
        try
        {
            popup.Status = "verdite2.build.reading";
            popup.Step = 0;

            // Not reused between runs: it is a translation of the disc, it is
            // rebuilt in under a second, and leaving 5 MB of recompiled game code
            // lying around is exactly what this project does not do.
            if (Directory.Exists(generated)) Directory.Delete(generated, recursive: true);

            popup.Status = "verdite2.build.translating";
            popup.Step = 1;
            Recompile.Run(discPath, generated);

            popup.Status = "verdite2.build.compiling";
            popup.Step = 2;
            GameCompile.Run(generated, gameDll);

            popup.Step = 3;
        }
        catch (Exception e)
        {
            log.AppendLine(e.ToString());
            popup.Error = e.Message;
        }
        finally
        {
            try { if (Directory.Exists(generated)) Directory.Delete(generated, recursive: true); } catch { }
        }
    })
    { IsBackground = true, Name = "verdite2-build" };

    work.Start();
    while (work.IsAlive) Runtime.Pump();

    if (popup.Error is null)
    {
        popup.Close();
        return;
    }

    // The build log is the file a failed first run asks the player to send, so it
    // leads with the build that wrote it.
    File.WriteAllText(Paths.BuildLog, $"Verdite2 {Ver.Full}\n\n{log}");

    // Hold the failure on screen. Runtime.Pump exits the process itself when the
    // window is closed, so this is how the player reads the message and then
    // leaves -- there is no game to fall back to.
    while (true) Runtime.Pump();
}

// Hand over to the built game.
//
// Its entry point is Program.<Main>$(string[]) -- Program.cs is top-level
// statements -- and it takes the disc image as argv[0], which is what Entry.Run
// reads. Loading into the default context rather than a collectible one is
// deliberate: the game is the rest of this process's life, MonoMod detours into
// it, and nothing is ever unloaded.
//
// The image handed over is the one the build was keyed on, which is what makes
// the baked LBAs in that build correct for it.
static void Play(string gameDll, string discPath)
{
    var asm = AssemblyLoadContext.Default.LoadFromAssemblyPath(gameDll);
    var main = asm.EntryPoint
        ?? throw new InvalidOperationException($"{gameDll} has no entry point.");

    try { main.Invoke(null, [new[] { discPath }]); }
    catch (TargetInvocationException e) when (e.InnerException is not null)
    {
        // Unwrap, or every crash in the game is reported as a reflection failure
        // and CrashDump's own report is buried a frame down.
        System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(e.InnerException).Throw();
    }
}
