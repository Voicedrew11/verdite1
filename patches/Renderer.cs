using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Stage I, the renderer <c>func_8001FDE4(VECTOR *pos, SVECTOR *rot)</c>, in C#.
///
///     KF1_RENDERER=1        this transcription (the default); 0 the recompiled routine
///     KF1_RENDERER=verify   the recompiled routine draws the frame; this one is then
///                           run against a record of it and the two compared
///
/// Twenty call sites in a fixed order, each through a delegate to its callee so every
/// hook on it still fires, with three blocks of arithmetic between them: the HUD
/// gauges (from HP and MP), the compass inputs, and the HUD models' matrix and the
/// up to six static models drawn under it. <see cref="ViewOverride"/> draws the frame
/// from a camera of the port's. <see cref="Verifier"/> records the recompiled run at
/// every call and replays this transcription against it. Verdite3's Stage15 on this
/// game's renderer; see "The renderer in C#" in docs/GAME_INTERNALS.md.
/// </summary>
public static class Renderer
{
    const uint Routine = 0x8001FDE4;

    /// <summary>The call sites, in the order the routine makes them.</summary>
    public enum Site
    {
        View,          // the camera block, from a0/a1
        FrameHead,     // flip the buffer index, clear the ordering table
        PoseMark,      // mark the pose slots in use as unclaimed
        GeomScreen,    // SetGeomScreen(200)
        Map,           // the map walk
        LightMap,      // SetLightMatrix for the HUD
        Compass,       // model 0x15, turned by the camera's yaw
        HudSprites,    // the HUD records from 0x80055C5C
        LightHud,      // SetLightMatrix
        HudSetup,      // the HUD model records at 0x80055D20
        HudRotation,   // RotMatrix of (u16[0x8009508A], 0, 0)
        HudSetRot,
        HudSetTrans,
        HudModel0,     // the record at 0x80055D20
        HudModel1,     // the record at 0x80055D2E
        HudModels,     // the four from 0x80055D3C
        Models,        // the model walk
        Arm,
        Present,       // DrawSync, VSync, PutDrawEnv, PutDispEnv, DrawOTag
        PoseSweep,     // free the pose slots nothing claimed
    }

    /// <summary>Each site's callee, and the return address its <c>jal</c> leaves in RA.</summary>
    static readonly (uint Callee, uint Return)[] Calls =
    [
        (0x8001C184, 0x8001FE00),
        (0x8001BFB8, 0x8001FE08),
        (0x800209A8, 0x8001FE10),
        (0x8004CB08, 0x8001FE18),
        (0x8001E83C, 0x8001FE20),
        (0x8004D7B4, 0x8001FE30),
        (0x8001F8B0, 0x80020134),
        (0x8001F9D4, 0x8002017C),
        (0x8004D7B4, 0x8002018C),
        (0x8001FAFC, 0x80020194),
        (0x8004E9B8, 0x800201E4),
        (0x8004D784, 0x800201EC),
        (0x8004D814, 0x800201F4),
        (0x8001E230, 0x80020238),
        (0x8001E230, 0x8002025C),
        (0x8001E230, 0x800202A8),
        (0x8001F218, 0x800202C8),
        (0x8001F798, 0x800202D0),
        (0x8001C050, 0x800202D8),
        (0x80020A98, 0x800202E0),
    ];

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;

    /// <summary>The mode as the Testing tab sets it: 0 recompiled, 1 C#, 2 verify.</summary>
    public static int Setting { get => (int)_mode; set => _mode = (Mode)Math.Clamp(value, 0, 2); }
    static bool _queued;
    static Action<CpuContext, IMemory>[]? _callees;

    /// <summary>Whether the C# routine draws (KF1_RENDERER unset or 1), so the override is read.</summary>
    public static bool InCSharp => _mode == Mode.On;

    /// <summary>How many times the On-mode routine has run.</summary>
    public static long Frames;

    /// <summary>A camera to draw the frame from instead of the one the renderer is
    /// handed, or null for the game's. Needs the C# routine; the recompiled one ignores it.</summary>
    public static Camera? ViewOverride { get; set; }

    /// <summary>The camera the main loop handed the renderer last, override or not.</summary>
    public static Camera? Handed { get; private set; }

