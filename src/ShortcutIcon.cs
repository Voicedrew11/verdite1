using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace Verdite.Core;

/// <summary>
/// The Windows half of the card icon: the shortcuts that start this install.
///
///     {Tag}_ICON_INSTALL=0    write nothing outside the game's own data directory
///
/// Explorer reads an executable's icon out of the file, and the file is the
/// release's: it cannot carry the game's art, it sits where an installed build
/// cannot write, and it is running. A shortcut, though, names its icon as a path
/// of its own. So the same sizes go into <c>icon.ico</c> in the data directory
/// (the working directory, as every file the runtime owns), and every shortcut on
/// the desktop, in the Start menu or pinned to the taskbar whose target is this
/// process, or the package's stub one level above <c>bin\</c>, is pointed at it.
/// A shortcut in an all-users folder is only rewritten when this user may write
/// there, which a per-user install's always are.
///
/// Windows only; on Linux <see cref="DesktopEntry"/> does the same through the
/// icon theme.
/// </summary>
public static class ShortcutIcon
{
    /// <summary>The icon file, relative to the data directory.</summary>
    public const string Ico = "icon.ico";

    public static void Publish(IReadOnlyList<(byte[] Rgba, int W, int H)> images)
    {
        if (!OperatingSystem.IsWindows()) return;
        if (Game.Env("ICON_INSTALL") is "0" or "off") return;

        string ico;
        bool changed;
        try
        {
            ico = Path.GetFullPath(Ico);
            changed = DesktopEntry.Write(ico, Encode(images));
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[{Game.Tag}] icon: cannot write {Ico}: {e.Message}");
            return;
        }

        var targets = Targets();
        if (targets.Count == 0) return;

        // Off the window's thread: the Start menu can hold a few hundred shortcuts.
        Task.Run(() =>
        {
            try { Retarget(ico, targets, changed); }
            catch (Exception e) { Console.Error.WriteLine($"[{Game.Tag}] icon: shortcuts: {e.Message}"); }
        });
    }

    /// <summary>
    /// What a shortcut to this game points at: the process, and for the Windows
    /// package (bin\Verdite3.exe) the stub of the same name above it, which is
    /// what the installer's shortcuts start. Nothing for a run under dotnet.
    /// </summary>
    static List<string> Targets()
    {
        var targets = new List<string>();
        var self = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(self)) return targets;
        if (Path.GetFileNameWithoutExtension(self).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            return targets;

        self = Path.GetFullPath(self);
        targets.Add(self);
        var dir = Path.GetDirectoryName(self);
        if (dir != null && Path.GetFileName(dir).Equals("bin", StringComparison.OrdinalIgnoreCase)
            && Path.GetDirectoryName(dir) is { } root)
            targets.Add(Path.Combine(root, Path.GetFileName(self)));
        return targets;
    }

