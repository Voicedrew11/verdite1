using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// A command channel for automated agents — the acting half beside the beacon's
/// watching half (see <see cref="AgentBeacon"/>):
///
///     KF2_SHELL=1     listen on 127.0.0.1:27900; KF2_SHELL=&lt;port&gt; picks another
///
/// Line protocol over TCP loopback: one request per line, one single-line JSON
/// response, always. Six commands:
///
///     state                 the beacon's snapshot as JSON
///     load &lt;slot 1..3&gt;      load a save through AutoReload.LoadSlot
///     warp &lt;area 0..7&gt;      re-enter an area through AreaWarp.TryRun
///     press &lt;button&gt; [ms]   hold a pad button for ms (default 150)
///     kill                  drop HP to zero, the way a hit would
///     nearby [radius]       live world-table records within radius of the
///                           player, nearest first (positions only; buf6's
///                           entity reading is still Inferred)
///     edit, select, set, pack, remaster, light
///                           the remaster editor's verbs (Remaster.Shell)
///     snap [hash|PATH] [after N]
///                           the presented picture (Remaster.Snap), answered
///                           from the present that reads it
///
/// Off unless KF2_SHELL is set, like every other agent switch: an unasked
/// listener is worse than one switch to find (the mouse-look precedent). A
/// socket rather than stdin because stdout already carries the beacon and the
/// [KF2] log lines — a stdio channel would make every client demultiplex its
/// responses out of the logs.
///
/// The load-bearing shape is the two marshal points. Commands arrive on socket
/// threads and must run on the game thread; where they run depends on whether
/// the command re-enters the loader:
///
///   * <c>state</c>, <c>press</c>, <c>kill</c>, <c>help</c> drain from a
///     VSyncEvent listener — the same place the beacon reads memory.
///   * <c>load</c> and <c>warp</c> drain from a post hook on main-loop stage 3.
///     func_80024154 waits on the CD by looping func_80017818, which calls
///     VSync: running them inside the VSync event nests VSync inside itself
///     and swaps overlays under a live frame — the documented death of the
///     debug panel's first warp button. Stage 3 is the game's own load path,
///     which AutoReload.Reload and AutoStart already call LoadSlot from.
///
/// <c>kill</c> is safe at VSync: AutoReload.Simulate only snapshots the CPU,
/// writes HP, calls func_8002A264 and restores — the settings page already
/// runs it from inside Present-inside-VSync.
/// </summary>
public static class AgentServer
{
    public const int DefaultPort = 27900;

    /// <summary>The listening port, or 0 when off.</summary>
    public static int Port { get; private set; }

    // End of main-loop stage 3 -- the site the game's own load path re-enters an
    // area from, and where AutoReload's and AutoStart's hooks already sit.
    const uint PlayerStage = 0x8002A550;

    const int MaxLineLength = 256;
    const int ReplyTimeoutMs = 5000;
    const int DefaultHoldMs = 150;
    const int QueueCap = 16;               // bound per queue; each heavy entry is seconds of loader time

    /// <summary>One request, routed to a queue; the reply is its single-line JSON.</summary>
    sealed record Cmd(string Name, string Arg1, string Arg2, TaskCompletionSource<string> Reply);

    // Fast: drained by the VSync listener. Heavy: drained by the stage-3 hook.
    static readonly ConcurrentQueue<Cmd> _fast = new();
    static readonly ConcurrentQueue<Cmd> _heavy = new();

    // The synthetic press. Own copy rather than AutoStart._inject, which belongs
    // to that patch's boot driver: the two must never fight over one field.
    static volatile ushort _pressBits;
    static long _pressUntil;