    /// <summary>The camera the C# routine last drew with: the block after its first call.</summary>
    public static Camera? Drawn { get; private set; }

    static readonly ModInfo _self = new()
    {
        Id = "kf1.renderer",
        Name = "Renderer",
        Version = "1.0",
        Description = "func_8001FDE4, stage I, in C#.",
    };

    public static void Configure(string? mode)
    {
        _mode = mode?.Trim().ToLowerInvariant() switch
        {
            "0" or "off" => Mode.Off,
            "verify" => Mode.Verify,
            _ => Mode.On,
        };
    }

    public static void Install()
    {
        // Attached in every mode, so the Testing tab can switch it live; off runs the recompiled routine.
        HookAttach.OnOverlayLoad("renderer", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, Routine);
        if (target == null) return false;
        if (!Bind())
        {
            Console.Error.WriteLine("[KF1] renderer: a callee is not mapped; the recompiled routine stays.");
            return false;
        }
        if (!_queued)
        {
            var impl = typeof(Renderer).GetMethod(nameof(Replace), BindingFlags.NonPublic | BindingFlags.Static)!;
            _queued = HookManager.AddReplace(_self, target, impl);
            if (!_queued) return false;
        }
        HookManager.Commit();
        bool ok = HookAttach.Installed(target);
        Console.WriteLine(ok ? $"[KF1] renderer: {_mode.ToString().ToLowerInvariant()}"
                             : "[KF1] renderer: not installed");
        return ok;
    }

