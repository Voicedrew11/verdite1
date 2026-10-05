using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using RecompOne.Runtime;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;

namespace Kf1;

/// <summary>
/// The command channel: KF1_SHELL=1 listens on 127.0.0.1:27901 (KF1_SHELL=&lt;port&gt;
/// picks another). One request per line, one single-line JSON reply. Verdite3's
/// AgentServer, with this game's verbs. Commands run on the game thread, drained on
/// VSync. See "Driving the game without a person" in docs/DEVELOPMENT.md.
/// </summary>
public static class AgentServer
{
    public const int DefaultPort = 27901;

    public static int Port { get; private set; }

    const int MaxLineLength = 256;
    const int ReplyTimeoutMs = 5000;
    const int DefaultHoldMs = 150;
    const int QueueCap = 16;

    sealed record Cmd(string Name, string Arg1, string Arg2, TaskCompletionSource<string> Reply)
    {
        public string[] Args { get; init; } = [];
    }

    static readonly ConcurrentQueue<Cmd> _fast = new();

    static volatile ushort _pressBits;
    static long _pressUntil;

    public static readonly Dictionary<string, ushort> Buttons = new(StringComparer.OrdinalIgnoreCase)
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

    static readonly string[] HelpCommands =
    [
        "state - the player/area snapshot as JSON",
        "press <button> [holdMs=150] - press a pad button; one press at a time, replaced by the next",
        "peek <addr> [bytes=16] - read guest memory, hex",
        "dump <file> - write the 2 MB of guest RAM to a file",
        "poke <addr> <hex bytes> - write guest memory (a diagnostic: it moves the game's own state)",
        "aspect [4:3|16:9|16:10|21:9|<ratio>] - the widescreen aspect, or the current one",
    ];

    public static void Configure(string? spec)
    {
        Port = 0;
        if (string.IsNullOrWhiteSpace(spec)) return;
        var s = spec.Trim().ToLowerInvariant();
        if (s is "0" or "off") return;
        if (s is "1" or "on" or "true" or "yes") { Port = DefaultPort; return; }
        if (!int.TryParse(s, out int port))
            throw new ArgumentException($"KF1_SHELL: cannot read '{spec}'");
        Port = Math.Clamp(port, 1, 65535);
    }

    public static void Install()
    {
        if (Port == 0) return;

        Event.AddListener<VSyncEvent>(_ => Drain(_fast));

        // PAD_dr's buffer is active-low with its two button bytes swapped against
        // Controller's layout, as in Verdite Core's Mouse.
        Event.AddListener<PadReadEvent>(e =>
        {
            if (e.Port != 0) return;
            ushort bits = _pressBits;
            if (bits != 0 && Environment.TickCount64 < _pressUntil)
                e.Buttons &= (ushort)~(ushort)((bits >> 8) | (bits << 8));
        });

        try
        {
            var listener = new TcpListener(IPAddress.Loopback, Port);
            listener.Start();
            new Thread(AcceptLoop) { IsBackground = true, Name = "kf1-agent-server" }.Start(listener);
            Console.WriteLine($"[KF1] agent server: listening on 127.0.0.1:{Port}");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[KF1] agent server: could not listen on port {Port}: {ex.Message}");
        }
    }

    static void AcceptLoop(object? obj)
    {
        var listener = (TcpListener)obj!;
        while (true)
        {
            TcpClient client;
            try { client = listener.AcceptTcpClient(); }
            catch (SocketException) { continue; }
            catch (IOException) { continue; }
            catch (ObjectDisposedException) { return; }
            new Thread(() => Serve(client)) { IsBackground = true, Name = "kf1-agent-client" }.Start();
        }
    }

    static void Serve(TcpClient client)
    {
        try
        {
            using (client)
            {
                Stream stream = client.GetStream();
                using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

                // Decoded by hand so the length cap binds while the line arrives.
                var decoder = Encoding.UTF8.GetDecoder();
                var bytes = new byte[256];
                var chars = new char[256];
                var line = new char[MaxLineLength];
                int len = 0;
                bool over = false;

                while (stream.Read(bytes, 0, bytes.Length) is int got && got > 0)
                {
                    int consumed = 0;
                    while (consumed < got)
                    {
                        decoder.Convert(bytes, consumed, got - consumed, chars, 0, chars.Length, false,
                                        out int bUsed, out int cUsed, out _);
                        consumed += bUsed;
                        for (int i = 0; i < cUsed; i++)
                        {
                            char ch = chars[i];
                            if (ch is '\n' or '\r')
                            {
                                if (over) writer.WriteLine(Err("line too long (256 max)"));
                                else if (new string(line, 0, len).Trim() is { Length: > 0 } text) writer.WriteLine(Route(text));
                                len = 0;
                                over = false;
                            }
                            else if (!over)
                            {
                                if (len == MaxLineLength) over = true;
                                else line[len++] = ch;
                            }
                        }
                    }
                }
            }
        }
        catch
        {
            // The client went away; the game carries on.
        }
    }

