using System.Reflection;
using System.Runtime.Loader;
using RecompOne.Runtime;
using RecompOne.Runtime.Host.Window;
using Verdite.Launcher.Build;

namespace Verdite.Launcher;

/// <summary>
/// The shipped entry point, shared by the ports.
///
/// A port cannot ship a playable binary. generated/ is a translation of
/// FromSoftware's code and the compiled form of it is no less derived than the
/// source, so the assembly that plays the game has to be built on the machine of
/// somebody who owns the disc. This is what makes that a first launch rather than
/// a developer setup: the launcher carries the inputs to a build -- config/ and
/// the port's own C# -- and turns them, plus the player's dump, into a game.
///
/// It is also, and separately, why a release can be built at all: nothing in the
/// launcher needs the disc to compile, so CI can produce it.
///
/// The sequence is: settle where files live, teach the runtime what a valid disc
/// is, ask for one, build if we have not built this one before, and hand over.
/// </summary>
public static class Launcher
{
    /// <summary>The game being launched; set first thing in <see cref="Run"/>.</summary>
    public static LauncherGame Game { get; private set; } = null!;

    /// <summary><c>[Verdite2]</c>, the launcher's console tag.</summary>
    internal static string Tag => $"[{Game.Name}]";

    public static int Run(LauncherGame game, string[] args)
    {
        Game = game;

        // Before any output. On Windows this is a WinExe, so it starts with no
        // console and everything written to one is discarded unless it is given
        // the terminal's.
        ConsoleAttach.ToParent();

        // Say which build this is, before anything can go wrong in it. A release is
        // many commits wide, so the number alone does not identify one; Ver.Full
        // carries the commit as well and is what a bug report should quote.
        Console.WriteLine($"{Tag} {Ver.Full}");

        try
        {
            // Before anything reads a relative path. Every file the runtime owns --
            // settings.json, interface.ini, the card saves, mods/.cache -- is
            // addressed relatively and would otherwise land beside the executable,
            // which an installed build cannot write to, or in whatever directory a
            // shortcut happened to start us in.
            Paths.Prepare();

            Runtime.DiscValidator = DiscCheck.Validate;

            // Before the window is made: on Wayland the app id is how the
            // compositor finds this entry, and so the icon.
            Runtime.AppId = game.AppId;
            Runtime.Initialize($"{game.Name} {Ver.Number}");
            Localization.Merge(game.Fill(BuildProgressPopup.Strings));
            Localization.Merge(game.Fill(UpdateBadge.Strings));

            // In the background; the badge appears in the menu bar once it has an answer.
            UpdateBadge.Install();
            UpdateCheck.Start();

            var icon = Path.Combine(AppContext.BaseDirectory, game.AppId + ".png");
            if (File.Exists(icon)) Runtime.SetIcon(File.ReadAllBytes(icon));

            // The runtime's own picker: it opens a native file dialog, refuses
            // anything DiscValidator rejects, saves the accepted path, and pumps the
            // window while it waits. A player who has already chosen passes straight
            // through.
            Runtime.WaitForValidDisc();

            var discPath = Runtime.CdPath;

            // An image on the command line is the developer form -- `Verdite2
            // other.cue`, or a .chd -- and it has to be settled HERE, before the
            // build key, rather than only handed to the game at the end. The
            // recompiled dispatch tables bake absolute LBAs from one mastering, and
            // Dispatcher arms an overlay swap on a CD read hitting that exact
            // sector, so an assembly built from the saved disc and then pointed at a
            // different image silently fails to load its area modules -- which is
            // the whole failure the per-user recompile exists to avoid. Whatever is
            // played is what is keyed and built, and an argument that is not a
            // usable disc is refused now rather than after fifteen seconds of
            // building.
            if (args.Length > 0)
            {
                if (DiscCheck.Validate(args[0]) is { } problem)
                    throw new InvalidOperationException($"{args[0]}: {problem}");
                discPath = args[0];
            }

            var gameDll = Path.Combine(Paths.Builds, BuildKey.Compute(discPath), game.GameAssembly + ".dll");

            if (!File.Exists(gameDll)) BuildGame(discPath, gameDll);

            Play(gameDll, discPath);
        }
        catch (Exception e)
        {
            // Nothing above this point has a window to report into for certain, so
            // the console is the last resort. A failure inside BuildGame is reported
            // in the popup and never reaches here.
            Console.Error.WriteLine($"[{game.Name} {Ver.Full}] {e}");
            return 1;
        }

        return 0;
    }

    /// <summary>
    /// Build the game assembly, with the window alive throughout.
    ///
    /// The work runs on a worker thread and the main thread pumps, because both
    /// steps block for seconds and a window that stops pumping for seconds is a
    /// window the desktop offers to force-quit. The main thread is the one that
    /// must do the pumping: it owns the GL context.
    /// </summary>
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
                popup.Status = "verdite.build.reading";
                popup.Step = 0;

                // Not reused between runs: it is a translation of the disc, it is
                // rebuilt in about a second, and leaving megabytes of recompiled
                // game code lying around is exactly what a port does not do.
                if (Directory.Exists(generated)) Directory.Delete(generated, recursive: true);

                popup.Status = "verdite.build.translating";
                popup.Step = 1;
                Recompile.Run(discPath, generated);

                popup.Status = "verdite.build.compiling";
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
        { IsBackground = true, Name = Game.AppId + "-build" };

        work.Start();
        while (work.IsAlive) Runtime.Pump();

        if (popup.Error is null)
        {
            popup.Close();
            return;
        }

        // The build log is the file a failed first run asks the player to send, so
        // it leads with the build that wrote it.
        File.WriteAllText(Paths.BuildLog, $"{Game.Name} {Ver.Full}\n\n{log}");

        // Hold the failure on screen. Runtime.Pump exits the process itself when
        // the window is closed, so this is how the player reads the message and
        // then leaves -- there is no game to fall back to.
        while (true) Runtime.Pump();
    }

    /// <summary>
    /// Hand over to the built game.
    ///
    /// Its entry point is Program.&lt;Main&gt;$(string[]) -- the game's Program.cs is
    /// top-level statements -- and it takes the disc image as argv[0], which is
    /// what Entry.Run reads. Loading into the default context rather than a
    /// collectible one is deliberate: the game is the rest of this process's life,
    /// MonoMod detours into it, and nothing is ever unloaded.
    ///
    /// The image handed over is the one the build was keyed on, which is what makes
    /// the baked LBAs in that build correct for it.
    /// </summary>
    static void Play(string gameDll, string discPath)
    {
        var asm = AssemblyLoadContext.Default.LoadFromAssemblyPath(gameDll);
        var main = asm.EntryPoint
            ?? throw new InvalidOperationException($"{gameDll} has no entry point.");

        try { main.Invoke(null, [new[] { discPath }]); }
        catch (TargetInvocationException e) when (e.InnerException is not null)
        {
            // Unwrap, or every crash in the game is reported as a reflection
            // failure and the game's own report is buried a frame down.
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(e.InnerException).Throw();
        }
    }
}