    /// <summary>A delegate per callee. Calling one runs the function's detour, so
    /// every hook on it fires as it does for the recompiled body's direct call.</summary>
    static bool Bind()
    {
        if (_callees != null) return true;
        if (Calls.Length != Enum.GetValues<Site>().Length) return false;
        var fns = new Action<CpuContext, IMemory>[Calls.Length];
        for (int i = 0; i < Calls.Length; i++)
        {
            var mi = SymbolRegistry.Resolve("game", null, Calls[i].Callee);
            if (mi == null) return false;
            fns[i] = mi.CreateDelegate<Action<CpuContext, IMemory>>();
        }
        _callees = fns;
        return true;
    }

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        // PGXP's RAM shadow is kept by the recompiled stores, which C# stores skip. A
        // renderer inside one being recorded is the recompiled one's too.
        if (_mode == Mode.Off || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking || m is not PSMemory mem
            || Verifier.Recording)
        {
            orig(c, m);
            return;
        }
        if (_mode == Mode.Verify) Verifier.Run(orig, c, mem);
        else
        {
            Frames++;
            Run(c, mem);
        }
    }

    /// <summary>The routine, transcribed register for register.</summary>
    static void Run(CpuContext c, PSMemory mem)
    {
        // The camera the frame would have used, put back in the block after an override
        // so nothing after the frame reads the drawn one.
        Camera? real = null;
        if (!Verifier.Replaying)
        {
            bool handedNow = c.A0 != 0u && c.A1 != 0u;
            if (handedNow) Handed = Camera.Read(mem, c.A0, c.A1);
            if (ViewOverride is { } view)
            {
                real = handedNow ? Handed : CameraBlock.Read(mem);
                CameraBlock.Store(mem, view);
                c.A0 = 0u;
                c.A1 = 0u;
            }
        }

        uint sp = c.SP - 0x50u;
        c.SP = sp;
        mem.WriteU32(sp + 0x48u, c.RA);
        mem.WriteU32(sp + 0x44u, c.S3);
        mem.WriteU32(sp + 0x40u, c.S2);
        mem.WriteU32(sp + 0x3Cu, c.S1);
        mem.WriteU32(sp + 0x38u, c.S0);

        Call(c, mem, Site.View);
        if (!Verifier.Replaying) Drawn = CameraBlock.Read(mem);
        Call(c, mem, Site.FrameHead);
        Call(c, mem, Site.PoseMark);
        c.A0 = 200u;
        Call(c, mem, Site.GeomScreen);
        Call(c, mem, Site.Map);
        c.A0 = 0x80055FE8u;
        Call(c, mem, Site.LightMap);

        Gauges(c, mem);
        Compass(c, mem);
        Call(c, mem, Site.Compass);

        // The HUD's light colours, then its sprite records.
        c.V1 = mem.ReadU16(0x80095062u);
        c.A1 = mem.ReadU16(0x80095060u);
        c.V0 = mem.ReadU8(0x80095064u);
        c.At = 0x80090000u;
        mem.WriteU16(0x8009505Au, (ushort)c.V1);
        mem.WriteU16(0x80095058u, (ushort)c.A1);
        mem.WriteU8(0x8009505Eu, (byte)c.V0);
        mem.WriteU8(0x8009505Du, (byte)c.V0);
        mem.WriteU8(0x8009505Cu, (byte)c.V0);
        c.A0 = c.S0 - 0xA8u;
        Call(c, mem, Site.HudSprites);
        c.A0 = 0x80056008u;
        Call(c, mem, Site.LightHud);
        Call(c, mem, Site.HudSetup);

        HudModels(c, mem, sp);

        Call(c, mem, Site.Models);
        Call(c, mem, Site.Arm);
        Call(c, mem, Site.Present);
        Call(c, mem, Site.PoseSweep);
        if (real is { } back) CameraBlock.Build(c, mem, back, _callees![(int)Site.View]);

        c.RA = mem.ReadU32(sp + 0x48u);
        c.S3 = mem.ReadU32(sp + 0x44u);
        c.S2 = mem.ReadU32(sp + 0x40u);
        c.S1 = mem.ReadU32(sp + 0x3Cu);
        c.S0 = mem.ReadU32(sp + 0x38u);
        c.SP = sp + 0x50u;
    }

    static void Call(CpuContext c, PSMemory mem, Site site)
    {
        c.RA = Calls[(int)site].Return;
        if (Verifier.Replaying) Verifier.Take(c, mem, site);
        else _callees![(int)site](c, mem);
    }

    /// <summary><c>div</c> as the recompiler emits it: LO and HI left alone by a zero
    /// divisor (its <c>break</c> is a no-op), and <c>at</c> left at 0x80000000 by the
    /// overflow check.</summary>
    internal static void Div(CpuContext c, uint s, uint t)
    {
        if (t != 0u)
        {
            if ((int)s == int.MinValue && (int)t == -1) { c.LO = 0x80000000u; c.HI = 0u; }
            else { c.LO = (uint)((int)s / (int)t); c.HI = (uint)((int)s % (int)t); }
        }
        c.At = 0x80000000u;
    }

    // ---- the inline blocks ---------------------------------------------------------

    // The HUD sprite records: 0xE bytes each from 0x80055C5C, +0 on, +0xA a value.
    const uint Hud = 0x80055C5Cu;

    /// <summary>The gauges: with the HUD up (<c>u8[0x800A0818] == 1</c>), the HP and MP
    /// bars' lengths from the current and maximum values, the two numbers over 100,
    /// and one of four condition marks by the bits of <c>u16[0x800A07AA]</c>.</summary>
    static void Gauges(CpuContext c, PSMemory mem)
    {
        c.T1 = 0x80055C94u;
        mem.WriteU8(c.T1, 0);
        c.At = 0x80050000u;
        mem.WriteU8(0x80055CA2u, 0);
        mem.WriteU8(0x80055CB0u, 0);
        mem.WriteU8(0x80055CBEu, 0);
        c.V1 = mem.ReadU8(0x800A0818u);
        c.V0 = 1u;
        c.A2 = 0x32u;
        if (c.V1 != c.V0)
        {
            c.At = 0x80050000u;
            foreach (uint k in OnFlags) mem.WriteU8(k, 0);
            return;
        }

        c.A1 = mem.ReadU16(0x800A0790u);
        c.V1 = c.A1 - 1u;
        Div(c, c.V1, c.A2);
        c.V1 = c.LO;
        c.V0 = mem.ReadU16(0x800A0792u);
        c.A0 = Times50(c.V0) + c.V1;
        Div(c, c.A0, c.A1);
        c.A0 = c.LO;
        c.A3 = mem.ReadU16(0x800A0794u);
        c.A1 = c.A3 - 1u;
        Div(c, c.A1, c.A2);
        c.A1 = c.LO;
        c.V0 = mem.ReadU16(0x800A0796u);
        c.V1 = Times50(c.V0) + c.A1;
        Div(c, c.V1, c.A3);
        c.V1 = c.LO;
        c.A2 = mem.ReadU16(0x800A0798u);
        c.V0 = 100u;
        Div(c, c.A2, c.V0);
        c.A2 = c.LO;
        c.A1 = mem.ReadU16(0x800A079Cu);
        Div(c, c.A1, c.V0);
        c.A1 = c.LO;
        c.T0 = mem.ReadU16(0x800A07AAu);
        c.A3 = 1u;
        c.At = 0x80050000u;
        foreach (uint k in OnFlags) mem.WriteU8(k, (byte)c.A3);
        mem.WriteU16(0x80055C66u, (ushort)c.A0);
        c.V0 = c.T0 & 1u;
        mem.WriteU16(0x80055C74u, (ushort)c.V1);
        mem.WriteU16(0x80055C82u, (ushort)c.A2);
        mem.WriteU16(0x80055C90u, (ushort)c.A1);

        // The first bit set, from 1 up, lights its mark.
        if (c.V0 != 0u) { c.V0 = c.T0 & 2u; mem.WriteU8(0x80055CBEu, (byte)c.A3); return; }
        c.V0 = c.T0 & 2u;
        if (c.V0 != 0u) { c.V0 = c.T0 & 4u; mem.WriteU8(0x80055CB0u, (byte)c.A3); return; }
        c.V0 = c.T0 & 4u;
        if (c.V0 != 0u) { c.V0 = c.T0 & 8u; mem.WriteU8(c.T1, (byte)c.A3); return; }
        c.V0 = c.T0 & 8u;
        if (c.V0 != 0u) mem.WriteU8(0x80055CA2u, (byte)c.A3);
    }

    // The gauge records' on bytes, written together.
    static readonly uint[] OnFlags =
        [0x80055C5Cu, 0x80055C6Au, 0x80055C78u, 0x80055C86u, 0x80055CCCu, 0x80055CDAu, 0x80055CE8u, 0x80055CF6u];

    /// <summary>((v * 3) * 8 + v) * 2, the shifts and adds the compiler made of v * 50.</summary>
    static uint Times50(uint v) => (((v << 1) + v << 3) + v) << 1;

    /// <summary>The compass: shown by <c>u8[0x800A0819]</c> in two records, its needle
    /// the negated yaw of the renderer's camera copy.</summary>
    static void Compass(CpuContext c, PSMemory mem)
    {
        c.V1 = mem.ReadU8(0x800A0819u);
        c.V0 = mem.ReadU16(CameraBlock.Angles + 2u);
        c.S0 = 0x80055D04u;
        mem.WriteU8(c.S0, (byte)c.V1);
        c.At = 0x80050000u;
        mem.WriteU8(0x80055D74u, (byte)c.V1);
        c.V0 = (0u - c.V0) & 0x0FFFu;
        mem.WriteU16(0x80055D86u, (ushort)c.V0);
        c.S1 = 1u;
    }

    /// <summary>The HUD models: a matrix turned by <c>u16[0x8009508A]</c> about X and
    /// set (0, 160, 200) ahead, and the static models of the records at 0x80055D20
    /// (0xE bytes, +0 on, +2 the model) drawn under it with their own light colours.</summary>
    static void HudModels(CpuContext c, PSMemory mem, uint sp)
    {
        c.V0 = 0xFFu;
        c.At = 0x80090000u;
        mem.WriteU8(0x8009505Cu, (byte)c.V0);
        mem.WriteU8(0x8009505Du, (byte)c.V0);
        mem.WriteU8(0x8009505Eu, (byte)c.V0);
        mem.WriteU32(sp + 0x24u, 0u);
        c.V0 = 0xA0u;
        mem.WriteU32(sp + 0x28u, c.V0);
        c.V0 = 0xC8u;
        mem.WriteU32(sp + 0x2Cu, c.V0);
        mem.WriteU16(sp + 0x34u, 0);
        mem.WriteU16(sp + 0x32u, 0);
        c.A0 = sp + 0x30u;
        c.V0 = mem.ReadU16(0x8009508Au);
        c.A1 = sp + 0x10u;
        mem.WriteU16(sp + 0x30u, (ushort)c.V0);
        Call(c, mem, Site.HudRotation);
        c.A0 = sp + 0x10u;
        Call(c, mem, Site.HudSetRot);
        c.A0 = sp + 0x10u;
        Call(c, mem, Site.HudSetTrans);

        c.V0 = mem.ReadU16(0x80095068u);
        c.V1 = mem.ReadU16(0x80095066u);
        c.S0 = 0x80055D20u;
        c.At = 0x80090000u;
        mem.WriteU16(0x8009505Au, (ushort)c.V0);
        mem.WriteU16(0x80095058u, (ushort)c.V1);
        c.V0 = mem.ReadU8(c.S0);
        c.A0 = c.S0 + 2u;
        if (c.V0 == c.S1)
        {
            c.A1 = 0u;
            c.A2 = 0u;
            Call(c, mem, Site.HudModel0);
        }
        c.V0 = mem.ReadU8(0x80055D2Eu);
        c.S3 = 1u;
        if (c.V0 == c.S1)
        {
            c.A0 = c.S0 + 0x10u;
            c.A1 = 0u;
            c.A2 = 0u;
            Call(c, mem, Site.HudModel1);
        }
        c.S0 = c.S0 + 0x1Cu;
        c.S1 = 3u;
        c.V0 = mem.ReadU16(0x8009506Cu);
        c.V1 = mem.ReadU16(0x8009506Au);
        c.S2 = 0xFFFFFFFFu;
        c.At = 0x80090000u;
        mem.WriteU16(0x8009505Au, (ushort)c.V0);
        mem.WriteU16(0x80095058u, (ushort)c.V1);
        while (true)
        {
            // The recompiled loop polls interrupts at its head; a replay does not.
            if (!Verifier.Replaying) RecompOne.Runtime.Interrupts.Poll(c, mem);
            c.V0 = mem.ReadU8(c.S0);
            if (c.V0 == c.S3)
            {
                c.V0 = c.S1 - 1u;
                c.A0 = c.S0 + 2u;
                c.A1 = 0u;
                c.A2 = 0u;
                Call(c, mem, Site.HudModels);
            }
            c.V0 = c.S1 - 1u;
            c.S1 = c.V0;
            c.V0 = (uint)((int)(c.V0 << 16) >> 16);
            bool more = c.V0 != c.S2;
            c.S0 = c.S0 + 0xEu;
            if (!more) break;
        }
    }

    // ---- verify --------------------------------------------------------------------

    /// <summary>
    /// The recompiled routine runs and every call it makes from its own body is
    /// recorded (registers on entry and exit, all of RAM and the scratchpad on entry
    /// and exit). This transcription is then run from the routine's entry state, each
    /// of its calls answered from the record instead of being made: the call must be
    /// the next one recorded, every register and all of RAM and the scratchpad must
    /// match the record on entry, and the recorded exit state is put back. Last, the
    /// return state is compared. A stretch of the body in which the recording's
    /// interrupt poll could have run a handler (<c>Interrupts.SlowPolls</c> moved) is
    /// not compared for RAM: the replay does not poll. **The recompiled result
    /// stands**. Verdite3's Stage15 verifier, every site compared rather than three.
    /// </summary>
    static class Verifier
    {
        const int MaxCalls = 32;

        public static bool Recording { get; private set; }
        public static bool Replaying { get; private set; }

        static bool? _hooked;

        static readonly int[] _seq = new int[MaxCalls];
        static int _nseq;
        static readonly CpuSnapshot[] _entry = new CpuSnapshot[MaxCalls], _exit = new CpuSnapshot[MaxCalls];
        static readonly byte[][] _entryRam = new byte[MaxCalls][], _exitRam = new byte[MaxCalls][];
        static readonly uint[][] _entryPad = new uint[MaxCalls][], _exitPad = new uint[MaxCalls][];
        static int _open, _nested;
        static readonly uint[] _pollsIn = new uint[MaxCalls], _pollsOut = new uint[MaxCalls];
        static uint _pollsStart, _pollsEnd;

        static byte[] _before = [], _final = [], _ours = [];
        static readonly uint[] _padBefore = new uint[Differential.PadWords], _padNow = new uint[Differential.PadWords];
        static readonly uint[] _finalPad = new uint[Differential.PadWords];
        static int _taken;

        static long _calls, _bad, _overflow, _stray, _interrupted;
        static readonly long[] _bySite = new long[Calls.Length];
        static readonly List<string> _samples = [];
        static double _reportAt;

        public static void Run(Action<CpuContext, IMemory> orig, CpuContext c, PSMemory mem)
        {
            _hooked ??= Hook();
            if (_hooked == false)
            {
                orig(c, mem);
                return;
            }

            var ram = Differential.Ram(mem);
            if (_before.Length != ram.Length)
            {
                _before = new byte[ram.Length];
                _final = new byte[ram.Length];
                _ours = new byte[ram.Length];
                for (int i = 0; i < MaxCalls; i++)
                {
                    _entryRam[i] = new byte[ram.Length];
                    _exitRam[i] = new byte[ram.Length];
                    _entryPad[i] = new uint[Differential.PadWords];
                    _exitPad[i] = new uint[Differential.PadWords];
                }
            }
            ram.CopyTo(_before);
            Differential.SavePad(mem, _padBefore);
            var entry = c.Snapshot();

            _open = -1;
            _nested = 0;
            _nseq = 0;
            _pollsStart = RecompOne.Runtime.Interrupts.SlowPolls;
            Recording = true;
            try { orig(c, mem); }
            finally { Recording = false; }
            _pollsEnd = RecompOne.Runtime.Interrupts.SlowPolls;
            ram.CopyTo(_final);
            Differential.SavePad(mem, _finalPad);
            var theirs = c.Snapshot();
            _calls++;

            _before.CopyTo(ram);
            Differential.LoadPad(mem, _padBefore);
            c.Restore(entry);
            _taken = 0;
            Replaying = true;
            try { Renderer.Run(c, mem); }
            finally { Replaying = false; }

            if (_taken != _nseq) Mismatch($"ours made {_taken} of {_nseq} recorded calls");
            var ours = c.Snapshot();
            if (Registers(ours, theirs) is { } r) Mismatch($"on return: {r}");
            if (_pollsEnd != (_nseq > 0 ? _pollsOut[_nseq - 1] : _pollsStart)) _interrupted++;
            else
            {
                if (Differential.Describe(ram, _final, 0, ram.Length) is { } diff) Mismatch($"on return: {diff}");
                Differential.SavePad(mem, _padNow);
                if (Differential.DescribePad(_padNow, _finalPad) is { } pd) Mismatch($"on return: {pd}");
            }

            // The recompiled result stands.
            _final.CopyTo(ram);
            Differential.LoadPad(mem, _finalPad);
            c.Restore(theirs);
            Report();
        }

        /// <summary>One call of ours, answered from the record at the same place in
        /// the sequence.</summary>
        public static void Take(CpuContext c, PSMemory mem, Site site)
        {
            int p = _taken++;
            if (p >= _nseq)
            {
                Mismatch($"ours called {site} after the routine's {_nseq} calls");
                return;
            }
            if (_seq[p] != (int)site)
                Mismatch($"call {p}: ours called {site} where the routine called {(Site)_seq[p]}");
            _bySite[_seq[p]]++;

            var now = c.Snapshot();
            if (Registers(now, _entry[p]) is { } r) Mismatch($"entering {site}: {r}");
            var ram = Differential.Ram(mem);
            if (_pollsIn[p] != (p > 0 ? _pollsOut[p - 1] : _pollsStart)) _interrupted++;
            else
            {
                if (Differential.Describe(ram, _entryRam[p], 0, ram.Length) is { } diff)
                    Mismatch($"entering {site}: {diff}");
                Differential.SavePad(mem, _padNow);
                if (Differential.DescribePad(_padNow, _entryPad[p]) is { } pd) Mismatch($"entering {site}: {pd}");
            }

            c.Restore(_exit[p]);
            _exitRam[p].CopyTo(ram);
            Differential.LoadPad(mem, _exitPad[p]);
        }

        /// <summary>Every general register with LO and HI, or null when they agree.</summary>
        static string? Registers(in CpuSnapshot ours, in CpuSnapshot theirs)
        {
            var o = Fields(ours);
            var t = Fields(theirs);
            for (int i = 0; i < o.Length; i++)
                if (o[i] != t[i]) return $"{Names[i]} recompiled {t[i]:X8} ours {o[i]:X8}";
            return null;
        }

        static readonly string[] Names =
            ["at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "gp", "sp", "fp", "ra", "lo", "hi"];

        static uint[] Fields(in CpuSnapshot s) =>
            [s.At, s.V0, s.V1, s.A0, s.A1, s.A2, s.A3, s.T0, s.T1, s.T2, s.T3, s.T4, s.T5, s.T6, s.T7,
             s.S0, s.S1, s.S2, s.S3, s.S4, s.S5, s.S6, s.S7, s.T8, s.T9, s.GP, s.SP, s.FP, s.RA, s.LO, s.HI];

        public static void Enter(CpuContext c, IMemory m)
        {
            if (!Recording) return;
            if (_open >= 0) { _nested++; return; }
            int j = Array.FindIndex(Calls, k => k.Return == c.RA);
            if (j < 0) { _stray++; return; }
            if (_nseq >= MaxCalls) { _overflow++; return; }
            int p = _nseq++;
            _seq[p] = j;
            _entry[p] = c.Snapshot();
            _pollsIn[p] = RecompOne.Runtime.Interrupts.SlowPolls;
            Differential.Ram((PSMemory)m).CopyTo(_entryRam[p]);
            Differential.SavePad((PSMemory)m, _entryPad[p]);
            _open = p;
        }

        public static void Exit(CpuContext c, IMemory m)
        {
            if (!Recording) return;
            if (_nested > 0) { _nested--; return; }
            if (_open < 0) return;
            int p = _open;
            _exit[p] = c.Snapshot();
            _pollsOut[p] = RecompOne.Runtime.Interrupts.SlowPolls;
            Differential.Ram((PSMemory)m).CopyTo(_exitRam[p]);
            Differential.SavePad((PSMemory)m, _exitPad[p]);
            _open = -1;
        }

        static bool Hook()
        {
            var enter = typeof(Verifier).GetMethod(nameof(Enter), BindingFlags.Public | BindingFlags.Static)!;
            var exit = typeof(Verifier).GetMethod(nameof(Exit), BindingFlags.Public | BindingFlags.Static)!;
            var targets = new List<MethodInfo>();
            foreach (var callee in Calls.Select(k => k.Callee).Distinct())
            {
                var mi = SymbolRegistry.Resolve("game", null, callee);
                if (mi == null)
                {
                    Console.WriteLine("[KF1] renderer verify: a callee is not mapped; not verifying");
                    return false;
                }
                targets.Add(mi);
            }
            // Outermost on both sides, so the record holds what every other hook did.
            foreach (var t in targets)
            {
                HookManager.AddPre(_self, t, enter, int.MinValue);
                HookManager.AddPost(_self, t, exit, int.MaxValue);
            }
            HookManager.Commit();
            bool ok = targets.All(HookAttach.Installed);
            Console.WriteLine(ok ? "[KF1] renderer verify: recording every call"
                                 : "[KF1] renderer verify: a callee could not be hooked; not verifying");
            return ok;
        }

        static void Mismatch(string what)
        {
            _bad++;
            if (_samples.Count < 8) _samples.Add(what);
        }

        static void Report()
        {
            double now = Environment.TickCount64 / 1000.0;
            if (now < _reportAt) return;
            _reportAt = now + 2.0;
            var sites = string.Join(", ", Enumerable.Range(0, Calls.Length).Where(i => _bySite[i] > 0)
                .Select(i => $"{(Site)i} {_bySite[i]}"));
            Console.WriteLine($"[KF1] verify renderer: {_calls} frame(s), {_bad} mismatch(es), " +
                              $"{_overflow} call(s) past the record, {_stray} stray, {_interrupted} stretch(es) interrupted; calls compared: {sites}");
            foreach (var s in _samples) Console.WriteLine($"[KF1]   {s}");
            _samples.Clear();
            _calls = _bad = _overflow = _stray = _interrupted = 0;
            Array.Clear(_bySite);
        }
    }
}
