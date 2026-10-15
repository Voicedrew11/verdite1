using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001ED38</c>, the bank model, transcribed literally. <c>KF1_BANKMODEL</c>.
/// Takes no arguments. It sets the geometry screen to 200, selects TMD 4
/// (<c>func_8001C0E8</c>), takes its first object (<c>func_8001C114</c>), projects
/// that object's vertices (<c>func_8001C60C</c>) and draws its polygons at depth
/// 1000 (<c>func_8001C7F8</c>).
/// </summary>
public static class BankModel
{
    public static readonly Literal R = new(typeof(BankModel), "bank model", "BANKMODEL", 0x8001ED38, Run,
        "Draws TMD 4's first object at depth +1000, for the interaction scenes.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x18u;
        mem.WriteU32(c.SP + 0x10u, c.RA);
        c.A0 = 0u;
        c.RA = 0x8001ED48u;
        Dispatcher.Call(c, mem, 0x8001BAB8u);
        c.A0 = 0u | 0x00C8u;
        c.RA = 0x8001ED50u;
        KingsField1_game.SetGeomScreen_game(c, mem);
        c.A0 = 0u | 0x0004u;
        c.RA = 0x8001ED58u;
        KingsField1_game.func_8001C0E8(c, mem);
        c.A0 = 0u;
        c.RA = 0x8001ED60u;
        KingsField1_game.func_8001C148(c, mem);
        c.A0 = 0u;
        c.RA = 0x8001ED68u;
        KingsField1_game.func_8001C114(c, mem);
        c.A0 = mem.ReadU32(c.V0 + 0x4u);
        c.RA = 0x8001ED74u;
        KingsField1_game.func_8001C60C(c, mem);
        c.A0 = 0u;
        c.A1 = 0u | 0x03E8u;
        c.RA = 0x8001ED80u;
        KingsField1_game.func_8001C7F8(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x18u;
        return;
    }
}
