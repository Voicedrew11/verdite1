using RecompOne.Runtime.Cdrom;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host;

namespace Verdite.Core;

/// <summary>
/// The window icon is the game's own memory-card icon, read off the player's disc.
///
///     {Tag}_ICON=orb         the shipped mark instead
///     {Tag}_ICON=off         no icon at all
///     {Tag}_ICON=0..2        a frame of the card icon's three (the port's default otherwise)
///     {Tag}_ICON_INSTALL=0   do not write the icon into the desktop's icon theme or the shortcuts
///
/// Where the icon is on a disc is the game's finding: the port hands in a
/// <see cref="Source"/> that reads it. Decoding, scaling, the icon theme, the
/// shortcuts and the copy kept for the next run are <see cref="WindowIcon"/>,
/// <see cref="DesktopEntry"/> and <see cref="ShortcutIcon"/>. A disc that does not
/// answer leaves whatever was already set. See "The icon comes off the disc" and
/// "Wayland takes the icon from the desktop entry" in docs/PACKAGING.md.
/// </summary>
public static class CardIcon
{
    /// <summary>The three frames of a card icon, stored a row at a time: frame 0's
    /// row y, then frame 1's, then frame 2's.</summary>
    public const int Frames = 3;

    /// <summary>
    /// Reads the palette and the pixels off <paramref name="disc"/>. The pixels are
    /// the three frames' rows, <c>Frames * WindowIcon.Side / 2</c> bytes a row, so
    /// one stride serves every port. Returns null when read; otherwise why not.
    /// </summary>
    public delegate string? Source(DiscFs disc, out byte[] clut, out byte[] pixels);

    /// <param name="discPath">The disc on the command line, or null.</param>
    /// <param name="frameByDefault">The frame used when <c>{Tag}_ICON</c> names none.</param>
    /// <param name="read">The port's disc layout.</param>
    public static void Install(string? discPath, int frameByDefault, Source read)
    {
        if (WindowIcon.Frame(Frames, byDefault: frameByDefault) is not { } frame) return;

        // What an earlier run read, over the orb, until this one reads the disc.
        WindowIcon.ApplySaved();

        if (!string.IsNullOrWhiteSpace(discPath) && File.Exists(discPath))
        {
            Apply(discPath, frame, read);
            return;
        }

        // No disc on the command line: the game opened on its own. The saved disc is
        // only known once the window has loaded settings.json, and a first run's only
        // once the picker answers, both inside Entry.Run. The first overlay loads after
        // both, on the game thread, so the icon is set on the window's own; until then
        // it is the copy an earlier run kept, or the orb.
        bool done = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (done) return;
            done = true;
            var path = RecompOne.Runtime.Runtime.CdPath;
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            {
                Console.Error.WriteLine($"[{Game.Tag}] icon: no disc path to read it from; keeping the shipped mark");
                return;
            }

            GpuJobs.Run(() => Apply(path, frame, read));
        });
    }

    static void Apply(string path, int frame, Source read)
    {
        try
        {
            using var disc = DiscFs.Open(path);
            if (read(disc, out byte[] clut, out byte[] pixels) is { } why)
            {
                Console.Error.WriteLine($"[{Game.Tag}] icon: {why}; keeping the shipped mark");
                return;
            }

            WindowIcon.Apply(WindowIcon.Decode(clut, pixels, Frames * WindowIcon.Side / 2, frame));
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[{Game.Tag}] icon: {e.Message}; keeping the shipped mark");
        }
    }
}
