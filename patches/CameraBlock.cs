using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// <c>func_8001C184(VECTOR *pos, SVECTOR *rot)</c> in C#: the renderer's first call,
/// and the one place the frame's view comes from.
///
///     KF1_CAMERABLOCK=1        this transcription (the default)
///     KF1_CAMERABLOCK=0        the recompiled routine
///     KF1_CAMERABLOCK=verify   run both on every call and compare
///
/// A non-null <c>pos</c> is copied to <see cref="Position"/> and its cell (the
/// coordinate over 2000) derived; a non-null <c>rot</c> to <see cref="Angles"/>; then
/// <c>RotMatrix</c> (<c>func_8004E9B8</c>) builds the view from the stored angles and
/// a pitch-only matrix from the pitch. Verdite3's CameraBlock on this game's block.
/// See "The renderer in C#" in docs/GAME_INTERNALS.md.
/// </summary>
public static class CameraBlock
{
    const uint Routine = 0x8001C184;
    const uint RotMatrix = 0x8004E9B8;

    /// <summary>The view matrix; the pitch-only matrix after it.</summary>
    public const uint ViewMatrix = 0x800956A0, PitchMatrix = 0x800956C0;

    /// <summary>VECTOR: X, Y, Z, and a pad word copied with them.</summary>
    public const uint Position = 0x80095744;

    /// <summary>SVECTOR: pitch, yaw, roll, and a pad halfword copied with them.</summary>
    public const uint Angles = 0x80095754;

    /// <summary>The eye's cell, <c>X / 2000</c> and <c>Z / 2000</c>, halfwords.</summary>
    public const uint CellX = 0x8009575C, CellZ = 0x8009575E;

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;

    /// <summary>The mode as the Testing tab sets it: 0 recompiled, 1 C#, 2 verify.</summary>
    public static int Setting { get => (int)_mode; set => _mode = (Mode)Math.Clamp(value, 0, 2); }
    static bool _queued;
    static Action<CpuContext, IMemory>? _rotMatrix;

    static readonly Differential _check = new("KF1", "func_8001C184", 0x400);

    static readonly ModInfo _self = new()
    {
        Id = "kf1.camerablock",
        Name = "Camera block",
        Version = "1.0",
        Description = "func_8001C184, the view the renderer draws with, in C#.",
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
        HookAttach.OnOverlayLoad("camera block", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, Routine);
        var rot = SymbolRegistry.Resolve("game", null, RotMatrix);
        if (target == null || rot == null) return false;
        _rotMatrix = rot.CreateDelegate<Action<CpuContext, IMemory>>();
        if (!_queued)
        {
            var impl = typeof(CameraBlock).GetMethod(nameof(Replace), BindingFlags.NonPublic | BindingFlags.Static)!;
            _queued = HookManager.AddReplace(_self, target, impl);
            if (!_queued) return false;
        }
        HookManager.Commit();
        bool ok = HookAttach.Installed(target);
        Console.WriteLine(ok ? $"[KF1] camera block: {_mode.ToString().ToLowerInvariant()}"
                             : "[KF1] camera block: not installed");
        return ok;
    }

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        // PGXP's RAM shadow is kept by the recompiled stores, which C# stores skip.
        if (_mode == Mode.Off || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking || m is not PSMemory mem || _rotMatrix == null)
        {
            orig(c, m);
            return;
        }
        if (_mode == Mode.Verify) _check.Run(orig, c, mem, Run);
        else Run(c, mem);
    }

    /// <summary>The camera the block holds: the one the last frame was drawn with.</summary>
    public static Camera Read(IMemory m) => Camera.Read(m, Position, Angles);

    /// <summary>Write a camera into the block as the routine's two copies would, the
    /// pad fields left alone. The view is not rebuilt; <see cref="Build"/> does that.</summary>
    public static void Store(IMemory m, in Camera cam)
    {
        cam.Write(m, Position, Angles);
        m.WriteU16(CellX, (ushort)(cam.X / 2000));
        m.WriteU16(CellZ, (ushort)(cam.Z / 2000));
    }

    /// <summary>Make <paramref name="cam"/> the view: store it and rebuild the matrices
    /// through the routine, so every hook on it sees the call.</summary>
    public static void Build(CpuContext c, IMemory m, in Camera cam, Action<CpuContext, IMemory> routine)
    {
        Store(m, cam);
        c.A0 = 0u;
        c.A1 = 0u;
        routine(c, m);
    }

    /// <summary>The routine, transcribed register for register.</summary>
    static void Run(CpuContext c, PSMemory mem)
    {
        uint sp = c.SP - 0x20u;
        c.SP = sp;
        mem.WriteU32(sp + 0x18u, c.RA);
        c.A3 = c.A0;
        c.T0 = c.A1;

        if (c.A3 != 0u)
        {
            c.V0 = Position;
            c.V1 = mem.ReadU32(c.A3);
            c.A0 = mem.ReadU32(c.A3 + 4u);
            c.A1 = mem.ReadU32(c.A3 + 8u);
            c.A2 = mem.ReadU32(c.A3 + 12u);
            mem.WriteU32(c.V0, c.V1);
            mem.WriteU32(c.V0 + 4u, c.A0);
            mem.WriteU32(c.V0 + 8u, c.A1);
            mem.WriteU32(c.V0 + 12u, c.A2);
            c.A0 = mem.ReadU32(c.V0);
            c.V0 = 2000u;
            Renderer.Div(c, c.A0, c.V0);
            c.A0 = c.LO;
            c.V1 = mem.ReadU32(Position + 8u);
            Renderer.Div(c, c.V1, c.V0);
            c.V1 = c.LO;
            c.At = 0x80090000u;
            mem.WriteU16(CellX, (ushort)c.A0);
            mem.WriteU16(CellZ, (ushort)c.V1);
        }

        if (c.T0 != 0u)
        {
            // lwl/lwr pairs: the SVECTOR need not be word-aligned.
            c.A1 = Angles;
            c.V0 = mem.ReadWordLeft(c.V0, c.T0 + 3u);
            c.V0 = mem.ReadWordRight(c.V0, c.T0);
            c.V1 = mem.ReadWordLeft(c.V1, c.T0 + 7u);
            c.V1 = mem.ReadWordRight(c.V1, c.T0 + 4u);
            mem.WriteWordLeft(c.A1 + 3u, c.V0);
            mem.WriteWordRight(c.A1, c.V0);
            mem.WriteWordLeft(c.A1 + 7u, c.V1);
            mem.WriteWordRight(c.A1 + 4u, c.V1);
        }

        c.A0 = Angles;
        c.A1 = ViewMatrix;
        c.RA = 0x8001C27Cu;
        _rotMatrix!(c, mem);

        // The pitch alone: an SVECTOR (pitch, 0, 0) on the stack.
        mem.WriteU16(sp + 0x14u, 0);
        mem.WriteU16(sp + 0x12u, 0);
        c.A1 = Angles;
        c.V0 = mem.ReadU16(c.A1);
        c.A0 = sp + 0x10u;
        c.A1 = PitchMatrix;
        mem.WriteU16(sp + 0x10u, (ushort)c.V0);
        c.RA = 0x8001C2A0u;
        _rotMatrix!(c, mem);

        c.RA = mem.ReadU32(sp + 0x18u);
        c.SP = sp + 0x20u;
    }
}