    static string Route(string line)
    {
        var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var cmd = new Cmd(parts[0].ToLowerInvariant(),
                          parts.Length > 1 ? parts[1] : "",
                          parts.Length > 2 ? parts[2] : "",
                          new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously))
        { Args = parts[1..] };

        switch (cmd.Name)
        {
            case "state" or "press" or "help" or "peek" or "dump" or "poke" or "aspect":
                Enqueue(_fast, cmd);
                break;
            default:
                return Err($"unknown command '{cmd.Name}'; try help");
        }

        if (!cmd.Reply.Task.Wait(ReplyTimeoutMs))
            return Err("timed out after 5s");
        return cmd.Reply.Task.Result;
    }

    static void Drain(ConcurrentQueue<Cmd> queue)
    {
        while (queue.TryDequeue(out var cmd))
        {
            string reply;
            try { reply = Execute(cmd); }
            catch (Exception ex) { reply = "{\"ok\":false,\"cmd\":" + Q(cmd.Name) + ",\"error\":" + Q(ex.Message) + "}"; }
            cmd.Reply.TrySetResult(reply);
        }
    }

    static void Enqueue(ConcurrentQueue<Cmd> queue, Cmd cmd)
    {
        if (queue.Count >= QueueCap)
        {
            cmd.Reply.TrySetResult(Err($"too many queued '{cmd.Name}' commands ({QueueCap} max)"));
            return;
        }
        queue.Enqueue(cmd);
    }

    static string Execute(Cmd cmd) => cmd.Name switch
    {
        "state" => "{\"ok\":true,\"cmd\":\"state\",\"state\":" + AgentBeacon.Snapshot() + "}",
        "press" => DoPress(cmd.Arg1, cmd.Arg2),
        "help" => "{\"ok\":true,\"cmd\":\"help\",\"commands\":[" + string.Join(',', HelpCommands.Select(Q)) + "]}",
        "peek" => DoPeek(cmd.Arg1, cmd.Arg2),
        "dump" => DoDump(cmd.Arg1),
        "poke" => DoPoke(cmd.Arg1, cmd.Arg2),
        "aspect" => Widescreen.Shell(cmd.Arg1),
        _ => Err($"unknown command '{cmd.Name}'; try help"),
    };

    static string DoPress(string name, string msArg)
    {
        if (!Buttons.TryGetValue(name, out ushort bits))
            return Err($"no such button '{name}'");
        long hold = DefaultHoldMs;
        if (msArg.Length > 0 && (!long.TryParse(msArg, out hold) || hold < 1 || hold > 5000))
            return Err("holdMs must be 1..5000");
        _pressBits = bits;
        _pressUntil = Environment.TickCount64 + hold;
        return "{\"ok\":true,\"cmd\":\"press\",\"button\":" + Q(name) + ",\"holdMs\":" + hold + "}";
    }

    static string DoPeek(string addrArg, string lenArg)
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return Err("not running");
        if (!uint.TryParse(addrArg.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out uint addr))
            return Err("peek <hex addr> [bytes]");
        int n = 16;
        if (lenArg.Length > 0 && (!int.TryParse(lenArg, out n) || n < 1 || n > 256))
            return Err("bytes must be 1..256");
        var sb = new StringBuilder();
        for (int i = 0; i < n; i++) sb.Append(m.ReadU8(addr + (uint)i).ToString("x2"));
        return "{\"ok\":true,\"cmd\":\"peek\",\"addr\":" + Q($"0x{addr:X8}") + ",\"hex\":" + Q(sb.ToString()) + "}";
    }

    static string DoPoke(string addrArg, string hex)
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return Err("not running");
        if (!uint.TryParse(addrArg.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out uint addr)
            || hex.Length == 0 || hex.Length % 2 != 0 || hex.Length > 512)
            return Err("poke <hex addr> <hex bytes>");
        byte[] bytes;
        try { bytes = Convert.FromHexString(hex); }
        catch (FormatException) { return Err("poke <hex addr> <hex bytes>"); }
        for (int i = 0; i < bytes.Length; i++) m.WriteU8(addr + (uint)i, bytes[i]);
        return "{\"ok\":true,\"cmd\":\"poke\",\"addr\":" + Q($"0x{addr:X8}") + ",\"bytes\":" + bytes.Length + "}";
    }

    static string DoDump(string path)
    {
        if (RecompOne.Runtime.Runtime.Mem is not RecompOne.Runtime.Memory.PSMemory m) return Err("not running");
        if (path.Length == 0) return Err("dump <file>");
        File.WriteAllBytes(path, m.Ram[..0x200000].ToArray());
        return "{\"ok\":true,\"cmd\":\"dump\",\"file\":" + Q(Path.GetFullPath(path)) + "}";
    }

    public static string Err(string message) => "{\"ok\":false,\"error\":" + Q(message) + "}";

    public static string Q(string s)
    {
        var sb = new StringBuilder(s.Length + 2);
        sb.Append('"');
        foreach (char ch in s)
        {
            switch (ch)
            {
                case '\"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (ch < 0x20) sb.Append("\\u").Append(((int)ch).ToString("x4"));
                    else sb.Append(ch);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }
}
