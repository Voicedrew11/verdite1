using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Verdite.Stub;

/// <summary>
/// Root-of-install launcher for a port's Windows package.
///
/// A self-contained apphost looks for its managed assembly and hostfxr next to
/// itself, so every DLL has to live in the same folder as the real launcher.
/// Putting that folder at the install root is a hundred files beside the thing
/// the player double-clicks. This process is built under the launcher's own name
/// (Verdite2.exe) and starts bin\ under that same name, is WinExe (no extra
/// console on double-click), attaches to a parent console when there is one so
/// the game's log still reaches a terminal, and waits so the child's
/// AttachConsole can join.
/// </summary>
static class Program
{
    const int AttachParentProcess = -1;

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AttachConsole(int dwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    static int Main()
    {
        try { AttachConsole(AttachParentProcess); }
        catch { /* No parent console: Explorer, a shortcut, Inno's launch. */ }

        var root = Path.GetFullPath(AppDomain.CurrentDomain.BaseDirectory)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var own = AppDomain.CurrentDomain.FriendlyName;   // Verdite2.exe
        var name = Path.GetFileNameWithoutExtension(own);
        var exe = Path.Combine(root, "bin", name + ".exe");
        if (!File.Exists(exe))
        {
            MessageBoxW(IntPtr.Zero,
                name + " cannot start because bin\\" + name + ".exe is missing.",
                name, 0x10);
            return 1;
        }

        var args = Environment.GetCommandLineArgs();
        var childArgs = new StringBuilder();
        for (var i = 1; i < args.Length; i++)
        {
            if (i > 1) childArgs.Append(' ');
            childArgs.Append(Quote(args[i]));
        }

        var psi = new ProcessStartInfo
        {
            FileName = exe,
            Arguments = childArgs.ToString(),
            UseShellExecute = false,
            WorkingDirectory = root,
        };

        using var p = Process.Start(psi);
        if (p is null) return 1;
        p.WaitForExit();
        return p.ExitCode;
    }

    static string Quote(string value)
    {
        if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '"' }) < 0)
            return value;
        return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }
}
