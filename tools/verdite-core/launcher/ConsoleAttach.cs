using System.Runtime.InteropServices;

namespace Verdite.Launcher;

/// <summary>
/// Give a Windows release back its standard output when it was started from a
/// terminal.
///
/// The launcher is WinExe so that double-clicking it does not also open a console
/// window behind the game. The cost of that is that a WinExe starts with no
/// console at all, so every log line, every agent beacon line and every build
/// diagnostic goes nowhere -- including for somebody running it from a terminal
/// precisely to read them, which is how a port is debugged.
///
/// ATTACH_PARENT_PROCESS is the way back: it attaches to the console of whatever
/// launched us if there is one, and fails harmlessly if there is not (a shortcut,
/// Explorer, a desktop entry). Then the standard handles have to be reopened,
/// because .NET has already cached the ones it was given, which were nothing.
///
/// Does nothing anywhere but Windows, where WinExe and Exe differ at all.
/// </summary>
static class ConsoleAttach
{
    const int AttachParentProcess = -1;

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AttachConsole(int processId);

    public static void ToParent()
    {
        if (!OperatingSystem.IsWindows()) return;

        try
        {
            if (!AttachConsole(AttachParentProcess)) return;

            var stdout = System.Console.OpenStandardOutput();
            var stderr = System.Console.OpenStandardError();
            System.Console.SetOut(new StreamWriter(stdout) { AutoFlush = true });
            System.Console.SetError(new StreamWriter(stderr) { AutoFlush = true });
        }
        catch
        {
            // A console we cannot attach to or reopen is not a reason to refuse to
            // start the game.
        }
    }
}
