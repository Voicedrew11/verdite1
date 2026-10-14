namespace Verdite.Core;

/// <summary>
/// The Wayland half of the window icon.
///
///     {Tag}_ICON_INSTALL=0    write nothing outside the game's own data directory
///
/// GLFW cannot set a window icon on Wayland — its own words are "Wayland: The
/// platform does not support setting the window icon" — because there is no
/// protocol for it that GLFW speaks. A compositor gets the icon by matching the
/// toplevel's app id to a desktop entry and reading that entry's Icon= key, so
/// the icon has to be a file in the icon theme rather than pixels on the wire.
///
/// So the same pixels go to <c>$XDG_DATA_HOME/icons/hicolor/NxN/apps/&lt;app
/// id&gt;.png</c>, one file per size, which is also what overrides an installed
/// build's icon: the shipped entry already says <c>Icon=verdite2</c> and
/// XDG_DATA_HOME outranks /usr/share, so nothing the packager wrote is touched.
/// An entry is only written when no <c>&lt;app id&gt;.desktop</c> exists
/// anywhere, which is the case for a run out of the source tree and not for an
/// installed or AppImage-integrated one; the game says what it calls itself there
/// (<see cref="Name"/>, <see cref="GenericName"/>, <see cref="Comment"/>).
///
/// Linux only; on Windows and X11 the window icon works and none of this runs.
/// See "Wayland takes the icon from the desktop entry" in Verdite2's
/// docs/PACKAGING.md.
/// </summary>
public static class DesktopEntry
{
    /// <summary>The entry's Name, the port's: <c>Verdite2</c>.</summary>
    public static string Name { get; set; } = "Verdite";

    /// <summary>The entry's GenericName, the game's: <c>King's Field</c>.</summary>
    public static string GenericName { get; set; } = "";

    /// <summary>The entry's Comment: what it is and that it needs the player's disc.</summary>
    public static string Comment { get; set; } = "";

    public static void Publish(IReadOnlyList<(byte[] Rgba, int W, int H)> images)
    {
        if (!OperatingSystem.IsLinux()) return;
        if (Game.Env("ICON_INSTALL") is "0" or "off") return;

        var id = RecompOne.Runtime.Runtime.AppId;
        if (string.IsNullOrWhiteSpace(id)) return;

        try
        {
            int written = 0;
            foreach (var (rgba, w, h) in images)
            {
                var dir = Path.Combine(DataHome, "icons", "hicolor", $"{w}x{h}", "apps");
                if (Write(Path.Combine(dir, id + ".png"), Png(rgba, w, h))) written++;
            }

            var entry = InstallEntry(id);
            if (written > 0 || entry)
                Console.WriteLine($"[{Game.Tag}] icon: {written} sizes into the icon theme" +
                                  (entry ? $", and a {id}.desktop" : "") +
                                  " (a compositor may want a relog to notice)");
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[{Game.Tag}] icon: cannot write the theme copy: {e.Message}");
        }
    }

    static string DataHome
    {
        get
        {
            var home = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            return !string.IsNullOrWhiteSpace(home) && Path.IsPathRooted(home)
                ? home
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                               ".local", "share");
        }
    }

    /// <summary>The entry is the packager's when there is one, at any precedence.</summary>
    static bool InstallEntry(string id)
    {
        var dirs = new List<string> { Path.Combine(DataHome, "applications") };
        var search = Environment.GetEnvironmentVariable("XDG_DATA_DIRS");
        foreach (var d in (string.IsNullOrWhiteSpace(search) ? "/usr/local/share:/usr/share" : search)
                 .Split(':', StringSplitOptions.RemoveEmptyEntries))
            dirs.Add(Path.Combine(d, "applications"));

        foreach (var dir in dirs)
            if (File.Exists(Path.Combine(dir, id + ".desktop")))
                return false;

        // The entry exists so the compositor has a name to resolve the icon
        // through, but it is a launcher too, so Exec has to survive this run:
        // inside an AppImage the apphost lives in a mount point that goes away,
        // and $APPIMAGE is the image itself.
        var exec = Environment.GetEnvironmentVariable("APPIMAGE");
        if (string.IsNullOrWhiteSpace(exec)) exec = Environment.ProcessPath ?? "";
        if (string.IsNullOrWhiteSpace(exec)) return false;
        var text = $"""
                    [Desktop Entry]
                    Type=Application
                    Name={Name}
                    GenericName={GenericName}
                    Comment={Comment}
                    Exec="{exec}" %f
                    Icon={id}
                    Terminal=false
                    Categories=Game;AdventureGame;
                    StartupWMClass={id}

                    """;
        return Write(Path.Combine(dirs[0], id + ".desktop"),
                     System.Text.Encoding.UTF8.GetBytes(text));
    }

    /// <summary>Writes only a file that is not already exactly this, so a boot that
    /// changes nothing touches nothing.</summary>
    internal static bool Write(string path, byte[] data)
    {
        if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(data)) return false;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, data);
        return true;
    }

    internal static byte[] Png(byte[] rgba, int w, int h)
    {
        var raw = new byte[h * (w * 4 + 1)];
        for (int y = 0; y < h; y++)
            Array.Copy(rgba, y * w * 4, raw, y * (w * 4 + 1) + 1, w * 4);

        using var ms = new MemoryStream();
        ms.Write([0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A]);

        var ihdr = new byte[13];
        Big(ihdr, 0, (uint)w);
        Big(ihdr, 4, (uint)h);
        ihdr[8] = 8;   // bits per channel
        ihdr[9] = 6;   // rgba
        Chunk(ms, "IHDR", ihdr);

        using (var z = new MemoryStream())
        {
            using (var deflate = new System.IO.Compression.ZLibStream(
                       z, System.IO.Compression.CompressionLevel.Optimal, true))
                deflate.Write(raw);
            Chunk(ms, "IDAT", z.ToArray());
        }

        Chunk(ms, "IEND", []);
        return ms.ToArray();
    }

    static void Chunk(Stream to, string tag, byte[] data)
    {
        var len = new byte[4];
        Big(len, 0, (uint)data.Length);
        to.Write(len);

        var body = new byte[4 + data.Length];
        for (int i = 0; i < 4; i++) body[i] = (byte)tag[i];
        data.CopyTo(body, 4);
        to.Write(body);

        var crc = new byte[4];
        Big(crc, 0, Crc(body));
        to.Write(crc);
    }

    static void Big(byte[] to, int at, uint v)
    {
        to[at] = (byte)(v >> 24);
        to[at + 1] = (byte)(v >> 16);
        to[at + 2] = (byte)(v >> 8);
        to[at + 3] = (byte)v;
    }

    static readonly uint[] CrcTable = BuildCrcTable();

    static uint[] BuildCrcTable()
    {
        var t = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }

        return t;
    }

    static uint Crc(byte[] data)
    {
        uint c = 0xFFFFFFFFu;
        foreach (byte b in data) c = CrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
}
