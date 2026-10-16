using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// <c>KF1_GPU_CHECK=1</c> under <c>capture</c>: the retained models' light, as the world
/// shader works it out (the corner's normal through the instance's LLM, BK + LCM, times
/// the RGBC, kept by the cue at the cache's IR0), against the colour the GTE wrote to
/// the packet, at every corner the polygon writer lights. The writer lights through two
/// library leaves: <c>func_8004DD40</c> (NCDT: normals at a0-a2, RGBC at a3, IR0 at
/// sp+0x10, colours to the words sp+0x14-0x1C point at) and <c>func_8004DD14</c> (NCDS:
/// normal at a0, RGBC at a1, IR0 in a2, colour to a3). The per-pixel path computes the
/// same at a corner, so this is its numerical check too. Step 4 of
/// docs/RENDERER_PLAN.md.
/// </summary>
public static class LightCheck
{
    const uint Ncdt = 0x8004DD40, Ncds = 0x8004DD14;
    static readonly ModInfo _self = new() { Id = "kf1.lightcheck", Name = "Light check", Version = "1.0" };

    static long _corners, _exact, _one, _worse;
    static int _max, _samples;

    public static void Install()
    {
        if (!GpuWorld.Check) return;
        HookAttach.OnOverlayLoad("light check", () =>
        {
            SymbolRegistry.Build();
            var t = SymbolRegistry.Resolve("game", null, Ncdt);
            var s = SymbolRegistry.Resolve("game", null, Ncds);
            if (t == null || s == null) return false;
            HookManager.AddPost(_self, t, typeof(LightCheck).GetMethod(nameof(AfterNcdt), BindingFlags.Public | BindingFlags.Static)!);
            HookManager.AddPost(_self, s, typeof(LightCheck).GetMethod(nameof(AfterNcds), BindingFlags.Public | BindingFlags.Static)!);
            HookManager.Commit();
            return HookAttach.Installed(t) && HookAttach.Installed(s);
        });
    }

    static bool Due => GpuWorld.Capture && !GpuWorld.Drawing && GpuWorld.Domain == "models";

    public static void AfterNcdt(CpuContext c, IMemory m)
    {
        if (!Due) return;
        uint rgbc = m.ReadU32(c.A3);
        int ir0 = (short)m.ReadU16(c.SP + 0x10u);
        Corner(m, c.A0, rgbc, ir0, m.ReadU32(m.ReadU32(c.SP + 0x14u)));
        Corner(m, c.A1, rgbc, ir0, m.ReadU32(m.ReadU32(c.SP + 0x18u)));
        Corner(m, c.A2, rgbc, ir0, m.ReadU32(m.ReadU32(c.SP + 0x1Cu)));
    }

    public static void AfterNcds(CpuContext c, IMemory m)
    {
        if (!Due) return;
        Corner(m, c.A0, m.ReadU32(c.A1), (short)c.A2, m.ReadU32(c.A3));
    }

    static short Lo(int r) => (short)Gte.ReadControl(r);
    static short Hi(int r) => (short)(Gte.ReadControl(r) >> 16);

    /// <summary>The world shader's model light at one corner (GlShaders' WorldVs and
    /// shade8), in floats, against the GTE's packet colour.</summary>
    static void Corner(IMemory m, uint normal, uint rgbc, int ir0, uint got)
    {
        float nx = (short)m.ReadU16(normal), ny = (short)m.ReadU16(normal + 2), nz = (short)m.ReadU16(normal + 4);
        Span<float> llm = [Lo(8), Hi(8), Lo(9), Hi(9), Lo(10), Hi(10), Lo(11), Hi(11), Lo(12)];
        Span<float> lcm = [Lo(16), Hi(16), Lo(17), Hi(17), Lo(18), Hi(18), Lo(19), Hi(19), Lo(20)];
        Span<float> bk = [(int)Gte.ReadControl(13), (int)Gte.ReadControl(14), (int)Gte.ReadControl(15)];
        Span<float> a = stackalloc float[3];
        for (int k = 0; k < 3; k++)
            a[k] = Math.Clamp((llm[k * 3] / 4096f) * nx + (llm[k * 3 + 1] / 4096f) * ny + (llm[k * 3 + 2] / 4096f) * nz, 0f, 32767f);
        // The cue: the shader's offset curve at DQB + 800 x 4096 keeps 1 - 2 IR0(RTPS) / 4096,
        // and the cache's IR0 the writer passes is that 2 IR0.
        float keep = Math.Clamp(1f - Math.Clamp(ir0, 0, 4096) / 4096f, 0f, 1f);
        int worst = 0;
        for (int k = 0; k < 3; k++)
        {
            float ir = Math.Clamp(bk[k] + (lcm[k * 3] * a[0] + lcm[k * 3 + 1] * a[1] + lcm[k * 3 + 2] * a[2]) / 4096f, 0f, 32767f);
            float lit = (rgbc >> (8 * k) & 0xFF) * ir / 4096f;
            int mine = (int)MathF.Round(Math.Clamp(lit * keep, 0f, 255f));
            int gte = (int)(got >> (8 * k) & 0xFF);
            worst = Math.Max(worst, Math.Abs(mine - gte));
        }
        _corners++;
        if (worst == 0) _exact++; else if (worst == 1) _one++; else _worse++;
        _max = Math.Max(_max, worst);
        if (worst > 2 && _samples++ < 12)
            Console.WriteLine($"[KF1] light check: normal ({nx},{ny},{nz}) rgbc {rgbc & 0xFFFFFF:X6} ir0 {ir0} gte {got & 0xFFFFFF:X6} off by {worst}");
    }

    public static string Report() =>
        $"model corners lit {_corners}: exact {_exact}, off by 1 {_one}, more {_worse} (max {_max} of 255)";
}
