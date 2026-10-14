using System.Text;

namespace RecompOne.Runtime.IO;

//0106. A player's file is replaced whole or not at all. File.WriteAllBytes and
//WriteAllText truncate the file first and then write it, so a crash, a power cut
//or a full disk between the two leaves it empty or short -- a memory card that
//loses every save, a settings file that loses every setting. Here the new bytes
//go to `<path>.tmp` and are renamed over the old file, which the file system
//does in one step: a reader sees the old file or the new one, never a part.
public static class DurableFile
{
    /// <summary>Replace <paramref name="path"/> with <paramref name="data"/> in one
    /// step. <paramref name="sync"/> also forces the bytes to the disk before the
    /// rename, so a power cut cannot leave the rename without its data; it costs
    /// a few milliseconds, which a memory card is worth and a settings file
    /// written on every slider step is not. Throws what the file system throws.</summary>
    public static void Write(string path, ReadOnlySpan<byte> data, bool sync = false)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        var tmp = full + ".tmp";
        try
        {
            using (var fs = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                fs.Write(data);
                fs.Flush(sync);
            }

            File.Move(tmp, full, overwrite: true);
        }
        catch
        {
            try
            {
                File.Delete(tmp);
            }
            catch
            {
            }

            throw;
        }
    }

    public static void WriteText(string path, string text, bool sync = false)
    {
        Write(path, Encoding.UTF8.GetBytes(text), sync);
    }

    /// <summary>As <see cref="Write"/>, but a failure is logged and returned
    /// rather than thrown: the caller is usually the game's own save, deep in an
    /// emulated BIOS call that has no way to take a host exception.</summary>
    public static bool TryWrite(string path, ReadOnlySpan<byte> data, string what, bool sync = false)
    {
        try
        {
            Write(path, data, sync);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException
                                      or System.Security.SecurityException)
        {
            Console.Error.WriteLine($"[Runtime] could not save {what} to {path}: {e.Message}");
            return false;
        }
    }

    public static bool TryWriteText(string path, string text, string what, bool sync = false)
    {
        return TryWrite(path, Encoding.UTF8.GetBytes(text), what, sync);
    }

    /// <summary>Keep a copy of a file that is about to be thrown away, beside it
    /// as `<path>.<label>-<time>`, so a damaged file a player reports can still
    /// be looked at. Returns the copy's path, or null.</summary>
    public static string? Keep(string path, string label)
    {
        try
        {
            if (!File.Exists(path)) return null;
            var copy = $"{path}.{label}-{DateTime.Now:yyyyMMdd-HHmmss}";
            File.Copy(path, copy, overwrite: true);
            return copy;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Runtime] could not keep a copy of {path}: {e.Message}");
            return null;
        }
    }
}