    // The same case-insensitive names KF2_AUTOPAD builds in Program.cs.
    static readonly Dictionary<string, ushort> Buttons = new(StringComparer.OrdinalIgnoreCase)
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
        "load <slot 1..3> - load a save through the game's own loader",
        "warp <area 0..7> - re-enter an area through the game's own entry routine",
        "press <button> [holdMs=150] - press a pad button; one press active at a time, replaced by the next",
        "kill - drop HP to zero, the way a hit would",
        "nearby [radius=8192] - live records of the world tables within radius units",
        "ending [boss|kill] - hand over to END.EXE; 'boss' runs the post-final-boss sequence, 'kill' replays the killing blow (docs/TODO.md #14)",
        "map [on|off|toggle] - the full-screen map, which pauses the world unless KF2_MAP_PAUSE=0",
        "goto <x> <y> <z> [yaw [pitch]] - put the player at a position in this area, and face yaw (0x1000 a turn) and pitch",
        "view [<x> <y> <z> <pitch> <yaw> <roll> | off] - the camera the last frame was drawn from, a digest of its cull grid and the cells it draws; with a camera, draw every frame from it until 'view off'",
        "waves [on|off | swell|swellsize|ripple|ripplesize|shade|speed <value>] - the water waves: their state, the switch, or one setting (not saved)",
    ];

    // HookManager attributes hooks to a mod so they can be removed again. This is
    // in-project rather than a loaded package, so it declares its own identity.
    static readonly ModInfo _self = new()
    {
        Id = "kf2.agentserver",
        Name = "Agent command channel",
        Version = "1.0",
        Description = "TCP loopback command channel for automated agents.",
    };

    public static void Configure(string? spec)
    {
        if (string.IsNullOrWhiteSpace(spec))
        {
            Port = 0;
            return;
        }

        var s = spec.Trim().ToLowerInvariant();
        if (s is "0" or "off") { Port = 0; return; }
        if (s is "1" or "on" or "true" or "yes") { Port = DefaultPort; return; }

        if (!int.TryParse(s, System.Globalization.NumberStyles.Integer,
                          System.Globalization.CultureInfo.InvariantCulture, out int port))
            throw new ArgumentException($"KF2_SHELL: cannot read '{spec}'");

        Port = Math.Clamp(port, 1, 65535);
    }

    public static void Install()
    {
        if (Port == 0) return;

        // Cheap commands: the VSync event fires on the game thread, the same
        // place the beacon reads memory, so no cross-thread access.
        Event.AddListener<VSyncEvent>(_ => Drain(_fast));

        // Heavy commands: attached on the first overlay load, when SymbolRegistry
        // can resolve GAME.EXE -- exactly like AutoReload.Attach.
        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached) return;
            attached = true;
            AttachHeavy();
        });

        // Inject through PAD_dr with AutoStart's exact math: the pad buffer is
        // active-low and its two button bytes are swapped against Controller's
        // layout, so a pressed Controller bit becomes a cleared swapped bit here.
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
            new Thread(AcceptLoop) { IsBackground = true, Name = "kf2-agent-server" }.Start(listener);
            Console.WriteLine($"[KF2] agent server: listening on 127.0.0.1:{Port}");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[KF2] agent server: could not listen on port {Port}: {ex.Message}");
        }
    }

    static void AttachHeavy()
    {
        SymbolRegistry.Build();
        var impl = typeof(AgentServer)
            .GetMethod(nameof(AfterPlayerStage), BindingFlags.Public | BindingFlags.Static)!;

        var target = SymbolRegistry.Resolve("game", null, PlayerStage);
        if (target == null || !HookManager.AddPost(_self, target, impl))
        {
            Console.Error.WriteLine($"[KF2] agent server: nothing hooked at game/0x{PlayerStage:X8} — " +
                                    "load/warp will time out until an area is live. " +
                                    "See \"The command channel\" in docs/PATCHES_AND_MODS.md.");
            return;
        }

        HookManager.Commit();
    }

    /// <summary>
    /// End of main-loop stage 3: the heavy drainer. Only runs once GAME.EXE is in
    /// a real area — which is precisely the guarantee load/warp need.
    /// </summary>
    public static void AfterPlayerStage(CpuContext c, IMemory m) => Drain(_heavy);

    // ---- accept / serve ----

    static void AcceptLoop(object? obj)
    {
        var listener = (TcpListener)obj!;
        while (true)
        {
            TcpClient client;
            try { client = listener.AcceptTcpClient(); }
            catch (SocketException) { continue; }         // transient; keep accepting
            catch (IOException) { continue; }
            catch (ObjectDisposedException) { return; }   // listener gone: shutting down

            new Thread(() => Serve(client)) { IsBackground = true, Name = "kf2-agent-client" }.Start();
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

                // Decoded by hand rather than StreamReader.ReadLine so the
                // length cap binds while the line arrives: ReadLine buffered
                // the whole newline-terminated line before the check could
                // run, letting a client that never terminates grow memory
                // without bound.
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
                        decoder.Convert(bytes, consumed, got - consumed,
                                        chars, 0, chars.Length, false,
                                        out int bUsed, out int cUsed, out _);
                        consumed += bUsed;
                        for (int i = 0; i < cUsed; i++)
                        {
                            char ch = chars[i];
                            if (ch is '\n' or '\r')
                            {
                                Answer(writer, line, len, over);
                                len = 0;
                                over = false;
                            }
                            else if (!over)
                            {
                                if (len == MaxLineLength) over = true;
                                else line[len++] = ch;
                            }
                            // else: discarding the tail of an over-long line
                        }
                    }
                }
            }
        }
        catch
        {
            // The client went away mid-request; the socket dies with the usings
            // above and any pending reply's TCS just never completes. The game
            // carries on regardless.
        }
    }

    /// <summary>One completed line. Empty input stays silent, as before; a
    /// line that grew past the cap answers the too-long error without the
    /// excess ever having been buffered.</summary>
    static void Answer(StreamWriter writer, char[] line, int len, bool over)
    {
        if (over)
        {
            writer.WriteLine("{\"ok\":false,\"error\":\"line too long (256 max)\"}");
            return;
        }
        var text = new string(line, 0, len).Trim();
        if (text.Length > 0) writer.WriteLine(Route(text));
    }

    // ---- routing ----

    /// <summary>Parse one request line, route it to a queue, wait for the game
    /// thread's answer. Unknown commands answer immediately.</summary>
    static string Route(string line)
    {
        var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        // goto and the remaster verbs take the whole rest of the line.
        bool whole = parts[0].Equals("goto", StringComparison.OrdinalIgnoreCase)
                     || parts[0].Equals("view", StringComparison.OrdinalIgnoreCase)
                     || parts[0].Equals("snap", StringComparison.OrdinalIgnoreCase)
                     || parts[0].Equals("waves", StringComparison.OrdinalIgnoreCase)
                     || Remaster.Shell.Verbs.Contains(parts[0].ToLowerInvariant());
        var cmd = new Cmd(parts[0].ToLowerInvariant(),
                          parts.Length > 1 ? (whole ? string.Join(' ', parts[1..]) : parts[1]) : "",
                          parts.Length > 2 ? parts[2] : "",
                          new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously));

        switch (cmd.Name)
        {
            case "state":
            case "press":
            case "kill":
            case "help":
            case "nearby":
            case "map":
            case "edit":
            case "select":
            case "set":
            case "pack":
            case "remaster":
            case "light":
            case "textures":
            case "atmos":
            case "snap":
            case "view":
            case "waves":
                Enqueue(_fast, cmd);
                break;
            case "load":
            case "warp":
            case "goto":
            case "ending":
                Enqueue(_heavy, cmd);
                break;
            default:
                return "{\"ok\":false,\"error\":" + Q($"unknown command '{cmd.Name}'; try help") + "}";
        }

        if (!cmd.Reply.Task.Wait(ReplyTimeoutMs))
        {
            return cmd.Name is "load" or "warp"
                ? "{\"ok\":false,\"error\":"
                  + Q("timed out after 5s — stage 3 never ran (are you in an area?)")
                  + ",\"cmd\":" + Q(cmd.Name) + "}"
                : "{\"ok\":false,\"error\":" + Q("timed out after 5s")
                  + ",\"cmd\":" + Q(cmd.Name) + "}";
        }

        return cmd.Reply.Task.Result;
    }

    /// <summary>The game-thread drainer: execute everything queued, reply to each.</summary>
    static void Drain(ConcurrentQueue<Cmd> queue)
    {
        while (queue.TryDequeue(out var cmd))
        {
            if (cmd.Name == "snap")
            {
                // Answered by the present that is read, not here.
                Remaster.Snap.Run(cmd.Arg1, r => cmd.Reply.TrySetResult(r));
                continue;
            }
            string reply;
            try { reply = Execute(cmd); }
            catch (Exception ex)
            {
                reply = "{\"ok\":false,\"cmd\":" + Q(cmd.Name) + ",\"error\":" + Q(ex.Message) + "}";
            }
            cmd.Reply.TrySetResult(reply);
        }
    }

    /// <summary>Bound the queues: every queued heavy command is seconds of
    /// serial loader time once stage 3 runs again, and an unbounded backlog
    /// would let one client schedule minutes of warps ahead.</summary>
    static void Enqueue(ConcurrentQueue<Cmd> queue, Cmd cmd)
    {
        if (queue.Count >= QueueCap)
        {
            cmd.Reply.TrySetResult(
                Err($"too many queued '{cmd.Name}' commands ({QueueCap} max)"));
            return;
        }
        queue.Enqueue(cmd);
    }

    static string Execute(Cmd cmd) => cmd.Name switch
    {
        "state" => DoState(),
        "press" => DoPress(cmd.Arg1, cmd.Arg2),
        "kill" => DoKill(),
        "help" => DoHelp(),
        "load" => DoLoad(cmd.Arg1),
        "warp" => DoWarp(cmd.Arg1),
        "nearby" => DoNearby(cmd.Arg1),
        "ending" => DoEnding(cmd.Arg1),
        "map" => DoMap(cmd.Arg1),
        "goto" => DoGoto(cmd.Arg1),
        "view" => DoView(cmd.Arg1),
        "waves" => Waves.Shell(cmd.Arg1),
        "edit" or "select" or "set" or "pack" or "remaster" or "light" or "textures" or "atmos" => Remaster.Shell.Run(cmd.Name, cmd.Arg1),
        _ => Err($"unknown command '{cmd.Name}'; try help"),
    };

    // ---- commands ----

    static string DoState()
    {
        if (RecompOne.Runtime.Runtime.Mem == null) return Err("not running");
        return "{\"ok\":true,\"cmd\":\"state\",\"state\":" + AgentBeacon.Snapshot() + "}";
    }

    /// <summary>
    /// Open or close the full-screen map.
    ///
    /// **On the fast queue, and that is the whole reason it is here.** The fast
    /// queue drains on <c>VSyncEvent</c> -- a vblank, which arrives whatever the
    /// world is doing -- while the heavy one drains on <c>PadReadEvent</c>, which
    /// is stage 3 and is therefore *not running* while the map has the world
    /// paused. A map verb on the heavy queue could open the map and then never
    /// close it.
    ///
    /// It exists because a pause is a thing only a person at the keyboard could
    /// switch on before: M and the pad button are the only two ways in, and
    /// neither is reachable from a headless run. With it, `map on` followed by the
    /// tick rate on the KF2_FPS_PROBE line is the measurement that says whether
    /// the world actually stopped.
    /// </summary>
    static string DoMap(string arg)
    {
        if (!Map.Enabled) return Err("the map is off (KF2_MAP=0 or the Gameplay switch)");

        bool open = arg switch
        {
            "" or "toggle" => !MapFullscreen.Instance.IsOpen,
            "on" or "1"    => true,
            "off" or "0"   => false,
            _              => throw new ArgumentException($"map: expected on, off or toggle, got '{arg}'"),
        };

        MapFullscreen.Instance.IsOpen = open;
        return "{\"ok\":true,\"cmd\":\"map\",\"open\":" + (open ? "true" : "false") +
               ",\"pauses\":" + (Map.Pause ? "true" : "false") + "}";
    }

    // ---- world perception (nearby) ----
    //
    // Both maps are the ones patches/AreaWarp.cs uses for centroid placement,
    // and the debug mod carried before it. The object table is documented in
    // docs/GAME_INTERNALS.md; buf6's entity reading is Inferred, not confirmed
    // (docs/TODO.md still calls that table unmapped), so records are reported
    // as positions tagged by table -- never named as enemies.
    const uint PlayerMaxHp = 0x80199426;   // u16; nonzero only while an area is up
    const uint PlayerPosX  = 0x801994EC;   // s32
    const uint PlayerPosY  = 0x801994F0;   // s32, height
    const uint PlayerPosZ  = 0x801994F4;   // s32

    const uint ObjectTable = 0x80177714;
    const int ObjectStride = 0x44;
    const int ObjectCount = 0x18C;
    const int ObjectEmptyOff = 0x4;        // byte == 0xFF when the slot is free
    const int ObjectPosOff = 0x14;         // VECTOR
    const uint EntityTable = 0x8016C544;   // buf6
    const int EntityStride = 0x7C;
    const int EntityCount = 0xC8;
    const int EntityEmptyOff = 0x0;
    const int EntityPosOff = 0x2C;         // VECTOR

    const int NearbyDefaultRadius = 8192;  // four tiles
    const int NearbyMaxRadius = 0x10000;
    const int NearbyListCap = 16;

    /// <summary>
    /// Live records of both world tables within a horizontal radius of the
    /// player, nearest first. This is the agent's world feedback: positions of
    /// whatever the tables hold, with distances, so navigation can be closed
    /// loop without claiming to know what any record is.
    /// </summary>
    static string DoNearby(string radiusArg)
    {
        int radius = NearbyDefaultRadius;
        if (radiusArg.Length > 0 &&
            (!int.TryParse(radiusArg, out radius) || radius < 1 || radius > NearbyMaxRadius))
            return Err($"radius must be 1..{NearbyMaxRadius}");

        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null || m.ReadU16(PlayerMaxHp) == 0) return Err("no area running");

        int px = (int)m.ReadU32(PlayerPosX);
        int py = (int)m.ReadU32(PlayerPosY);
        int pz = (int)m.ReadU32(PlayerPosZ);

        var sb = new StringBuilder("{\"ok\":true,\"cmd\":\"nearby\",\"radius\":")
            .Append(radius)
            .Append(",\"pos\":[")
            .Append(px).Append(',').Append(py).Append(',').Append(pz).Append(']');

        AppendNearby(sb, m, "objects", ObjectTable, ObjectStride, ObjectCount,
                     ObjectEmptyOff, ObjectPosOff, px, pz, radius);
        AppendNearby(sb, m, "entities", EntityTable, EntityStride, EntityCount,
                     EntityEmptyOff, EntityPosOff, px, pz, radius);
        sb.Append('}');
        return sb.ToString();
    }

    static void AppendNearby(StringBuilder sb, IMemory m, string name,
                             uint table, int stride, int count,
                             int emptyOff, int posOff,
                             int px, int pz, int radius)
    {
        var hits = new List<(long D2, int I, int X, int Y, int Z)>();
        long r2 = (long)radius * radius;
        int live = 0;
        for (int i = 0; i < count; i++)
        {
            uint rec = table + (uint)(i * stride);
            if (m.ReadU8(rec + (uint)emptyOff) == 0xFF) continue;
            live++;
            int x = (int)m.ReadU32(rec + (uint)posOff);
            int y = (int)m.ReadU32(rec + (uint)(posOff + 4));
            int z = (int)m.ReadU32(rec + (uint)(posOff + 8));
            long dx = x - px, dz = z - pz;
            long d2 = dx * dx + dz * dz;
            if (d2 > r2) continue;
            hits.Add((d2, i, x, y, z));
        }
        hits.Sort((a, b) => a.D2.CompareTo(b.D2));

        sb.Append(",\"").Append(name).Append("\":{\"total\":").Append(live)
          .Append(",\"within\":").Append(hits.Count).Append(",\"items\":[");
        for (int n = 0; n < hits.Count && n < NearbyListCap; n++)
        {
            var h = hits[n];
            if (n > 0) sb.Append(',');
            sb.Append("{\"i\":").Append(h.I)
              .Append(",\"pos\":[").Append(h.X).Append(',').Append(h.Y).Append(',').Append(h.Z).Append(']')
              .Append(",\"dist\":").Append((int)Math.Sqrt(h.D2))
              .Append('}');
        }
        sb.Append("]}");
    }

    static string DoPress(string name, string msArg)
    {
        if (!Buttons.TryGetValue(name, out ushort bits))
            return Err($"no such button '{name}' (Cross, Start, Select, Circle, Square, Triangle, " +
                       "L1-R3, Up, Down, Left, Right)");

        long hold = DefaultHoldMs;
        if (msArg.Length > 0 &&
            (!long.TryParse(msArg, out hold) || hold < 1 || hold > 5000))
            return Err("holdMs must be 1..5000");

        // One synthetic press at a time: a later press replaces bits and deadline.
        _pressBits = bits;
        _pressUntil = Environment.TickCount64 + hold;

        return "{\"ok\":true,\"cmd\":\"press\",\"button\":" + Q(name) + ",\"holdMs\":" + hold + "}";
    }

    static string DoKill()
    {
        AutoReload.Simulate();
        return "{\"ok\":true,\"cmd\":\"kill\",\"status\":" + Q(AutoReload.Status) + "}";
    }

    static string DoHelp()
    {
        var sb = new StringBuilder("{\"ok\":true,\"cmd\":\"help\",\"commands\":[");
        var all = HelpCommands.Concat(Remaster.Shell.Help).Append(Remaster.Snap.Usage).ToArray();
        for (int i = 0; i < all.Length; i++)
        {
            if (i > 0) sb.Append(',');
            sb.Append(Q(all[i]));
        }
        sb.Append("]}");
        return sb.ToString();
    }

    static string DoLoad(string slotArg)
    {
        if (!int.TryParse(slotArg, out int slot) || slot < 1 || slot > 3)
            return Err("slot must be 1..3");

        var c = RecompOne.Runtime.Runtime.Cpu;
        var m = RecompOne.Runtime.Runtime.Mem;
        if (c == null || m == null) return Err("not running");

        uint result = AutoReload.LoadSlot(c, m, (byte)slot, out uint area);
        return result switch
        {
            0 => "{\"ok\":true,\"cmd\":\"load\",\"slot\":" + slot + ",\"area\":" + area + "}",
            1 => Err("no such save file"),
            2 => Err("checksum failed"),
            _ => Err($"load failed ({result})"),
        };
    }

    static string DoWarp(string areaArg)
    {
        if (!int.TryParse(areaArg, out int area))
            return Err("usage: warp <area 0..7>");

        var c = RecompOne.Runtime.Runtime.Cpu;
        var m = RecompOne.Runtime.Runtime.Mem;
        if (c == null || m == null) return Err("not running");

        string? err = AreaWarp.TryRun(c, m, area);
        return err != null ? Err(err) : ("{\"ok\":true,\"cmd\":\"warp\",\"area\":" + area + "}");
    }

    const uint BaseYaw = 0x8019950E;   // s16; the composed yaw at 0x80199506 follows it

    /// <summary>Drained after stage 3, so the next tick walks on from the new position.</summary>
    static string DoGoto(string args)
    {
        var a = args.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (a.Length is < 3 or > 5 || !int.TryParse(a[0], out int x) || !int.TryParse(a[1], out int y)
            || !int.TryParse(a[2], out int z) || (a.Length >= 4 && !int.TryParse(a[3], out _))
            || (a.Length == 5 && !int.TryParse(a[4], out _)))
            return Err("usage: goto <x> <y> <z> [yaw [pitch]]");
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return Err("not running");
        m.WriteU32(PlayerPosX, (uint)x);
        m.WriteU32(PlayerPosY, (uint)y);
        m.WriteU32(PlayerPosZ, (uint)z);
        if (a.Length >= 4) m.WriteU16(BaseYaw, (ushort)(int.Parse(a[3]) & 0xFFF));
        if (a.Length == 5) m.WriteU16(Analog.Pitch, (ushort)int.Parse(a[4]));
        return "{\"ok\":true,\"cmd\":\"goto\",\"pos\":[" + x + "," + y + "," + z + "]}";
    }

    const uint GridOffsetX = 0x80192E98, GridOffsetZ = 0x80192E9C;   // words
    const uint Grid = 0x80192EAC, GridCells = 24 * 24;                  // what the tile walk reads

    /// <summary>
    /// The camera the last frame was drawn from, and a digest of the cull grid built
    /// from it: the grid's two offset words and its 576 cells. With a camera, every
    /// frame is drawn from it (<see cref="Stage13.ViewOverride"/>) until <c>view off</c>.
    /// Two frames drawn from one camera, wherever the player stands, should give one
    /// digest if the grid depends on the eye alone.
    /// </summary>
    static string DoView(string args)
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return Err("not running");
        var a = args.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (a.Length == 1 && a[0].Equals("off", StringComparison.OrdinalIgnoreCase))
            Stage13.ViewOverride = null;
        else if (a.Length == 6 && a.All(t => int.TryParse(t, out _)))
        {
            var n = Array.ConvertAll(a, int.Parse);
            Stage13.ViewOverride = new Camera(n[0], n[1], n[2], (short)n[3], (short)n[4], (short)n[5]);
        }
        else if (a.Length != 0)
            return Err("usage: view [<x> <y> <z> <pitch> <yaw> <roll> | off]");

        var cam = Camera.Read(m);
        ulong hash = 14695981039346656037UL;
        void Mix(uint b) { hash ^= b; hash *= 1099511628211UL; }
        Mix(m.ReadU32(GridOffsetX));
        Mix(m.ReadU32(GridOffsetZ));
        int drawn = 0;
        for (uint i = 0; i < GridCells; i++)
        {
            uint cell = m.ReadU8(Grid + i);
            if ((cell & 3u) != 0) drawn++;   // bit 0 the lower half, bit 1 the upper
            Mix(cell);
        }
        return "{\"ok\":true,\"cmd\":\"view\",\"camera\":[" +
               $"{cam.X},{cam.Y},{cam.Z},{cam.Pitch},{cam.Yaw},{cam.Roll}]," +
               $"\"override\":{(Stage13.ViewOverride != null ? "true" : "false")}," +
               $"\"grid\":\"{hash:x16}\",\"drawn\":{drawn}}}";
    }

    // ---- the ending ----
    //
    // The game's own hand-over, which no session can otherwise reach without
    // finishing the game -- and the two moments the port has been reported to
    // hang at live on either side of it.
    //
    // GAME.EXE's main loop tests a word right after stage 13: nonzero means
    // "stop", and which nonzero it is chooses what runs next. 1 writes the boot
    // stub's file-name index 2 (END.EXE) and 9 writes index 0 (OPEN.EXE, i.e.
    // quit to title); the loop then returns, and the stub -- which Execs an
    // executable as a call and loops -- loads the chosen file. See BootExe for
    // the loader's own side of that.
    //
    //   ending        write the word, and nothing else. The hand-over on its own.
    //   ending boss   run the post-boss sequence that normally writes it:
    //                 func_8019F688 in fdat23 (area 7), which fdat23's boss
    //                 damage handler func_8019FA2C calls when the boss's HP word
    //                 at rec+0x1A reaches zero. It is two modal loops that
    //                 present their own frames -- LoopPacing's territory -- and
    //                 it ends by writing the quit word itself.
    //   ending kill   the killing blow's own path, which is the only way to reach
    //                 docs/TODO.md #14 without finishing the game. The game gets
    //                 there as: func_800271D0 -> func_8003A9CC -> (dispatch slot
    //                 0x48 = fdat23's func_8019FA2C) -> func_8019F474 +
    //                 func_8019F688 -> back into func_8003A9CC, which takes the
    //                 death branch and calls func_8003A490(record, 3). The middle
    //                 of that blanks u8[+0x2] to 0xFF on entity 0 and entities
    //                 6..10, so the last call resolves a descriptor 255 records
    //                 past the block and reads a live object field as a pointer.
    //                 This replays the two calls that matter, in that order.
    const uint QuitWord = 0x80199574;      // GAME.EXE; 1 = ending, 9 = title
    const uint BossSetup = 0x8019F474;     // fdat23
    const uint BossEnding = 0x8019F688;    // fdat23
    const uint BossDamageHook = 0x8019FA2C; // fdat23, dispatch slot 0x48
    const uint DeathReaction = 0x8003A490;  // GAME.EXE, the call that faults
    const uint EndingRan = 0x801B30A2;      // func_8019F474 sets it; the hook's gate
    const uint PhaseTwoRan = 0x801B30A6;    // func_8019F1B0 sets it
    const uint BossHp = EntityTable + 0x1Au;

    static string DoEnding(string modeArg)
    {
        var c = RecompOne.Runtime.Runtime.Cpu;
        var m = RecompOne.Runtime.Runtime.Mem;
        if (c == null || m == null) return Err("not running");

        string mode = modeArg.Trim().ToLowerInvariant();
        if (mode.Length == 0)
        {
            m.WriteU32(QuitWord, 1u);
            return "{\"ok\":true,\"cmd\":\"ending\",\"how\":\"quitword\"}";
        }

        if (mode == "kill") return DoEndingKill(c, m);
        if (mode != "boss") return Err("usage: ending [boss|kill]");

        // Both, in the caller's order. func_8019F474 is what fills the pointer at
        // 0x801A0598 that func_8019F688 writes the camera through, so calling the
        // sequence alone dereferences whatever the module's data happens to hold
        // (measured: "unmapped address: 0x0145505B").
        var setup = SymbolRegistry.Resolve("fdat23", null, BossSetup);
        var fn = SymbolRegistry.Resolve("fdat23", null, BossEnding);
        if (setup == null || fn == null)
            return Err("fdat23 is not loaded — warp to area 7 first");

        var prepare = setup.CreateDelegate<Action<CpuContext, IMemory>>();
        var run = fn.CreateDelegate<Action<CpuContext, IMemory>>();
        var saved = c.Snapshot();
        try { prepare(c, m); run(c, m); }
        finally { c.Restore(saved); }

        return "{\"ok\":true,\"cmd\":\"ending\",\"how\":\"boss\"}";
    }

    /// <summary>
    /// The final blow, as the game delivers it. Preconditions are set the way the
    /// fight leaves them -- the phase-two transition already done, the ending not
    /// yet run, and the boss on its last point of HP -- so the hook takes its
    /// ending branch rather than the top-up branch that survives the first kill.
    /// Then the death reaction `func_8003A9CC` would make next is made, which is
    /// the call that reads the type byte fdat23 has just blanked.
    /// </summary>
    static string DoEndingKill(CpuContext c, IMemory m)
    {
        var hook = SymbolRegistry.Resolve("fdat23", null, BossDamageHook);
        var death = SymbolRegistry.Resolve("game", null, DeathReaction);
        if (hook == null) return Err("fdat23 is not loaded — warp to area 7 first");
        if (death == null) return Err($"no game function at 0x{DeathReaction:X8}");

        m.WriteU8(EndingRan, 0);
        m.WriteU8(PhaseTwoRan, 1);
        m.WriteU16(BossHp, 1);

        var damage = hook.CreateDelegate<Action<CpuContext, IMemory>>();
        var react = death.CreateDelegate<Action<CpuContext, IMemory>>();
        var saved = c.Snapshot();
        int typeAfter;
        try
        {
            c.A0 = EntityTable;
            c.A1 = 1u;
            damage(c, m);

            typeAfter = m.ReadU8(EntityTable + 2u);

            // Printed rather than only returned: the two modal loops above take
            // longer than the reply timeout, so the JSON never reaches the client.
            uint desc = 0x80172624u + (uint)typeAfter * 120u;
            var words = new StringBuilder();
            for (int k = 0; k < 15; k++)
                words.Append($" {m.ReadU32(desc + 0x38u + (uint)(k * 4)):X8}");
            Console.WriteLine($"[KF2] ending kill: entity 0 type is now {typeAfter}, " +
                              $"descriptor 0x{desc:X8}, pointers:{words}");

            c.A0 = EntityTable;
            c.A1 = 3u;
            react(c, m);
            Console.WriteLine("[KF2] ending kill: the death reaction returned without faulting.");
        }
        finally { c.Restore(saved); }

        return "{\"ok\":true,\"cmd\":\"ending\",\"how\":\"kill\",\"typeAfter\":" + typeAfter + "}";
    }

    // ---- JSON helpers ----

    static string Err(string message) =>
        "{\"ok\":false,\"error\":" + Q(message) + "}";

    /// <summary>A double-quoted, escaped JSON string. Every response goes through
    /// this for anything not produced by this file, so user input can never end a
    /// string early.</summary>
    static string Q(string s)
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