    static void Retarget(string ico, List<string> targets, bool icoChanged)
    {
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var folders = new (string Dir, bool Deep)[]
        {
            (Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), false),
            (Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), false),
            (Environment.GetFolderPath(Environment.SpecialFolder.Programs), true),
            (Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), true),
            (Path.Combine(appData, "Microsoft", "Internet Explorer", "Quick Launch", "User Pinned", "TaskBar"), false),
        };

        int set = 0;
        var refused = new List<string>();
        foreach (var (dir, deep) in folders)
        {
            if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) continue;
            IEnumerable<string> links;
            try
            {
                links = Directory.EnumerateFiles(dir, "*.lnk",
                    new EnumerationOptions { RecurseSubdirectories = deep, IgnoreInaccessible = true });
            }
            catch { continue; }

            foreach (var lnk in links)
                switch (Point(lnk, ico, targets))
                {
                    case Outcome.Set:
                        set++;
                        Notify(ShcneUpdateItem, lnk);
                        break;
                    case Outcome.Refused:
                        refused.Add(lnk);
                        break;
                }
        }

        // A shortcut already on icon.ico is not saved again, and Explorer keeps the
        // picture it cached for that path: a new frame would not show until it is told.
        if (icoChanged) Notify(ShcneAssocChanged, null);

        if (set > 0)
            Console.WriteLine($"[{Game.Tag}] icon: {set} shortcut(s) now wear {Ico}");
        foreach (var lnk in refused)
            Console.Error.WriteLine($"[{Game.Tag}] icon: cannot rewrite {lnk} (an all-users shortcut; installed for every user)");
    }

    enum Outcome { Other, Already, Set, Refused }

    static Outcome Point(string lnk, string ico, List<string> targets)
    {
        object? obj = null;
        try
        {
            obj = new ShellLink();
            var link = (IShellLinkW)obj;
            var file = (IPersistFile)obj;
            file.Load(lnk, 0);

            var path = new StringBuilder(1024);
            link.GetPath(path, path.Capacity, IntPtr.Zero, 0);
            var target = path.ToString();
            if (target.Length == 0) return Outcome.Other;
            try { target = Path.GetFullPath(target); }
            catch { return Outcome.Other; }
            if (!targets.Contains(target, StringComparer.OrdinalIgnoreCase)) return Outcome.Other;

            var icon = new StringBuilder(1024);
            link.GetIconLocation(icon, icon.Capacity, out int index);
            if (index == 0 && string.Equals(icon.ToString(), ico, StringComparison.OrdinalIgnoreCase))
                return Outcome.Already;

            link.SetIconLocation(ico, 0);
            try { file.Save(lnk, true); }
            catch (UnauthorizedAccessException) { return Outcome.Refused; }
            catch (COMException e) when (e.HResult == unchecked((int)0x80070005)) { return Outcome.Refused; }
            return Outcome.Set;
        }
        catch
        {
            return Outcome.Other;
        }
        finally
        {
            if (obj != null) Marshal.FinalReleaseComObject(obj);
        }
    }

    /// <summary>
    /// An .ico of the sizes a shell draws: 32-bit DIBs up to 48 and a PNG at 256,
    /// the form Explorer reads at every size. Entry 0 of a card's palette is
    /// transparent, so the alpha channel carries it and the AND mask is empty.
    /// </summary>
    static byte[] Encode(IReadOnlyList<(byte[] Rgba, int W, int H)> images)
    {
        var entries = new List<(int W, int H, byte[] Data)>();
        foreach (var (rgba, w, h) in images)
        {
            if (w is 16 or 32 or 48) entries.Add((w, h, Dib(rgba, w, h)));
            else if (w == 256) entries.Add((w, h, DesktopEntry.Png(rgba, w, h)));
        }

        using var ms = new MemoryStream();
        using var o = new BinaryWriter(ms);
        o.Write((ushort)0);
        o.Write((ushort)1);
        o.Write((ushort)entries.Count);
        int offset = 6 + 16 * entries.Count;
        foreach (var (w, h, data) in entries)
        {
            o.Write((byte)(w >= 256 ? 0 : w));
            o.Write((byte)(h >= 256 ? 0 : h));
            o.Write((byte)0);
            o.Write((byte)0);
            o.Write((ushort)1);
            o.Write((ushort)32);
            o.Write(data.Length);
            o.Write(offset);
            offset += data.Length;
        }

        foreach (var (_, _, data) in entries) o.Write(data);
        return ms.ToArray();
    }

    static byte[] Dib(byte[] rgba, int w, int h)
    {
        int mask = (w + 31) / 32 * 4 * h;
        using var ms = new MemoryStream();
        using var o = new BinaryWriter(ms);
        o.Write(40);
        o.Write(w);
        o.Write(h * 2);   // the colour rows and the AND mask's
        o.Write((ushort)1);
        o.Write((ushort)32);
        o.Write(0);
        o.Write(w * h * 4 + mask);
        o.Write(0);
        o.Write(0);
        o.Write(0);
        o.Write(0);
        for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++)
        {
            int s = (y * w + x) * 4;
            o.Write(rgba[s + 2]);
            o.Write(rgba[s + 1]);
            o.Write(rgba[s]);
            o.Write(rgba[s + 3]);
        }

        o.Write(new byte[mask]);
        return ms.ToArray();
    }

    const int ShcneUpdateItem = 0x00002000;
    const int ShcneAssocChanged = 0x08000000;

    static void Notify(int what, string? path)
    {
        try { SHChangeNotify(what, path == null ? 0 : 0x0005 /* SHCNF_PATHW */, path, null); }
        catch { /* only the refresh is lost */ }
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern void SHChangeNotify(int wEventId, int uFlags, string? dwItem1, string? dwItem2);

    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    class ShellLink { }

    [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellLinkW
    {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
        void GetIDList(out IntPtr ppidl);
        void SetIDList(IntPtr pidl);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
        void GetHotkey(out short pwHotkey);
        void SetHotkey(short wHotkey);
        void GetShowCmd(out int piShowCmd);
        void SetShowCmd(int iShowCmd);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, int dwReserved);
        void Resolve(IntPtr hwnd, int fFlags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
    }
}
