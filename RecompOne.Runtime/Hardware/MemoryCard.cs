using System.Text;

namespace RecompOne.Runtime.Hardware;

public sealed class MemoryCard
{
    public const int CardSize = 0x20000;
    private const int Fr = 0x80, Blk = 0x2000, Dir = 15;

    private readonly byte[] _d = new byte[CardSize];
    private readonly string _path;
    public bool Enabled = true;

    //0106. Whether this session has copied the card to `<path>.bak` yet: done
    //once, before the first write, so the backup is the card as the session
    //found it, not one frame behind the save that is being written.
    private bool _backedUp;
    private bool _failing;

    public MemoryCard(string path)
    {
        _path = path;
        if (!File.Exists(path))
        {
            _backedUp = true;
            Format();
            return;
        }

        //0106. A card the file system cut short, or one that is not a card at all,
        //used to load as whatever bytes it had with zeros after them, and the
        //next save wrote that back over it. It is kept aside as it is, and the
        //session's backup restored if there is a good one; with none, the old
        //behaviour stands -- the game sees an unformatted card and offers to
        //format it, which is the player's choice, not ours.
        var bytes = TryRead(path);
        if (bytes != null && Valid(bytes))
        {
            bytes.CopyTo(_d, 0);
            return;
        }

        var kept = IO.DurableFile.Keep(path, "damaged");
        var backup = TryRead(path + ".bak");
        if (backup != null && Valid(backup))
        {
            backup.CopyTo(_d, 0);
            _backedUp = true;
            Console.Error.WriteLine($"[Runtime] memory card {path} is damaged ({Describe(bytes)}); " +
                                    $"restored {path}.bak, damaged copy kept as {kept ?? "(none)"}");
            Flush();
            Runtime.ShowNotice($"The memory card file {Path.GetFileName(path)} was damaged, so its backup " +
                               $"from {File.GetLastWriteTime(path + ".bak"):g} was restored. " +
                               $"The damaged file was kept as {Path.GetFileName(kept ?? "(not kept)")}.");
            return;
        }

        if (bytes != null) Array.Copy(bytes, _d, Math.Min(bytes.Length, CardSize));
        Console.Error.WriteLine($"[Runtime] memory card {path} is damaged ({Describe(bytes)}) and has no good " +
                                $"backup; loaded as it is, damaged copy kept as {kept ?? "(none)"}");
        Runtime.ShowNotice($"The memory card file {Path.GetFileName(path)} is damaged and has no backup. " +
                           $"A copy was kept as {Path.GetFileName(kept ?? "(not kept)")}.");
    }

    private static bool Valid(byte[] b)
    {
        return b.Length == CardSize && b[0] == 0x4D && b[1] == 0x43;
    }

    private static string Describe(byte[]? b)
    {
        if (b == null) return "unreadable";
        return b.Length != CardSize ? $"{b.Length} bytes, not {CardSize}" : "no card header";
    }

    private static byte[]? TryRead(string path)
    {
        try
        {
            return File.Exists(path) ? File.ReadAllBytes(path) : null;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Runtime] could not read {path}: {e.Message}");
            return null;
        }
    }

    //0106. The whole card, replaced in one step and forced to the disk (DurableFile),
    //after a once-a-session copy of what was there. A failure is logged and shown
    //once, never thrown: this runs inside the game's own save, in an emulated BIOS
    //call, and the game's copy of the card in memory is still whole.
    public void Flush()
    {
        try
        {
            if (!_backedUp && File.Exists(_path))
            {
                var current = File.ReadAllBytes(_path);
                if (Valid(current)) IO.DurableFile.Write(_path + ".bak", current, sync: true);
            }

            _backedUp = true;
            IO.DurableFile.Write(_path, _d, sync: true);
            if (_failing) Console.WriteLine($"[Runtime] memory card {_path} saved again");
            _failing = false;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Runtime] could not save memory card {_path}: {e.Message}");
            if (_failing) return;
            _failing = true;
            Runtime.ShowNotice($"The memory card could not be saved to {Path.GetFileName(_path)}: {e.Message} " +
                               "The save is not on disk yet; it will be written by the next save that succeeds.");
        }
    }

    private static byte Sum(byte[] d, int o)
    {
        byte x = 0;
        for (var i = 0; i < 0x7F; i++) x ^= d[o + i];
        return x;
    }

    private void Fix(int o)
    {
        _d[o + 0x7F] = Sum(_d, o);
    }

    public void Format()
    {
        Array.Clear(_d);
        _d[0] = 0x4D;
        _d[1] = 0x43;
        Fix(0);
        for (var i = 1; i <= Dir; i++)
        {
            var o = i * Fr;
            _d[o] = 0xA0;
            _d[o + 8] = 0xFF;
            _d[o + 9] = 0xFF;
            Fix(o);
        }

        for (var i = 16; i <= 35; i++)
        {
            var o = i * Fr;
            _d[o] = _d[o + 1] = _d[o + 2] = _d[o + 3] = 0xFF;
            _d[o + 8] = 0xFF;
            _d[o + 9] = 0xFF;
            Fix(o);
        }

        Flush();
    }

    private string NameOf(int b)
    {
        int o = b * Fr + 0x0A, n = 0;
        while (n < 20 && _d[o + n] != 0) n++;
        return Encoding.ASCII.GetString(_d, o, n);
    }

    public int FileSize(int b)
    {
        return BitConverter.ToInt32(_d, b * Fr + 4);
    }

    public int Find(string name)
    {
        for (var b = 1; b <= Dir; b++)
            if (_d[b * Fr] == 0x51 && NameOf(b) == name)
                return b;
        return 0;
    }

    public int[] Chain(int first)
    {
        var list = new List<int>();
        int b = first, guard = 0;
        while (b >= 1 && b <= Dir && guard++ < Dir)
        {
            list.Add(b);
            var next = _d[b * Fr + 8] | (_d[b * Fr + 9] << 8);
            if (next == 0xFFFF) break;
            b = next + 1;
        }

        return list.ToArray();
    }

    public int Create(string name, int blocks)
    {
        if (blocks < 1) blocks = 1;
        if (Find(name) != 0) return 0;
        var free = new List<int>();
        for (var b = 1; b <= Dir && free.Count < blocks; b++)
            if (_d[b * Fr] == 0xA0)
                free.Add(b);
        if (free.Count < blocks) return 0;
        for (var i = 0; i < blocks; i++)
        {
            int b = free[i], o = b * Fr;
            for (var k = 0; k < Fr; k++) _d[o + k] = 0;
            _d[o] = (byte)(i == 0 ? 0x51 : i == blocks - 1 ? 0x53 : 0x52);
            var next = i == blocks - 1 ? 0xFFFF : free[i + 1] - 1;
            _d[o + 8] = (byte)next;
            _d[o + 9] = (byte)(next >> 8);
            if (i == 0)
            {
                var sz = blocks * Blk;
                _d[o + 4] = (byte)sz;
                _d[o + 5] = (byte)(sz >> 8);
                _d[o + 6] = (byte)(sz >> 16);
                _d[o + 7] = (byte)(sz >> 24);
                var nb = Encoding.ASCII.GetBytes(name);
                for (var k = 0; k < nb.Length && k < 20; k++) _d[o + 0x0A + k] = nb[k];
            }

            Fix(o);
            Array.Clear(_d, b * Blk, Blk);
        }

        Flush();
        return free[0];
    }

    public void FrameRead(int frame, Span<byte> dst)
    {
        if ((uint)frame < CardSize / Fr) _d.AsSpan(frame * Fr, Fr).CopyTo(dst);
    }

    public void FrameWrite(int frame, ReadOnlySpan<byte> src)
    {
        if ((uint)frame >= CardSize / Fr) return;
        src.CopyTo(_d.AsSpan(frame * Fr, Fr));
        Flush();
    }

    public byte ReadByte(int[] chain, int pos)
    {
        int bi = pos / Blk, off = pos % Blk;
        return bi < chain.Length ? _d[chain[bi] * Blk + off] : (byte)0;
    }

    public void WriteByte(int[] chain, int pos, byte v)
    {
        int bi = pos / Blk, off = pos % Blk;
        if (bi < chain.Length) _d[chain[bi] * Blk + off] = v;
    }

    public void Delete(string name)
    {
        var first = Find(name);
        if (first == 0) return;
        foreach (var b in Chain(first))
        {
            _d[b * Fr] = 0xA0;
            Fix(b * Fr);
        }

        Flush();
    }

    public List<(string name, int size)> Match(string pattern)
    {
        var r = new List<(string, int)>();
        for (var b = 1; b <= Dir; b++)
            if (_d[b * Fr] == 0x51 && Glob(pattern, NameOf(b)))
                r.Add((NameOf(b), FileSize(b)));
        return r;
    }

    private static bool Glob(string pat, string name)
    {
        if (pat.Length == 0) return true;
        int pi = 0, ni = 0;
        while (pi < pat.Length)
        {
            if (pat[pi] == '*') return true;
            if (ni >= name.Length) return false;
            if (pat[pi] != '?' && pat[pi] != name[ni]) return false;
            pi++;
            ni++;
        }

        return ni == name.Length;
    }
}